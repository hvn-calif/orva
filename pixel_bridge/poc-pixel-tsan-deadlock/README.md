# pixel_bridge -- `format_panic` backtrace-capture deadlocks

**Finding:** `pixel_bridge::image::format_panic` calls
`std::backtrace::Backtrace::capture()` on the panic-recovery path
(`pixel_bridge/rust/image.rs:295`), which runs on **every** malformed image that
panics the `image` crate (see the `b/372147306` comment on
`run_catching_panics`). Capturing a backtrace unwinds the stack via
`_Unwind_Backtrace -> _Unwind_Find_FDE -> dl_iterate_phdr` and holds a
process-global lock while allocating. That single line is the trigger for **two
distinct deadlocks**; decoding attacker-controlled input becomes a denial of
service in either environment.

- **Mechanism 1 (rust#130187) -- `src/main.rs`.** `Backtrace::capture()` holds
  the **non-reentrant** `std::sys::backtrace::lock` across the whole unwind, and
  the unwind allocates. If any allocation performed while the lock is held itself
  captures a backtrace (heap-sampling allocator / heaptrack / tracing shim), the
  same thread re-acquires the same lock and **hangs forever**. Reproduced
  deterministically here; gdb stack below.

- **Mechanism 2 (TSAN `dl_iterate_phdr` interceptor) -- `src/dlopen_loi.rs`.**
  Under ThreadSanitizer, `Backtrace::capture()`'s `dl_iterate_phdr` and any
  concurrent `dlopen`/`dlclose` acquire the **loader lock** and TSAN's **internal
  lock** in opposite orders -> lock-order inversion. Reproduced here with the
  reporter's exact recipe (one thread capturing, one thread `dlopen`ing) under
  normal `cargo` + TSAN; gdb shows both threads parked on the two locks.

Both are on the toolchain in this repo (`nightly-2026-05-19`, aarch64). Both are
fixed by the same one-line change: don't capture a backtrace in the recovery
path (it removes the `_Unwind_Backtrace -> dl_iterate_phdr` call entirely).

## Vulnerable code (verbatim)

```rust
// pixel_bridge/rust/image.rs
fn format_panic(err: Box<dyn std::any::Any + Send>) -> String {
    let backtrace = std::backtrace::Backtrace::capture();   // line 295  <-- deadlocks
    ...
    format!("Rust panic caught: {}\nBacktrace:\n{}", msg, backtrace)
}

fn run_catching_panics<F, T>(f: F) -> Result<T, String> {
    // b/372147306 - The image crate can panic on malformed input.
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(f)) {
        Ok(res) => Ok(res),
        Err(err) => Err(format_panic(err)),                 // <-- every caught panic
    }
}
```

## What triggers the recursion

The lock is only re-entered if an allocation *during the capture* itself
captures a backtrace. That extra capture comes from a process component that
records an origin stack per allocation:

- a **heap profiler / sampling allocator** (e.g. TCMalloc heap sampling, common
  in Google server binaries), or
- an **allocation tracer** (heaptrack), or
- a **tracing / sanitizer allocator shim** configured to record allocation
  origins.

`AllocProfiler` in `src/main.rs` is a minimal, re-entrancy-guarded model of
exactly that class of allocator. Everything else in the PoC
(`format_panic` / `run_catching_panics`) is a byte-for-byte copy of the
pixel_bridge shapes.

## Result (reproduced on this host)

`suite` builds a normal (non-sanitizer) binary. `tsan` rebuilds it under
ThreadSanitizer with an instrumented std (`-Zsanitizer=thread -Zbuild-std`).
Exit code `124` from `timeout` means the process was still running == deadlock.

| Mode | Build | `format_panic` captures? | Sampling allocator? | Outcome |
|---|---|---|---|---|
| `safe`     | plain | no  | armed | completes, `rc=0` |
| `deadlock` | plain | yes | armed | **HANG** (`timeout` -> exit 124) |
| `safe`     | tsan  | no  | armed | completes, `rc=0` |
| `deadlock` | tsan  | yes | armed | **HANG** (`timeout` -> exit 124) |

Captured stuck stack (the whole point of the finding) -- one thread, the
non-reentrant lock taken twice:

```
#1  futex_wait                                              <-- blocked forever
...
#5  std::sys::backtrace::lock         backtrace.rs:24       <-- 2nd acquire (same thread)
#6  std::backtrace::Backtrace::create backtrace.rs:326
#7  ...::AllocProfiler::alloc         src/main.rs:64        <-- allocation captures a backtrace
...
#9  alloc::alloc::alloc                                     <-- allocation WHILE lock is held
...
#17 alloc::vec::Vec::push<BacktraceFrame>                   <-- capture is collecting frames
...
#22 _Unwind_Backtrace (libgcc_s)
#25 std::backtrace::Backtrace::create backtrace.rs:331      <-- 1st acquire holds the lock
#26 ...::format_panic (capture=true)  src/main.rs:82        == pixel_bridge image.rs:295
#27 ...::run_catching_panics          src/main.rs:105       == pixel_bridge image.rs:308
#28 ...::main
```

## Mechanism 2 -- TSAN `dl_iterate_phdr` <-> loader-lock inversion (`src/dlopen_loi.rs`)

Recipe (the reporter's): thread group 1 loops `Backtrace::force_capture()`
(== image.rs:295), thread group 2 loops `dlopen("libm.so.6")` + `dlclose`.
Built under normal `cargo` + `-Zsanitizer=thread`.

Two locks: **L** = glibc dynamic-loader lock (taken by both `dl_iterate_phdr`
and `dlopen`/`dlclose`); **T** = TSAN's internal `LibIgnore` lock
(`sanitizer_mutex`, taken by TSAN's interceptors). The threads acquire them in
opposite orders. Real gdb frames captured mid-collapse (verbatim; full dump in
`mechanism2-gdb-stacks.txt`). Frame paths say `src/main.rs` because they are from
the scratchpad reproducer crate; `actor_backtrace`/`actor_dlopen` are the
functions committed here as `src/dlopen_loi.rs`:

**(A) Capture side** -- this is the chain that is *not* visible in std's
`backtrace.rs` (which only shows `lock()`): `create()` drives the libgcc
unwinder, which calls `dl_iterate_phdr` per frame and wants **L**.

```
#2  _Unwind_Backtrace ()                              from libgcc_s.so.1   <-- calls _Unwind_Find_FDE -> dl_iterate_phdr (wants L)
#3  std::backtrace_rs::backtrace::libunwind::trace    libunwind.rs:117
#4  std::backtrace_rs::backtrace::trace_unsynchronized mod.rs:66
#5  std::backtrace::Backtrace::create ()              backtrace.rs:331
#6  ...::actor_backtrace ()                           src/main.rs:14       == pixel_bridge image.rs:295
```

**(B) dlopen side, holds T, wants L** -- TSAN's `dlclose` interceptor takes the
`LibIgnore` lock **T**, then itself calls `dl_iterate_phdr` (to refresh its
module map) which wants **L**.  order: `T -> L`

```
#5  dl_iterate_phdr ()                       from libc.so.6                     <-- wants loader lock L
#6  ___interceptor_dl_iterate_phdr ()        tsan_interceptors_posix.cpp:2512
#7  init ()                                  sanitizer_linux_libcdep.cpp:789
#8  __sanitizer::LibIgnore::OnLibraryLoaded  sanitizer_libignore.cpp:54         <-- holds TSAN lock T
#9  ___interceptor_dlclose ()                sanitizer_common_interceptors.inc:6550
#10 ...::actor_dlopen ()                     src/main.rs:23
```

**(C) dlopen side, holds L-state, blocked wanting T** -- another `dlclose`
thread parked acquiring **T** while inside the real `dlclose`.  order: `L -> T`

```
#0  FutexWait ()                             sanitizer_linux.cpp:832            <-- blocked on TSAN lock T
#2  Lock ()                                  sanitizer_mutex.h:196
#3  GenericScopedLock ()                     sanitizer_mutex.h:383
#4  __sanitizer::LibIgnore::OnLibraryLoaded  sanitizer_libignore.cpp:39
#5  ___interceptor_dlclose ()                sanitizer_common_interceptors.inc:6550
#6  ...::actor_dlopen ()                     src/main.rs:23
```

`L <-> T` taken in opposite orders across threads == lock-order inversion.
Observed: capture throughput dropped from ~32,000 / 500ms to `bt=+4..+7` (and
momentary hard `bt=+0`) for a sustained window -- a >99.99% collapse. On this
host's glibc the loader lock is recursive, so the threads livelock (rare
progress) rather than freezing permanently; on a non-recursive loader path it is
a permanent hang. Either way it is a denial of service.

## Honest scope

- **Mechanism 1 (rust#130187): confirmed, deterministic.** The stuck stack above
  pins the reentrancy to the `Backtrace::capture()` line. Requires a
  backtrace-capturing allocator in the process (heap sampler / heaptrack /
  tracing shim); `AllocProfiler` models that class. Permanent hang.
- **Mechanism 2 (TSAN `dl_iterate_phdr`): confirmed as a >99.99% throughput
  collapse / near-deadlock on this host.** Requires TSAN + concurrent `dlopen`
  at runtime (common: codec plugins, NSS, ICU). gdb shows both lock orders
  parked simultaneously. Earlier, TSAN *without* concurrent `dlopen` did not
  reproduce -- the `dlopen` churn is the missing ingredient that forces TSAN to
  take its internal lock under the loader lock.
- **Common cause, common fix.** Both fire from the same `_Unwind_Backtrace ->
  dl_iterate_phdr` unwind that `Backtrace::capture()` performs. All of this is a
  **denial of service**, not memory corruption.
- The recursion guard (`SAMPLING`) in `src/main.rs` exists so Mechanism 1
  deadlocks against the *format_panic* capture rather than racing inside the
  profiler; a real profiler hits the same window nondeterministically under load.

## Fix direction

Do not capture a backtrace inside the `catch_unwind` recovery path. The applied
patch to `pixel_bridge/rust/image.rs` drops the `Backtrace::capture()` line from
`format_panic` (keeping the `"Rust panic caught: {msg}"` string the C++ side
keys on). That removes the `_Unwind_Backtrace -> dl_iterate_phdr` call, killing
both mechanisms. A per-thread reentrancy guard does **not** help Mechanism 1
(the recursion is inside a foreign allocator that ignores our guard) and does
not help Mechanism 2 (the second lock holder is TSAN's dlopen interceptor).
Caveat: the **default panic hook** also unwinds via `dl_iterate_phdr` when
`RUST_BACKTRACE` is set, so a fully clean deployment should also unset
`RUST_BACKTRACE` or install a non-capturing hook. Upstream: rust-lang/rust#130187,
LLVM D67738, tcmalloc#120.

## Run

```
./run.sh              # Mechanism 1 suite: safe completes, deadlock hangs, prints stuck stack
./run.sh deadlock     # Mechanism 1 only: the hanging case + gdb stack dump
./run.sh safe         # Mechanism 1 only: the completing case (== the fix)
./run.sh tsan         # Mechanism 1 under ThreadSanitizer
./run.sh dlopen       # Mechanism 2: TSAN dl_iterate_phdr <-> loader-lock collapse
TIMEOUT=8 ./run.sh    # shorten the Mechanism 1 hang timeout
SECS=40 ./run.sh dlopen   # run Mechanism 2 longer
```

Requires the `nightly-2026-05-19` toolchain with `rust-src` (for `-Zbuild-std`).
Override with `TC=<toolchain> ./run.sh`.
