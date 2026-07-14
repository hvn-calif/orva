# pixel_bridge -- `format_panic` backtrace-capture deadlocks

**Finding:** `pixel_bridge::image::format_panic` calls
`std::backtrace::Backtrace::capture()` on the panic-recovery path
(`pixel_bridge/rust/image.rs:295`), which runs on **every** malformed image that
panics the `image` crate (see the `b/372147306` comment on
`run_catching_panics`). Capturing a backtrace unwinds the stack via
`_Unwind_Backtrace -> _Unwind_Find_FDE -> dl_iterate_phdr` and holds a
process-global lock while allocating. That single line is the shared trigger for
**two candidate deadlocks** with different status (see "Net assessment"):
Mechanism 1 (prod hang) needs a *Rust* backtrace-capturing allocator, so it does
**not** fire under TCMalloc; Mechanism 2 (a TSAN deadlock reported by a reviewer)
is **UNVERIFIED here** -- I could not reproduce it as a hang on any TSAN I have,
including with the reviewer's own barrier+capture+panic repro, and I cannot test
the `ScopedIgnoreInterceptors` guard. The fix is cheap defensive hardening, not a
fix for a bug independently confirmed in this PoC.

- **Mechanism 1 (rust#130187) -- `src/main.rs`.** `Backtrace::capture()` holds
  the **non-reentrant** `std::sys::backtrace::lock` across the whole unwind, and
  the unwind allocates. If any allocation performed while the lock is held itself
  captures a **Rust** backtrace (a Rust tracing/heap-profiler global allocator),
  the same thread re-acquires the same lock and **hangs forever**. Reproduced
  deterministically here; gdb stack below. NOTE: **TCMalloc does not trigger
  this** -- its C++ lock-free unwinder never touches the Rust lock (see below).

- **Mechanism 2 (TSAN, reviewer-reported) -- UNVERIFIED.** `src/dlopen_loi.rs`
  explores a `Backtrace::capture` + concurrent `dlopen` scenario and shows a
  severe but **transient** throughput collapse (recovers every time -- contention,
  not a hang). That is **NOT** the reviewer's repro and **NOT** a reproduction of
  their deadlock: their repro is 10 barrier-synced threads doing
  `catch_unwind(|| { capture(); panic!() })` with no `dlopen`, and it does **not**
  hang on either TSAN available here. Treat `src/dlopen_loi.rs` as an exploration
  artifact, not proof of a deadlock. See "Net assessment".

Both are on the toolchain in this repo (`nightly-2026-05-19`, aarch64) but not in
Google's build. The same one-line change removes the `_Unwind_Backtrace ->
dl_iterate_phdr` call entirely, so it forecloses both regardless of allocator or
TSAN version -- a cheap defensive fix, not a live-bug fix.

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

The lock is only re-entered if an allocation *during the capture* itself calls
**Rust's** `std::backtrace::Backtrace` (the only thing that takes
`std::sys::backtrace::lock`). So the partner must be a global allocator that
records an origin stack **by calling into Rust std's backtrace machinery**:

- a **Rust global allocator that captures a Rust backtrace on alloc** -- e.g.
  Rust heap-profiler crates (`dhat`, `bytehound`, tracing-allocator shims), or
- the `rust-lang/rust#130187` condition (default panic hook + test output
  capturing + such an allocator).

**TCMalloc does NOT qualify, and this is the key limitation of Mechanism 1.**
TCMalloc is C++: its heap-sampling path unwinds with its own lock-free C++
stack walker (`absl::GetStackTrace`, raw PCs, no symbolization at capture) and
**never touches Rust's `std::sys::backtrace::lock`**. There is no shared lock,
so no reentrancy. A process whose allocator is TCMalloc does not reproduce
Mechanism 1 -- an earlier version of this writeup wrongly cited TCMalloc heap
sampling as the partner; that was incorrect.

`AllocProfiler` in `src/main.rs` models the Rust-backtrace-capturing allocator
class (it calls `Backtrace::force_capture()` on alloc). It is a faithful model
of a *Rust* tracing allocator, **not** of TCMalloc. Everything else in the PoC
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

