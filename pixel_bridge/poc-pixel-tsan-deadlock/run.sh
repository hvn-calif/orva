#!/usr/bin/env bash
# PoC for the pixel_bridge format_panic() backtrace-capture self-deadlock
# (rust-lang/rust#130187 reached via image.rs:295).
#
#   ./run.sh              -> suite: safe mode completes; deadlock mode hangs (killed by timeout)
#   ./run.sh deadlock     -> just the hanging case (killed after TIMEOUT s), prints stuck stack
#   ./run.sh safe         -> just the completing case
#   ./run.sh tsan         -> build+run BOTH modes under ThreadSanitizer
#
# Exit-code convention used below: 124 from `timeout` == the process was still
# running == deadlock. Any other exit == it terminated on its own.
set -uo pipefail
cd "$(dirname "$0")" || exit 1

TC="${TC:-nightly-2026-05-19-aarch64-unknown-linux-gnu}"
TRIPLE="$(rustc -vV | sed -n 's/host: //p')"
TIMEOUT="${TIMEOUT:-12}"

build_plain() { cargo "+$TC" build --quiet; }
build_tsan()  { RUSTFLAGS="-Zsanitizer=thread" cargo "+$TC" build --quiet --target "$TRIPLE" -Zbuild-std; }

bin_plain="./target/debug/pixel_tsan_deadlock_poc"
bin_tsan="./target/$TRIPLE/debug/pixel_tsan_deadlock_poc"

run_deadlock() { # $1 = binary
  echo "### deadlock mode: format_panic captures a backtrace under a sampling allocator"
  timeout "$TIMEOUT" env RUST_BACKTRACE=1 "$1" deadlock
  local rc=$?
  if [[ $rc -eq 124 ]]; then
    echo ">>> HUNG for ${TIMEOUT}s -> DEADLOCK confirmed (exit 124)"
  else
    echo ">>> exited on its own (rc=$rc) -- no deadlock in this config"
  fi
}

run_safe() { # $1 = binary
  echo "### safe mode: format_panic does NOT capture (the mitigation)"
  timeout "$TIMEOUT" env RUST_BACKTRACE=1 "$1" safe
  echo ">>> exit rc=$? (completes cleanly)"
}

# Best-effort: while the deadlock binary is hung, dump the reentrant stack.
show_stuck_stack() { # $1 = binary
  command -v gdb >/dev/null 2>&1 || { echo "(gdb not present; skipping stack dump)"; return; }
  RUST_BACKTRACE=1 "$1" deadlock >/dev/null 2>&1 &
  local pid=$!
  sleep 3
  echo "--- stuck thread (pid $pid): reentrant backtrace::lock acquisition ---"
  gdb -p "$pid" -batch -ex "bt" 2>/dev/null \
    | grep -iE "backtrace|capture|lock|futex|format_panic|alloc|_Unwind" | head -30
  kill -9 "$pid" 2>/dev/null
}

run_dlopen_loi() { # Mechanism 2: needs TSAN
  echo "### Mechanism 2: TSAN dl_iterate_phdr interceptor <-> loader-lock inversion"
  echo "### thread group 1 = Backtrace::capture (image.rs:295), group 2 = dlopen(libm.so.6)"
  RUSTFLAGS="-Zsanitizer=thread -Cunsafe-allow-abi-mismatch=sanitizer" \
    cargo "+$TC" build --quiet --target "$TRIPLE" 2>/dev/null \
    || RUSTFLAGS="-Zsanitizer=thread" cargo "+$TC" build --quiet --target "$TRIPLE" -Zbuild-std
  SECS="${SECS:-25}" RUST_BACKTRACE=1 TSAN_OPTIONS="halt_on_error=0" \
    "./target/$TRIPLE/debug/dlopen_loi"
  echo ">>> any 'THROUGHPUT COLLAPSE' line above == the lock-order inversion reproduced"
}

mode="${1:-suite}"
case "$mode" in
  dlopen) run_dlopen_loi ;;
  safe)     build_plain && run_safe "$bin_plain" ;;
  deadlock) build_plain && run_deadlock "$bin_plain"; echo; show_stuck_stack "$bin_plain" ;;
  tsan)
    echo "=== ThreadSanitizer build (instrumented std via -Zbuild-std) ==="
    build_tsan || { echo "tsan build failed"; exit 1; }
    TSAN_OPTIONS=halt_on_error=0 run_safe "$bin_tsan"
    echo
    run_deadlock "$bin_tsan"
    ;;
  suite)
    build_plain || { echo "build failed"; exit 1; }
    echo "### [1/2] safe -- baseline, the same panic + same allocator, no capture"
    run_safe "$bin_plain"
    echo
    echo "### [2/2] deadlock -- the shipped image.rs:295 capture path"
    run_deadlock "$bin_plain"
    echo
    show_stuck_stack "$bin_plain"
    ;;
  *) echo "usage: ./run.sh [suite|safe|deadlock|tsan]"; exit 2 ;;
esac
