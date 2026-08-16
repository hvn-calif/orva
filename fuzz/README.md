# Differential fuzzing: avro_bridge vs Apache avro-cpp

This tree is pinned at `84ce4af`, deliberately. Two divergences that later
commits closed are still open bugs here:

- **D1** — a `string` holding non-UTF-8 bytes. avro-cpp accepts it unvalidated;
  the bridge rejects it on write (`rust/value.rs`, `create_string` calls
  `utf8(v)?`) and on read.
- **D2** — duplicate keys in a map. avro-cpp keeps both entries; the bridge
  collapses them last-write-wins (`entries.insert` into a `HashMap`).

Both were fixed twelve commits later by `efb7162`. So this baseline has a known
answer key, and **the harness's acceptance test is that it finds D1 and D2 from
an empty corpus without being told to look for them.** A differential fuzzer
that cannot rediscover known bugs is not evidence of anything.

`suppressions.txt` is therefore empty of entries. Do not add D1 or D2 to make a
run green.

## Expect a red ctest here

At this commit the three differential properties **fail on purpose**:

| property | what it finds |
| --- | --- |
| `Differential.DatumCircleAgrees` | D1, D2, and `UUID_INVALID_REJECTED` |
| `Differential.SchemaVerdictsAgree` | avro-cpp accepts the namespace `ns..bad`, which has an empty component; the bridge rejects it |
| `Differential.SchemasCrossParse` | `CanonicalForm()` is not canonical for a logical-typed primitive |

That is the acceptance criterion, not a broken build. Everything else is green:
the 54 pre-existing bridge tests, the 12 generator-level properties, and the
pinning tests that record each finding.

`ctest` also runs avro-cpp's own test suite, because avro-cpp's CMakeLists
registers it unconditionally. Those are not ours;
`AvrogencppTestReservedWords` reports "Not Run" because the whole avro-cpp
subdirectory is `EXCLUDE_FROM_ALL` (it does not compile under clang 21).

To run only this project's tests:

```sh
ctest --test-dir build -R 'AvroIr|AvroSchema|AvroValue|Datum|Container|Streaming|Divergence|Suppression|Differential'
```

## Building and running

```sh
# Unit-test mode: every FUZZ_TEST also runs as a short gtest, so ctest covers
# them at about a second each.
cmake -S . -B build -DAVRO_BUILD_FUZZERS=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure

# Fuzzing mode. FUZZTEST_FUZZING_MODE is a configure-time option, so this needs
# its own build tree.
cmake -S . -B build-fuzz -DAVRO_BUILD_FUZZERS=ON -DFUZZTEST_FUZZING_MODE=ON
cmake --build build-fuzz --parallel 2

./build-fuzz/fuzz/avro_ir_fuzz_test --list_fuzz_tests
./build-fuzz/fuzz/avro_ir_fuzz_test --fuzz=AvroIr.NormalizeIsIdempotent --fuzz_for=60s
```

Keep `--parallel` low. This machine has 7 GB of RAM and ten cores; building
abseil, RE2, the ANTLR runtime, FuzzTest and avro-cpp with ASan at `-j10`
thrashes.

### Runtime environment

```sh
export ASAN_OPTIONS='detect_leaks=0:allocator_may_return_null=1:max_allocation_size_mb=1024:handle_abort=1:symbolize=1'
export UBSAN_OPTIONS='print_stacktrace=1:halt_on_error=1'
```

`allocator_may_return_null=1` matters most. Avro is a length-prefixed format,
so the mutator produces a multi-gigabyte array length within minutes. Without
it, ASan hard-aborts on the oversized request and FuzzTest records that as a
*crash*, filling the corpus with identical worthless findings. With it, the
allocation returns null and the decoder's own ceiling handles it.

`detect_leaks=0` because avro-cpp's Boost `iostreams` filter chains are not
leak-clean; leaks would drown real findings. Run LSan separately later with a
suppressions file.

`llvm-symbolizer` is **required** for readable reports and is not installed by
default here: `sudo dnf install llvm` (the packaged 21.1.8 matches clang).

## Suppressing a divergence

Every reportable difference has a stable ID in `suppress.h`. Add one per line
to `suppressions.txt`, optionally narrowed to a substring of the finding's
detail text:

```
D7                                    # deliberate: bridge accepts, avrocpp fails
DECODE_VERDICT_BRIDGE_LENIENT:array continuation
```

An unknown ID is a fatal startup error rather than a silent no-op, so a typo
cannot leave you believing something was muted. The active suppression set is
printed in every failure message, because a reproducer captured under one set
is uninterpretable under another.

Suppression works two ways, from the same file. Differences that are a property
of the *input* (D1, D2) are removed by `Normalize` **before** either lowering
runs, so both engines provably receive identical input and a suppression can
never manufacture a false agreement. Everything else is skipped at comparison
time.

## Triaging a crash

Two binaries exist for a reason. `avro_ir_fuzz_test` and the bridge-only
targets link **no avro-cpp at all**, so any sanitizer report from them is
unambiguously about this binding. The differential targets link both.

**If the top non-harness frame is in `avro::`, it is an avro-cpp finding** — to
report upstream, not a bridge regression. We are fuzzing a 2021-era C++ parser
with clang 21; it will produce its own findings.

Two bridge signals are findings even though they look benign:

- an error string containing `Rust panic caught while processing Avro input:` —
  a clean `absl::Status`, but it means apache-avro panicked;
- a process abort from Crubit's `check_no_mutable_aliasing` guard, or a Rust
  stack overflow, which aborts rather than unwinding.

## What this harness deliberately does not do

**Compare encoded bytes across engines.** Two sources of run-to-run variation
live inside the libraries: object-container sync markers are random, and Rust
`HashMap` iteration order makes encoded map bytes differ between runs (D3).
Value-level comparison is unaffected by both; byte-level comparison would be a
flaky test. Byte comparison is allowed only within one engine, in one process,
and only when the schema contains no map.

**Test hash-colliding map keys.** Rust's `HashMap` uses SipHash-1-3 with a
per-process random seed, so colliding keys cannot be precomputed and any
behaviour depending on a collision would not reproduce. Shipping a collision
list would be theatre. What is reproducible is bucket *pressure*: many keys
drawn from a tiny alphabet, which `AnyMapKey` generates.

**Instrument the Rust half** (phase 1). Corrosion drives cargo, so the C++
`-fsanitize=` flags do not reach the Rust staticlib. Roughly none of the decode
logic is visible to the mutator — the C++ layer is a thin `StatusOr` wrapper
over Crubit thunks. This is survivable only because generation is
structure-aware: the oracle does the work coverage feedback would otherwise do.
Rust heap errors are still caught, because Rust uses the system allocator and
ASan intercepts it. Phase 2 adds sancov via
`corrosion_add_target_rustflags(rust -Cpasses=sancov-module ...)`.

## Layout

| file | role |
| --- | --- |
| `ir.{h,cc}` | the `Node` tree, `Normalize`, and the mode-resolution scheme |
| `domains.{h,cc}` | FuzzTest domains; depth is a hard structural bound |
| `lower_schema.{h,cc}` | `Node` → Avro schema JSON, plus name-scope resolution |
| `suppress.{h,cc}` | divergence registry and suppression |
| `ir_test.cc` | generator-level properties; links neither engine |

`lower_schema` depends only on `ir.h` — not on the bridge, not on avro-cpp.
Keeping the lowerings on disjoint dependency edges is what makes the two-binary
triage rule above possible.