- **Mechanism 1 (rust#130187): confirmed, deterministic -- but only under a
  *Rust* backtrace-capturing allocator.** The stuck stack above pins the
  reentrancy to the `Backtrace::capture()` line. It fires only if the global
  allocator itself calls Rust's `Backtrace` on alloc (`AllocProfiler` models
  that). **It does NOT reproduce under TCMalloc**, whose C++ lock-free unwinder
  never touches Rust's `std::sys::backtrace::lock`. So for a TCMalloc-based
  binary this is a latent Rust-std footgun, not a live hang. Permanent hang only
  in the (uncommon here) Rust-tracing-allocator configuration.
- **Mechanism 2 (TSAN `dl_iterate_phdr`): UNVERIFIED here -- I could not
  reproduce it as a hang on any TSAN available to me.**
  - The reporter's actual repro is **10 barrier-synced threads each doing
    `catch_unwind(|| { Backtrace::capture(); panic!() })`** -- *no `dlopen`*.
    Run under both the Rust-bundled TSAN and clang-21's TSAN here, it **does not
    hang** (runs tens of thousands of rounds steadily).
  - My earlier `dlopen` experiment produced a severe *transient* throughput
    collapse that always **recovered** -- that is cross-lock contention, a
    *different* phenomenon, **not** the reporter's deadlock and not a hang. An
    earlier version of this file wrongly presented that collapse as "verified
    Mechanism 2"; retracted.
  - I **cannot** test the `ScopedIgnoreInterceptors` hypothesis: no controlled
    with-guard/without-guard build, and no access to the reporter's TSAN. Upstream
    `sanitizer_libignore.cpp::OnLibraryLoaded` does hold its mutex across a
    `dl_iterate_phdr` enumeration with no such guard -- consistent with a
    deadlock being *possible* on some runtime -- but that is code-reading, not a
    reproduction.
  - **Net: whether M2 is a real deadlock is undetermined by this PoC.** The
    reporter has observed it on their runtime; I have not reproduced it on mine.
- The recursion guard (`SAMPLING`) in `src/main.rs` exists so Mechanism 1
  deadlocks against the *format_panic* capture rather than racing inside the
  profiler; a real *Rust-backtrace-capturing* allocator hits the same window
  nondeterministically under load.

## Net assessment (after reviewer feedback + verification)

Both come from one primitive: `format_panic` calls `Backtrace::capture()` on
every caught panic. What differs is the runtime dependence:

- **Mechanism 1 (prod hang): does NOT reproduce under Google's allocator.** It
  needs a *Rust* backtrace-capturing global allocator; TCMalloc (C++, lock-free
  unwind) never touches the Rust lock. Latent footgun, not a live prod hang.
  [reviewer correct]
- **Mechanism 2 (TSAN deadlock): UNVERIFIED by this PoC.** I could not reproduce
  it as a hang on either TSAN I have (Rust-bundled or clang-21), including with
  the reporter's own barrier+capture+panic code. My earlier `dlopen` "collapse"
  was a distinct, recovering contention artifact, not this deadlock -- retracted.
  I also cannot test the `ScopedIgnoreInterceptors` claim (no controlled build,
  no access to their runtime). The reporter has observed the hang; I have not.

Practical read: M1's prod risk is low (allocator-dependent; Google's isn't the
triggering kind). M2 is unresolved on my side -- real per the reporter, not
reproduced here. The one-line fix removes the shared primitive regardless, so it
stands as cheap defensive hardening, **not** as a fix for a bug I have
independently confirmed.

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

Cross-runtime check with **clang's** TSAN (proves Mechanism 2 is not a
Rust-bundled-TSAN artifact -- it collapses under stock clang-21 too):

```
clang++ -O1 -g -fsanitize=thread -pthread mechanism2_clang_tsan.cc -o m2 -ldl
TSAN_OPTIONS=halt_on_error=0 ./m2 8 8 30    # bt_threads dl_threads seconds
```

Watch for `*** COLLAPSE ***` lines (backtrace throughput dropping ~99.99% while
`dlopen` progress hits ~0). If your LLVM TSAN is patched (e.g. Google-internal)
it may not collapse; stock clang-21.1.8 does.
