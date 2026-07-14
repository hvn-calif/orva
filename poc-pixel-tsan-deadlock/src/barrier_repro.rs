//! The reviewer's *actual* Mechanism-2 repro, as a runnable binary so you can
//! test it yourself under gdb.
//!
//! 10 threads are released *at the same instant* by a Barrier, and each one runs
//! `catch_unwind(|| { Backtrace::capture(); panic!() })`. There is NO dlopen
//! here -- this is exactly the shape the reviewer said reproduces a TSAN hang.
//!
//! A heartbeat thread prints the completed-round count twice a second. That is
//! your hang detector:
//!   * number keeps climbing  -> NOT hung (this is what happens on my TSANs).
//!   * number frozen for a while, process still alive -> candidate HANG; attach
//!     gdb (see TUTORIAL.md) and look at where every thread is stuck.
//!
//! Build + run: `./run.sh barrier`  (see TUTORIAL.md for the manual commands).

use std::backtrace::Backtrace;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Barrier};
use std::thread;
use std::time::Duration;

static ROUND: AtomicU64 = AtomicU64::new(0);

fn main() {
    // Silence the per-panic output so the heartbeat is readable. The explicit
    // `Backtrace::capture()` below is the thing under test. To also exercise the
    // default panic hook's own backtrace, delete this line and run with
    // RUST_BACKTRACE=1 (output will be very noisy).
    std::panic::set_hook(Box::new(|_| {}));

    let threads: usize = env_usize("THREADS", 10);
    let rounds: u64 = env_u64("ROUNDS", u64::MAX);

    // Heartbeat / hang detector.
    thread::spawn(|| loop {
        thread::sleep(Duration::from_millis(500));
        eprintln!("[barrier] completed rounds = {}", ROUND.load(Ordering::Relaxed));
    });

    eprintln!("[barrier] {threads} threads, capture()+panic!() per round, barrier-synced");
    for r in 0..rounds {
        let barrier = Arc::new(Barrier::new(threads));
        let mut handles = Vec::with_capacity(threads);
        for _ in 0..threads {
            let b = barrier.clone();
            handles.push(thread::spawn(move || {
                b.wait(); // all `threads` threads leave this line together
                let _ = catch_unwind(AssertUnwindSafe(|| {
                    let _ = Backtrace::force_capture(); // the risky primitive
                    panic!("boom");
                }));
            }));
        }
        for h in handles {
            h.join().unwrap();
        }
        ROUND.store(r + 1, Ordering::Relaxed);
    }
    eprintln!("[barrier] finished all rounds with NO hang");
}

fn env_usize(k: &str, d: usize) -> usize {
    std::env::var(k).ok().and_then(|s| s.parse().ok()).unwrap_or(d)
}
fn env_u64(k: &str, d: u64) -> u64 {
    std::env::var(k).ok().and_then(|s| s.parse().ok()).unwrap_or(d)
}
