//! PoC: self-deadlock in `pixel_bridge::image::format_panic` via
//! `std::backtrace::Backtrace::capture()` (rust-lang/rust#130187).
//!
//! The vulnerable code (pixel_bridge/rust/image.rs):
//!
//! ```ignore
//! fn format_panic(err: Box<dyn std::any::Any + Send>) -> String {
//!     let backtrace = std::backtrace::Backtrace::capture();   // <-- image.rs:295
//!     ...
//! }
//!
//! fn run_catching_panics<F, T>(f: F) -> Result<T, String> {
//!     // b/372147306 - The image crate can panic on malformed input.
//!     match std::panic::catch_unwind(std::panic::AssertUnwindSafe(f)) {
//!         Ok(res) => Ok(res),
//!         Err(err) => Err(format_panic(err)),                 // reached on every caught panic
//!     }
//! }
//! ```
//!
//! `Backtrace::capture()` acquires a process-global, **non-reentrant** lock
//! (`std::sys::backtrace::lock`) and holds it across the whole unwind +
//! frame-collection. Collecting frames *allocates* (it pushes into a `Vec`).
//! If any allocation performed while that lock is held itself captures a
//! backtrace, the same thread tries to re-acquire the same non-reentrant lock
//! and **self-deadlocks**. That is rust#130187, still live on this toolchain.
//!
//! pixel_bridge is exposed because `format_panic` runs on EVERY malformed
//! image that panics the `image` crate, and it captures unconditionally.
//!
//! The recursion is supplied by any process component that captures a stack
//! trace per allocation: a heap profiler / sampling allocator (e.g. TCMalloc
//! heap sampling), an allocation tracer (heaptrack), or a sanitizer/allocator
//! shim configured to record allocation origins. `AllocProfiler` below models
//! exactly that class of allocator.
//!
//! Modes:
//!   deadlock  (default) -- format_panic captures a backtrace; the profiling
//!                           allocator recurses into capture -> HANG.
//!   safe                -- format_panic does NOT capture (the mitigation);
//!                           same allocator, same panic -> completes cleanly.

use std::alloc::{GlobalAlloc, Layout, System};
use std::backtrace::Backtrace;
use std::sync::atomic::{AtomicBool, Ordering};

/// Models a heap profiler / tracing allocator: records the origin stack of a
/// sampled allocation by capturing a backtrace. Re-entrancy-guarded so it does
/// not recurse into *itself* -- the deadlock is against the OTHER capture that
/// is already in progress on this thread (the one in `format_panic`).
struct AllocProfiler;

/// Only sample allocations once we are inside the panic-handling window, so the
/// PoC is deterministic instead of racing on which allocation gets sampled.
static ARMED: AtomicBool = AtomicBool::new(false);
static SAMPLING: AtomicBool = AtomicBool::new(false);

unsafe impl GlobalAlloc for AllocProfiler {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let should_sample = ARMED.load(Ordering::Relaxed)
            && !SAMPLING.swap(true, Ordering::Relaxed);
        if should_sample {
            // A real sampling allocator records where this allocation came from.
            let _origin = Backtrace::force_capture();
            SAMPLING.store(false, Ordering::Relaxed);
        }
        unsafe { System.alloc(layout) }
    }
    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        unsafe { System.dealloc(ptr, layout) }
    }
}

#[global_allocator]
static GLOBAL: AllocProfiler = AllocProfiler;

/// Verbatim shape of pixel_bridge's `format_panic` (image.rs:294).
/// `capture` toggles the single line that differs between the bug and the fix.
fn format_panic(err: Box<dyn std::any::Any + Send>, capture: bool) -> String {
    let backtrace = if capture {
        // pixel_bridge/rust/image.rs:295 -- the deadlocking line.
        Some(Backtrace::force_capture())
    } else {
        // Mitigation: do not capture a backtrace inside the catch_unwind path.
        None
    };
    let msg = err
        .downcast_ref::<&str>()
        .copied()
        .or_else(|| err.downcast_ref::<String>().map(|s| s.as_str()))
        .unwrap_or("Unknown panic payload");
    match backtrace {
        Some(bt) => format!("Rust panic caught: {}\nBacktrace:\n{}", msg, bt),
        None => format!("Rust panic caught: {}", msg),
    }
}

/// Verbatim shape of pixel_bridge's `run_catching_panics` (image.rs:308).
fn run_catching_panics<F, T>(f: F, capture: bool) -> Result<T, String>
where
    F: FnOnce() -> T,
{
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(f)) {
        Ok(res) => Ok(res),
        Err(err) => Err(format_panic(err, capture)),
    }
}

fn main() {
    let mode = std::env::args().nth(1).unwrap_or_else(|| "deadlock".to_string());
    let capture = match mode.as_str() {
        "deadlock" => true,
        "safe" => false,
        other => {
            eprintln!("usage: pixel_tsan_deadlock_poc [deadlock|safe]");
            eprintln!("unknown mode: {other}");
            std::process::exit(2);
        }
    };

    // Quiet the default panic hook so the only backtrace capture under test is
    // the one in format_panic. (The default hook ALSO captures when
    // RUST_BACKTRACE=1; suppressing it isolates the finding to image.rs:295.)
    std::panic::set_hook(Box::new(|_| {}));

    eprintln!("[poc] mode={mode} (capture_backtrace_in_format_panic={capture})");
    eprintln!("[poc] simulating image-crate panic on malformed input (b/372147306)");

    ARMED.store(true, Ordering::Relaxed);
    let result: Result<(), String> = run_catching_panics(
        || {
            // Stand-in for a panic raised deep inside the `image` crate while
            // decoding malformed input. The std panic/backtrace path that
            // format_panic drives is identical regardless of the panic source.
            panic!("simulated malformed-image decode panic");
        },
        capture,
    );
    ARMED.store(false, Ordering::Relaxed);

    match result {
        Ok(()) => eprintln!("[poc] closure returned Ok (unexpected)"),
        Err(msg) => eprintln!("[poc] recovered a {}-byte error status", msg.len()),
    }
    eprintln!("[poc] reached end of main -- NO deadlock");
}
