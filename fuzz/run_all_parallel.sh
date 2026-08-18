#!/bin/bash
# Runs every FUZZ_TEST in the project concurrently for one duration each.
#
# The property list is read from the binaries rather than hardcoded, so a new
# FUZZ_TEST is picked up without editing this script.
#
# Usage: run_all_parallel.sh <duration> <output-dir> [rss_limit_mb]
#   duration        passed to --fuzz_for, e.g. 90s, 10m, 1h
#   output-dir      created if absent; holds logs, corpora and the memory trace
#   rss_limit_mb    per-process soft limit; 0 disables. Default 1500.
#
# Requires a build configured with -DFUZZTEST_FUZZING_MODE=ON.

set -uo pipefail

readonly DURATION="${1:?usage: run_all_parallel.sh <duration> <output-dir> [rss_limit_mb]}"
readonly OUT_DIR="${2:?usage: run_all_parallel.sh <duration> <output-dir> [rss_limit_mb]}"
readonly RSS_LIMIT_MB="${3:-1500}"

readonly FUZZ_BUILD="${FUZZ_BUILD:-/opt/dfz-fuzz}"
readonly BINARIES=(
  "${FUZZ_BUILD}/fuzz/avro_ir_fuzz_test"
  "${FUZZ_BUILD}/fuzz/avro_differential_fuzz_test"
  "${FUZZ_BUILD}/avro_bytes_fuzz_test"
)

# detect_leaks and allocator_may_return_null match what fuzz/CMakeLists.txt
# gives these binaries under ctest. ASAN_OPTIONS from the environment overrides
# __asan_default_options(), so it is set here explicitly rather than relied on.
#
# The other three are what make 13 concurrent jobs fit in 7.5 GB. Measured on
# AvroIr.ValueBearingTreesAreWellFormed, 60s: peak RSS 703 MB with ASan's
# defaults against 146 MB with these, and the same throughput either way
# (680k runs against 677k). The cost is a shorter use-after-free detection
# window and thinner allocation stacks in a report; llvm-symbolizer is not
# installed on this host, so those stacks are bare addresses regardless.
#
# max_allocation_size_mb is what README.md prescribes and what turns avro-cpp's
# oversized reserves into a std::bad_alloc CallAvrocpp can report as a verdict.
# Without it, a request larger than the machine can map reaches ASan's
# out-of-memory path, which aborts whatever allocator_may_return_null says: one
# 837 GB request ended a run that way. It is defence in depth rather than the
# primary guard, because the bridge's half cannot use it -- Rust aborts on a
# failed allocation instead of throwing -- so both harnesses still have to keep
# declared counts small enough that the bridge never reserves past the limit.
export ASAN_OPTIONS="detect_leaks=0:allocator_may_return_null=1:max_allocation_size_mb=1024:quarantine_size_mb=32:malloc_context_size=10:max_redzone=16"

# Already-triaged divergences, muted for the duration of a long run.
#
# fuzz/suppressions.txt stays committed empty on purpose: the harness proving
# itself means rediscovering D1 and D2 from an empty corpus without being told
# to look. But a property that reports a finding aborts the process through a
# gtest assertion, and --continue_after_crash does not cover that path, so five
# of these properties would die in their first second and never reach an hour.
# Muting by ID here leaves the committed default alone. Override with
# AVRO_FUZZ_SUPPRESS= (empty) to see the known findings again.
: "${AVRO_FUZZ_SUPPRESS=D1,D2,D9,SCHEMA_PARSE_VERDICT,CROSS_PARSE_ROUND_TRIP,\
DECODE_VERDICT_BRIDGE_LENIENT,DECODE_VERDICT_AVROCPP_LENIENT,\
UUID_INVALID_REJECTED,UUID_TEXT_NOT_PRESERVED,STRING_BYTES_TYPE_MISMATCH,\
RUST_PANIC_CAUGHT,ARRAY_LEN,ARRAY_ITEM_FABRICATED,\
SCALAR_VALUE:uuid values differ}"
export AVRO_FUZZ_SUPPRESS

