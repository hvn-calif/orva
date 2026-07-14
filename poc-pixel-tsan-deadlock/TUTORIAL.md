# TUTORIAL: verify these deadlock claims yourself with gdb

Audience: you know Rust (you read the Rust book) but have never used gdb, and
terms like "backtrace lock", "dl_iterate_phdr", "TSAN interceptor" are new. By
the end you will be able to (1) make a program hang on purpose, (2) attach gdb to
a stuck program, (3) read the stack to see *why* it is stuck, and (4) decide for
yourself whether each claim in this folder is real.

Everything is copy-paste. Commands assume you are in this directory:

```
cd poc-pixel-tsan-deadlock
```

--------------------------------------------------------------------------------

## 0. One-time setup

You need the nightly toolchain used by this repo, its source (for building an
instrumented std), and gdb.

```
# toolchain + std source (for the TSAN builds later)
rustup toolchain list | grep nightly-2026-05-19   # should already be installed
rustup component add rust-src --toolchain nightly-2026-05-19-aarch64-unknown-linux-gnu

# gdb
gdb --version | head -1        # if "command not found": sudo dnf install -y gdb
```

gdb attaches to *other* processes, which the kernel restricts. Check:

```
cat /proc/sys/kernel/yama/ptrace_scope
```

- `0` = you can attach to your own processes freely. Good, do nothing.
- `1` (common) = you can still attach to processes *you launched from the same
  shell* (that is what we do here), so you are fine.
- If you ever get `ptrace: Operation not permitted`, run gdb with `sudo`.

--------------------------------------------------------------------------------

## 1. The mental model (read once, 5 minutes)

**The one line under suspicion.** In `pixel_bridge/rust/image.rs`, the function
`format_panic` used to call:

```rust
let backtrace = std::backtrace::Backtrace::capture();
```

This runs every time the `image` crate panics on a bad image. The whole question
is: can that line make the program *hang*?

**What "capture a backtrace" actually does.** It walks up the call stack to list
the functions that got you here. Walking the stack is not free magic:

1. It takes a **lock** (a "do not touch while I work" flag). Rust has one global
   lock for this, called the *backtrace lock*. Only Rust's own backtrace code
   ever touches it.
2. While holding that lock it calls a C function, `_Unwind_Backtrace`, which for
   each stack frame must find that function's "unwind tables". To do that it
   calls another C function, `dl_iterate_phdr`, which lists the shared libraries
   loaded in the process. `dl_iterate_phdr` takes a *different* lock: the
   **dynamic loader lock** (owned by the C library, not Rust).

So one innocent `capture()` grabs **two** locks: the Rust backtrace lock, then
(deep inside) the loader lock.

**What a deadlock is.** A deadlock is a standoff between threads over locks.
The two shapes that matter here:

- *Self-deadlock (Mechanism 1):* one thread takes a lock, then before releasing
  it tries to take **the same lock again**. If the lock is "non-reentrant" (it
  does not allow the same thread in twice), the thread waits for itself forever.
- *Lock-order inversion (Mechanism 2):* thread A takes lock X then wants lock Y;
  thread B takes lock Y then wants lock X. Each holds what the other needs.
  Neither moves. This needs two locks and two threads.

**The two claims in this folder, in plain words:**

- **Mechanism 1** (`src/main.rs`): the Rust backtrace lock is non-reentrant. If,
  *while* capture holds it, a memory allocation happens that *also* asks Rust to
  capture a backtrace, the thread self-deadlocks. This requires a special
  allocator that captures Rust backtraces. We can force it and watch it hang.
- **Mechanism 2** (`src/barrier_repro.rs`): under the ThreadSanitizer (TSAN)
  testing tool, the loader lock and a TSAN-internal lock might be taken in
  opposite orders by different threads, causing a lock-order inversion. This one
  is *reported by a reviewer*; this PoC has **not** reproduced it. You will test
  it and see for yourself.

