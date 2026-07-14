//! PoC (Mechanism 2): TSAN `dl_iterate_phdr` interceptor <-> dynamic-loader-lock
//! lock-order inversion, triggered by `pixel_bridge::image::format_panic`'s
//! `std::backtrace::Backtrace::capture()` (image.rs:295) when the process also
//! `dlopen`s libraries at runtime.
//!
//! This is a DIFFERENT deadlock from src/main.rs (rust#130187). Same trigger
//! line, different lock cycle. Build under ThreadSanitizer.
//!
//! The cycle (confirmed by gdb; see README):
//!
//!   thread A (Backtrace::capture): _Unwind_Backtrace -> _Unwind_Find_FDE
//!       -> TSAN's ___interceptor_dl_iterate_phdr grabs a TSAN internal lock,
//!          then calls real dl_iterate_phdr which wants the loader lock.
//!       order:  TSAN-lock -> loader-lock
//!
//!   thread B (dlopen/dlclose): real dl{open,close} takes the loader lock,
//!       then TSAN's ___interceptor_dl{open,close} -> LibIgnore::OnLibraryLoaded
//!       -> GenericScopedLock wants the SAME TSAN internal lock.
//!       order:  loader-lock -> TSAN-lock
//!
//!   Opposite orders across the two threads => deadlock.
//!
//! On glibc where the loader lock is recursive (e.g. this host) the two threads
//! livelock: throughput collapses by >99.99% with momentary hard freezes,
//! rather than hanging permanently forever. On runtimes where the loader path
//! is not recursive it is a permanent hang. Either way it is a denial of
//! service, and the fix is identical: don't call Backtrace::capture() in the
//! panic-recovery path (see src/main.rs / the applied image.rs patch).

use std::backtrace::Backtrace;
use std::ffi::CString;
use std::os::raw::c_int;
use std::sync::atomic::{AtomicU64, Ordering};

const RTLD_NOW: c_int = 2; // RTLD_LOCAL is the default (no RTLD_GLOBAL) -> real unload on dlclose

static BT: AtomicU64 = AtomicU64::new(0);
static DL: AtomicU64 = AtomicU64::new(0);

// pixel_bridge format_panic path: catch a panic, then capture a backtrace,
// which unwinds via _Unwind_Backtrace -> _Unwind_Find_FDE -> dl_iterate_phdr.
fn actor_backtrace() {
    loop {
        let _ = std::panic::catch_unwind(|| panic!("boom"));
        let _bt = Backtrace::force_capture(); // == image.rs:295
        BT.fetch_add(1, Ordering::Relaxed);
    }
}

// Any code that dlopen's at runtime (codec plugins, NSS, ICU, etc.).
fn actor_dlopen(name: CString) {
    loop {
        unsafe {
            let h = libc::dlopen(name.as_ptr(), RTLD_NOW);
            if !h.is_null() {
                libc::dlclose(h);
            }
        }
        DL.fetch_add(1, Ordering::Relaxed);
    }
}

fn main() {
    std::panic::set_hook(Box::new(|_| {}));
    let lib = std::env::var("LIB").unwrap_or_else(|_| "libm.so.6".into());
    let nbt: usize = std::env::var("BT").ok().and_then(|s| s.parse().ok()).unwrap_or(6);
    let ndl: usize = std::env::var("DL").ok().and_then(|s| s.parse().ok()).unwrap_or(6);
    let secs: u64 = std::env::var("SECS").ok().and_then(|s| s.parse().ok()).unwrap_or(30);

    eprintln!("[dlopen_loi] lib={lib} backtrace_threads={nbt} dlopen_threads={ndl}");
    eprintln!("[dlopen_loi] baseline throughput is ~tens-of-thousands of captures/500ms;");
    eprintln!("[dlopen_loi] a collapse to single digits (or 0) is the lock-order inversion.");

    for _ in 0..nbt {
        std::thread::spawn(actor_backtrace);
    }
    let name = CString::new(lib).unwrap();
    for _ in 0..ndl {
        let n = name.clone();
        std::thread::spawn(move || actor_dlopen(n));
    }

    let (mut lb, mut ld) = (0u64, 0u64);
    let mut collapses = 0u64;
    for _ in 0..(secs * 2) {
        std::thread::sleep(std::time::Duration::from_millis(500));
        let (b, d) = (BT.load(Ordering::Relaxed), DL.load(Ordering::Relaxed));
        let (db, dd) = (b - lb, d - ld);
        // A >99% drop from a healthy rate is the inversion biting.
        let collapsed = db < 100;
        if collapsed {
            collapses += 1;
        }
        eprintln!(
            "[dlopen_loi] bt=+{db} dl=+{dd}{}",
            if collapsed { "   *** THROUGHPUT COLLAPSE (near-deadlock) ***" } else { "" }
        );
        lb = b;
        ld = d;
    }
    eprintln!("[dlopen_loi] collapsed intervals: {collapses}/{} (any >0 == the LOI reproduced)", secs * 2);
}