# avro_bytes_fuzz_test.cc predates fuzz/suppress.h and carries its own table and
# its own variable, so AVRO_FUZZ_SUPPRESS does not reach it. Empty by default
# because its kKnownDivergences table already covers what it has found.
: "${AVRO_BYTES_FUZZ_SKIP=}"
export AVRO_BYTES_FUZZ_SKIP

for binary in "${BINARIES[@]}"; do
  if [[ ! -x "${binary}" ]]; then
    echo "missing binary: ${binary}" >&2
    exit 1
  fi
done

mkdir -p "${OUT_DIR}/logs" "${OUT_DIR}/corpus"

# Resident memory of one job: the launched process plus its direct children,
# since each fuzzing job runs the property in a forked runner.
tree_rss_kb() {
  local pid="$1" total=0 kb
  while read -r kb; do
    total=$((total + kb))
  done < <(ps -o rss= -p "${pid}" --ppid "${pid}" 2>/dev/null)
  echo "${total}"
}

# Samples memory every 5s until killed, so a run that dies of memory pressure
# leaves evidence of why, and per-job so the heaviest property is identifiable.
sample_memory() {
  local totals="$1" per_job="$2"
  shift 2
  local -a watched=("$@")
  printf 'unix_seconds\tfuzz_rss_kb\tmem_available_kb\tswap_used_kb\tlive_jobs\n' \
    > "${totals}"
  printf 'unix_seconds\tproperty\trss_kb\n' > "${per_job}"
  while :; do
    local rss=0 live=0 index pid job_rss now available swap_total swap_free
    now=$(date +%s)
    for index in "${!watched[@]}"; do
      pid="${watched[index]}"
      kill -0 "${pid}" 2>/dev/null || continue
      live=$((live + 1))
      job_rss=$(tree_rss_kb "${pid}")
      rss=$((rss + job_rss))
      printf '%s\t%s\t%s\n' "${now}" "${labels[index]}" "${job_rss}" \
        >> "${per_job}"
    done
    available=$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo)
    swap_total=$(awk '/^SwapTotal:/ {print $2}' /proc/meminfo)
    swap_free=$(awk '/^SwapFree:/ {print $2}' /proc/meminfo)
    printf '%s\t%s\t%s\t%s\t%s\n' "${now}" "${rss}" "${available}" \
      "$((swap_total - swap_free))" "${live}" >> "${totals}"
    sleep 5
  done
}

declare -a pids=()
declare -a labels=()

for binary in "${BINARIES[@]}"; do
  while read -r property; do
    [[ -n "${property}" ]] || continue
    log="${OUT_DIR}/logs/${property}.log"
    # continue_after_crash is what makes a fixed-duration run possible at all:
    # five of these properties find a known divergence in their first second,
    # and the default behaviour is to stop there.
    "${binary}" \
      --fuzz="${property}" \
      --fuzz_for="${DURATION}" \
      --corpus_database="${OUT_DIR}/corpus" \
      --rss_limit_mb="${RSS_LIMIT_MB}" \
      --continue_after_crash=true \
      > "${log}" 2>&1 &
    pids+=("$!")
    labels+=("${property}")
    echo "started ${property} (pid $!)"
  done < <("${binary}" --list_fuzz_tests 2>&1 |
    sed -n 's/^\[\*\] Fuzz test: //p')
done

echo "${#pids[@]} properties running for ${DURATION}; logs in ${OUT_DIR}/logs"

sample_memory "${OUT_DIR}/memory.tsv" "${OUT_DIR}/memory_by_job.tsv" "${pids[@]}" &
sampler_pid=$!
# shellcheck disable=SC2064  # Expand the pid now; the variable is gone at trap time.
trap "kill ${sampler_pid} 2>/dev/null" EXIT

status_file="${OUT_DIR}/status.tsv"
printf 'property\texit_code\n' > "${status_file}"
failures=0
for i in "${!pids[@]}"; do
  wait "${pids[i]}"
  code=$?
  printf '%s\t%s\n' "${labels[i]}" "${code}" >> "${status_file}"
  if [[ "${code}" -ne 0 ]]; then
    failures=$((failures + 1))
  fi
done

echo "done: ${failures} of ${#pids[@]} properties exited non-zero"
exit 0