Your job in this tutorial is not to trust the paragraphs above. It is to make the
program stop, look at the stack, and confirm or reject each story.

--------------------------------------------------------------------------------

## 2. gdb in ten minutes (only what you need)

gdb is a debugger. We use it in exactly one way: **attach to a program that is
already running (and possibly stuck), and print what every thread is doing.**

The whole workflow is three commands:

```
gdb -p <PID>                 # attach to a running process by its PID
(gdb) thread apply all bt    # print a stack trace ("backtrace") for EVERY thread
(gdb) detach                 # let the process go, then:
(gdb) quit
```

Or do it in one non-interactive shot (this is what we will mostly use):

```
gdb -p <PID> -batch -ex "thread apply all bt"
```

- `-p <PID>`: attach to process id PID. Get the PID with `pgrep -n <name>` (the
  `-n` means "newest match").
- `-batch`: run the `-ex` commands, print output, exit. No interactive prompt.
- `-ex "..."`: a gdb command to run. You can pass several `-ex` in a row.

**How to read a stack trace.** One thread's backtrace looks like a list of
frames, newest (where the thread is *right now*) at the top:

```
Thread 5 (LWP 12345):
#0  0x... in futex_wait () at .../futex.rs:73
#1  ...  in Mutex::lock_contended () at .../mutex/futex.rs:61
#2  ...  in std::sys::backtrace::lock () at .../sys/backtrace.rs:24
#3  ...  in std::backtrace::Backtrace::create () at .../backtrace.rs:326
#4  ...  in your_program::something () at src/main.rs:14
```

Read it top-to-bottom as "right now I am in `futex_wait`, which was called by
`Mutex::lock_contended`, which was called by ... , which your code reached from
`something` at `src/main.rs:14`." `#0` is the deepest call.

**The one skill that matters:** spotting a *stuck* thread. A thread waiting on a
lock sits in one of these at `#0`/`#1`:

- `futex_wait` / `futex` (Rust `std::sync::Mutex` waits here)
- `__lll_lock_wait`, `pthread_mutex_lock` (C library locks)
- `FutexWait`, `sanitizer_mutex`, `GenericScopedLock` (TSAN's own locks)

When you see a thread parked in one of those, read *downward* until you reach a
function name you recognize (your code, or `Backtrace`, or `dlopen`). That tells
you which lock it is stuck on and why.

That is all the gdb you need. Now let's make things hang.

--------------------------------------------------------------------------------

## 3. Lab 1 (Mechanism 1): make it hang on demand, deterministically

This is the confidence builder: it hangs every time, so you can practice gdb on a
guaranteed-stuck process.

### 3a. Run it and watch it hang

```
cargo +nightly-2026-05-19-aarch64-unknown-linux-gnu build --quiet
RUST_BACKTRACE=1 ./target/debug/pixel_tsan_deadlock_poc deadlock
```

It prints two lines and then **stops forever** (press Ctrl-C to kill it):

```
[poc] mode=deadlock (capture_backtrace_in_format_panic=true)
[poc] simulating image-crate panic on malformed input (b/372147306)
        <-- hangs here; no more output
```

Compare with the *safe* mode (the same program with the capture removed, which is
the fix):

```
RUST_BACKTRACE=1 ./target/debug/pixel_tsan_deadlock_poc safe
```

This one prints `... reached end of main -- NO deadlock` and exits. Same panic,
same allocator, one line different. That contrast is the finding in miniature.

### 3b. Attach gdb to the hung process

Run it in the background, grab its PID, attach. The most robust way to get the
PID is `$!`, which the shell sets to the process you just backgrounded with `&`:

```
RUST_BACKTRACE=1 ./target/debug/pixel_tsan_deadlock_poc deadlock &   # note the trailing &
PID=$!                                # PID of that background process
sleep 2
echo "hung pid = $PID"
gdb -p "$PID" -batch -ex "bt"        # bt = backtrace of the (single) stuck thread
kill -9 "$PID"                        # clean up when done
```

(Why `$!` and not `pgrep -n <name>`? Linux truncates a process's name to 15
characters, so `pixel_tsan_deadlock_poc` shows up as `pixel_tsan_dead` and a
`pgrep -n pixel_tsan_deadlock_poc` finds nothing. Use `$!` when you launched it,
or `pgrep -f <name>` which matches the full command line.)

### 3c. Read the stack (this is the whole point)

You will see something like this. Read the annotations:

```
#1  futex_wait ()                          futex.rs:73     <-- STUCK here, waiting on a lock
#5  std::sys::backtrace::lock ()           backtrace.rs:24 <-- trying to take the backtrace lock (2nd time!)
#6  std::backtrace::Backtrace::create ()   backtrace.rs:326
#7  ...::AllocProfiler::alloc ()           src/main.rs     <-- an allocation is capturing a backtrace
#9  alloc::alloc::alloc ()                                 <-- ...the allocation happened...
#17 alloc::vec::Vec::push<BacktraceFrame>                  <-- ...inside the FIRST capture collecting frames
#22 _Unwind_Backtrace (libgcc_s)
#25 std::backtrace::Backtrace::create ()   backtrace.rs    <-- the FIRST capture already holds the lock
#26 ...::format_panic ()                   src/main.rs     <-- == pixel_bridge image.rs:295
#28 ...::main ()
```

What you are looking at, bottom to top: `main` -> `format_panic` calls
`Backtrace::create` (frame 25) which **takes the backtrace lock** and starts
collecting frames; collecting frames allocates (frame 17 -> 9); that allocation
runs the custom allocator (frame 7) which **asks for another backtrace** (frame
6) which tries to **take the same lock again** (frame 5) and blocks forever
(frame 1). One thread, one lock, taken twice. That is Mechanism 1, and you just
saw it with your own eyes.

Now you understand Mechanism 1 and you can drive gdb. Everything else is variations.

--------------------------------------------------------------------------------

## 4. Lab 2 (Mechanism 2): the reviewer's repro, and how to tell hang from "just slow"

This is the elusive one. The claim: under TSAN, 10 threads all capturing +
panicking at once deadlock. On the two TSANs available when writing this, it did
**not** hang. You are going to try to make it hang and, crucially, learn to tell
the difference between "hung" and "running slowly".

### 4a. Build and run under TSAN

```
./run.sh barrier
```

(That script builds `src/barrier_repro.rs` with ThreadSanitizer and runs it. The
first build is slow because TSAN rebuilds the standard library.)

You will see a heartbeat:

```
[barrier] 10 threads, capture()+panic!() per round, barrier-synced
[barrier] completed rounds = 3324
[barrier] completed rounds = 5752
[barrier] completed rounds = 8210
...
```

### 4b. The key skill: is it hung, or just slow?

This is where people fool themselves. Two different situations:

- **Not hung:** the `completed rounds` number keeps going up. Slow is fine; a
  deadlock is *frozen*, not slow.
- **Candidate hang:** the number stops changing for many seconds **and the
  process is still alive**. Only then is it worth attaching gdb.

Do NOT rely on "the terminal stopped printing" alone. Output through a pipe can
buffer and *look* frozen while the program is fine. Always confirm with the
round counter (and gdb below), not with silence.

Let the heartbeat run for a minute. On the reference machine it climbs to tens of
thousands of rounds and never freezes -> **no hang observed.** If yours behaves
the same, you have reproduced the "cannot confirm M2" result honestly.

### 4c. If it DOES freeze: catch it in gdb

Suppose the counter sticks at, say, `completed rounds = 4291` and does not move
for 10+ seconds. Attach and dump every thread:

```
PID=$(pgrep -nf barrier_repro)      # -f matches the full command line
gdb -p "$PID" -batch -ex "set pagination off" -ex "thread apply all bt 8"
```

`bt 8` prints only the top 8 frames per thread (enough, and less noise). Now
diagnose using the rules from section 2:

- **A real deadlock:** you will find threads parked in `futex_wait` /
  `FutexWait` / `sanitizer_mutex` / `pthread_mutex_lock`, AND the number stays
  frozen across *repeated* gdb dumps (run the command again 5 seconds later;
  same threads, same lock frames = truly stuck).
- **Just contention / slow:** if you dump twice and the threads have *moved*
  (different frames), or the round counter resumes, it was never a deadlock.

For Mechanism 2 specifically, a real inversion would show two different kinds of
stuck thread at the same time, for example:

```
Thread A:  ... _Unwind_Find_FDE / dl_iterate_phdr ...   <-- wants the loader lock
Thread B:  ... FutexWait / sanitizer_mutex ... OnLibraryLoaded / dlopen/dlclose ...  <-- holds loader-lock state, wants a TSAN lock
```

If you can capture *that* pair, frozen, across two dumps seconds apart, you have
proven Mechanism 2 is a real deadlock on your machine. If you cannot make it
freeze at all, you have reproduced the "unverified" conclusion.

### 4d. Turn the knobs

Make collisions more likely and see if behavior changes:

```
# more threads, only the barrier binary
THREADS=64 ./run.sh barrier

# run longer / count rounds to a fixed number then stop
ROUNDS=200000 THREADS=32 ./run.sh barrier
```

If it never freezes across large thread counts and long runs, that is real,
reportable evidence that *your* TSAN does not exhibit the deadlock.

--------------------------------------------------------------------------------

## 5. Lab 3 (optional): the dlopen contention experiment

`src/dlopen_loi.rs` is a *different* experiment (backtrace threads + threads that
`dlopen`/`dlclose` a library). It is NOT the reviewer's repro and NOT a proof of
their deadlock; it produces a *transient* slowdown that recovers. It is included
so you can see the difference between "contention that recovers" and "a hang".

```
./run.sh dlopen
```

Watch the `bt=+N` throughput numbers. You will likely see them crater
(`*** THROUGHPUT COLLAPSE ***`) and then **recover**. Recovery is the tell: a
deadlock never recovers. Use this lab to train your eye so you do not mistake a
slow patch for a deadlock in Lab 2.

You can also run the same idea under clang's C++ TSAN (no Rust involved) to prove
the slowdown is a property of the sanitizer runtime, not of Rust:

```
clang++ -O1 -g -fsanitize=thread -pthread mechanism2_clang_tsan.cc -o m2 -ldl
TSAN_OPTIONS=halt_on_error=0 ./m2 8 8 30
```

--------------------------------------------------------------------------------

## 6. Your "is it really hung?" checklist

Before you ever claim "it deadlocked", confirm all of:

1. [ ] The program's own progress signal (here: `completed rounds`) is **frozen**,
       not just slow. Watch it for 10+ seconds.
2. [ ] The process is **still alive**: `kill -0 <PID>` succeeds (no output), and
       `ps -o stat= -p <PID>` is NOT `Z` (zombie) and NOT gone.
3. [ ] `gdb ... thread apply all bt` shows threads parked in a *lock wait*
       (`futex_wait`, `FutexWait`, `pthread_mutex_lock`, `sanitizer_mutex`).
4. [ ] A **second** gdb dump several seconds later shows the **same** threads in
       the **same** lock frames (truly stuck, not moving).
5. [ ] You can name the cycle: which thread holds what, and wants what.

If any box is unchecked, you have contention or slowness, not a proven deadlock.
Being able to say "I could not check box 4" is a perfectly good, honest result --
it is exactly where this PoC landed for Mechanism 2.

--------------------------------------------------------------------------------

## 7. Experiments to convince (or un-convince) yourself

- **Prove Mechanism 1 depends on the capture:** run `deadlock` (hangs) vs `safe`
  (does not). Only the `Backtrace::capture()` line differs. This is the cleanest
  proof in the folder.
- **Prove Mechanism 1 needs a backtrace-capturing allocator:** the default system
  allocator does not capture backtraces, which is why a normal program with
  `Backtrace::capture()` does not hang. `src/main.rs`'s `AllocProfiler` is what
  supplies the reentrancy; read it (it is ~15 lines).
- **Try to break Mechanism 2:** vary `THREADS`, `ROUNDS`, add/remove
  `RUST_BACKTRACE=1`, and (advanced) delete the `set_hook` line in
  `src/barrier_repro.rs` so the *default* panic hook also captures a backtrace.
  If none of these freeze, write that down: it is evidence.
- **Compare TSAN versions:** if you have another clang/LLVM, build
  `mechanism2_clang_tsan.cc` with it and compare. Different sanitizer versions
  behave differently; that is the crux of the whole M2 disagreement.

--------------------------------------------------------------------------------

## 8. Glossary

- **lock / mutex:** a "one at a time" gate around shared data. `lock()` waits your
  turn; the holder must release before others proceed.
- **futex:** the Linux primitive Rust's `Mutex` uses to sleep while waiting. A
  thread sitting in `futex_wait` is asleep waiting for a lock.
- **reentrant / non-reentrant:** a reentrant lock lets the *same thread* lock it
  again without waiting; a non-reentrant one does not (so re-locking = hang).
  Rust's backtrace lock is non-reentrant.
- **unwinding:** walking up the call stack, used both for capturing a backtrace
  and for propagating a `panic!`.
- **FDE / unwind tables:** per-function data telling the unwinder how to step up
  one frame. Found via `dl_iterate_phdr`.
- **dl_iterate_phdr:** C-library function that lists loaded shared objects; takes
  the **loader lock**. Called under the hood by backtrace/panic unwinding.
- **loader lock:** the C library's lock protecting the list of loaded libraries.
  `dlopen`/`dlclose` and `dl_iterate_phdr` all take it.
- **TSAN (ThreadSanitizer):** a testing build (`-Zsanitizer=thread`) that detects
  data races by *intercepting* library calls. Its interceptors take their own
  internal locks -- the extra locks at the heart of the Mechanism 2 story.
- **interceptor:** TSAN's wrapper around a real function (like `dlopen`) that does
  bookkeeping before/after calling the real one.
- **PID / LWP:** process id / a thread's id. `pgrep -n name` finds the newest PID.

--------------------------------------------------------------------------------

## 9. gdb cheat sheet

```
gdb -p <PID>                                  attach interactively
  (gdb) info threads                          list threads
  (gdb) thread apply all bt                   backtrace of every thread
  (gdb) thread apply all bt 8                 ...top 8 frames each
  (gdb) bt                                    backtrace of the current thread
  (gdb) thread 5                              switch to thread 5
  (gdb) detach                                release the process
  (gdb) quit

gdb -p <PID> -batch -ex "thread apply all bt"     one-shot, non-interactive

PID=$!                     PID of the process you just launched with `&` (most robust)
pgrep -nf <name>           newest PID whose full command line matches name
                           (plain `pgrep -n <name>` fails on names > 15 chars)
kill -0  <PID>             is it alive? (no output = yes)
kill -9  <PID>             force kill
ps -o stat= -p <PID>       process state (Z = zombie/dead, R = running, S/D = sleeping)
```

Two gotchas you will hit:
- **"zombie / Operation not permitted"** from gdb = the process already exited
  before you attached. Re-run and attach faster, or add `sleep`.
- **Attaching pauses the process** while gdb is in control; it resumes on
  `detach`/`quit` or when gdb exits. So the program cannot make progress *during*
  the dump; that is expected and not itself a hang.
```
