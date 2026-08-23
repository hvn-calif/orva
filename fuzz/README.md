# Differential fuzzing: avro_bridge vs Apache avro-cpp

## Glossary

Every short form used here, other than ones assumed universally known (JSON,
UTF-8, RAM, ID).

| Term | Expansion | What it is here |
| --- | --- | --- |
| ANTLR | (a name) | the parser-generator whose C++ runtime FuzzTest pulls in; it appears only as a build dependency. |
| ASan | AddressSanitizer | clang's memory-error detector, on globally in the fuzzing build. |
| avro-cpp | Apache Avro C++ | the incumbent implementation, the thing we compare against. |
| `avro_bridge` | (a name) | the replacement: a Crubit/corrosion binding over the Rust `apache-avro` crate. Called "the bridge" below. |
| Crubit | (a name) | Google's C++/Rust interop generator, which produces the bridge's C++ API. |
| corrosion | (a name) | the CMake plugin that drives `cargo` for the Rust half of the build. |
| D1 | (a name) | divergence register entry 1: a `string` holding non-UTF-8 bytes. Also a suppression ID (`suppress.h`). |
| D2 | (a name) | divergence register entry 2: duplicate keys in a map. Also a suppression ID. |
| D3 | (a name) | divergence register entry 3: encoded map byte order is unstable, because Rust `HashMap` iteration order varies per process. Not a suppression ID. |
| divergence register | (a name) | `doc/AvrocppDivergences.md` on `main`: the hand-maintained list of known, accepted differences between the two implementations. |
| FuzzTest | (a name) | Google's property-based fuzzing framework, this harness's test driver. |
| lowering | (a name) | a function turning the generated `Node` tree into one concrete artifact: schema JSON, a bridge `AvroValue`, or an avro-cpp `GenericDatum`. |
| LSan | LeakSanitizer | clang's memory-leak detector, disabled here via `detect_leaks=0`. |
| oracle | (a name) | the comparison step deciding whether the two implementations agreed on one generated input. |
| RE2 | (a name) | Google's regular-expression library, another FuzzTest build dependency. |
| sancov | SanitizerCoverage | clang's coverage instrumentation, which feeds the mutator. |
| suppression ID | (a name) | a name that mutes one known divergence for a run, via `suppressions.txt` or `AVRO_FUZZ_SUPPRESS`. |
| UBSan | UndefinedBehaviorSanitizer | clang's undefined-behaviour detector, enabled per target. |

## Why this baseline

This tree is pinned at `84ce4af`, deliberately. Two divergences the register
documents are still open here, D1 on its write side and D2 in both directions:

- **D1** -- a `string` holding non-UTF-8 bytes. avro-cpp accepts it unvalidated;
  the bridge rejects it on write (`rust/value.rs`, `create_string` calls
  `utf8(v)?`). The **read** side is closed: the non-UTF-8 patch is in
  `patches/` and `install_avro_cpp_defaults` (`rust/datum.rs`) turns it on, so a
  decode now yields the bytes. Note that this binary used not to set that
  setting, so it measured the crate default rather than the bridge's; the row in
  the table below said "closed" of the patch, not of what ran here.
- **D2** -- duplicate keys in a map. avro-cpp keeps both entries; the bridge
  collapses them last-write-wins (`entries.insert` into a `HashMap`).

Both are register entries, written down before this harness existed, so
**the acceptance test is that the harness finds D1 and D2 starting from an empty
corpus, with no seed inputs and no test written to target them.** A differential
fuzzer that cannot rediscover known bugs is not evidence of anything.

`suppressions.txt` is therefore empty of entries. Do not add D1 or D2 to make a
run green.

### What twelve commits later actually changed

`efb7162` ("Stop losing data on two avrocpp-legal inputs") is often described as
having closed both. It did not, and the distinction matters when reading the
findings below, because the sites this harness pins are the ones it left alone:

| | what `efb7162` did | state on `main` |
| --- | --- | --- |
| D1, read | non-UTF-8 `string` decodes to `Value::Bytes`; `get_string` accepts it | closed |
| D1, write | untouched | **still rejects**: `create_string` calls `utf8(v)?`, `rust/value.rs:75` |
| D2, projected decoder | added `insert_map_entry` in `rust/decode.rs` | **rejects** duplicates. avro-cpp keeps both, so the two still disagree -- the register calls the projected path "stricter than the other two" |
| D2, apache-avro paths | untouched | still collapses |
| D2, write | untouched | **still collapses silently**: `map_put` is `HashMap::insert`, `rust/value.rs:560-569` |

So the divergences did not close; D2's decode path swapped silent data loss for
a loud rejection, which is a better failure mode but still a difference from
avro-cpp. Both pinning tests in `differential_test.cc` exercise the **write**
side of their divergence, which means both describe behaviour that is still
live on `main`.

## Expect a red ctest here

At this commit all five differential properties **fail on purpose**:

| property | input shape | what it finds |
| --- | --- | --- |
| `Differential.DatumCircleAgrees` | tree | D1, D2, and `UUID_INVALID_REJECTED` |
| `Differential.SchemaVerdictsAgree` | tree | avro-cpp accepts the namespace `ns..bad`, which has an empty component; the bridge rejects it |
| `Differential.SchemasCrossParse` | tree | the bridge re-renders a `duration` fixed as `{"type":{"type":"fixed",...}}`, which avro-cpp cannot parse, and drops its name and namespace |
| `Differential.SchemaTextVerdictsAgree` | bytes | avro-cpp accepts the namespace `ns..bad`, measured after both of this property's original findings closed |
| `Differential.DecodersAgreeOnArbitraryBytes` | bytes | avro-cpp fabricating an array item without changing the length (`fuzz/FINDINGS.md` finding 13, an avro-cpp bug). The fabricated-nulls and trailing-bytes divergences it used to find first are both closed |

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

### Running every property at once

`run_all_parallel.sh <duration> <output-dir> [rss_limit_mb]` starts every
`FUZZ_TEST` in the project concurrently, reads the property list from the
binaries, and writes per-property logs, `status.tsv` of exit codes, and memory
traces.

```sh
./fuzz/run_all_parallel.sh 1h ./fuzzrun/hour 800
```

It sets the environment above plus three ASan options that are not in it:
`quarantine_size_mb=32:malloc_context_size=10:max_redzone=16`. Thirteen jobs at
ASan's defaults peak at 703 MB each and drive the machine into swap; with these
they peak at 146 MB, and throughput is unchanged. It also sets
`AVRO_FUZZ_SUPPRESS` to the already-triaged divergences, because a property that
reports a finding aborts through a gtest assertion in the first second or two and
would never reach an hour. `AGENTS.md` has the numbers and the reasoning.

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

**If the top non-harness frame is in `avro::`, it is an avro-cpp finding** -- to
report upstream, not a bridge regression. We are fuzzing a 2021-era C++ parser
with clang 21; it will produce its own findings.

Two bridge signals are findings even though they look benign:

- an error string containing `Rust panic caught while processing Avro input:` --
  a clean `absl::Status`, but it means apache-avro panicked;
- a process abort from Crubit's `check_no_mutable_aliasing` guard, or a Rust
  stack overflow, which aborts rather than unwinding.

## What this harness deliberately does not do

**Check bridge API that avro-cpp has no counterpart to.** avro-cpp is the
reference wherever it has one. Where the bridge adds surface avro-cpp never had
-- `CanonicalForm()` and the four fingerprint functions are the whole of it at
this commit; avro-cpp 1.11.4 has no canonical-form or fingerprint API -- there
is nothing to compare, so a wrong answer there is a bridge bug rather than a
divergence and does not belong in this harness. One such bug was found while
writing it and is written up in `doc/CanonicalFormBug.md`.


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
logic is visible to the mutator -- the C++ layer is a thin `StatusOr` wrapper
over Crubit thunks. This is survivable only because generation is
structure-aware: the oracle does the work coverage feedback would otherwise do.
Rust heap errors are still caught, because Rust uses the system allocator and
ASan intercepts it. Phase 2 adds sancov via
`corrosion_add_target_rustflags(rust -Cpasses=sancov-module ...)`.

## Layout

| file | role |
| --- | --- |
| `ir.{h,cc}` | the `Node` tree, `Normalize`, and the mode-resolution scheme. Note `Normalize` legalises the tree, which is why the byte-oriented properties exist: it makes malformed shapes such as `[]` unreachable |
| `domains.{h,cc}` | FuzzTest domains; depth is a hard structural bound |
| `lower_schema.{h,cc}` | `Node` -> Avro schema JSON, plus name-scope resolution |
| `suppress.{h,cc}` | divergence registry and suppression. Its own library, `avro_fuzz_suppress`, so the value oracles can use it without dragging the tree generator in |
| `compare.{h,cc}` | the pairwise value walk. With `dump.{h,cc}` it forms `avro_fuzz_compare`, which the top-level `avro_bytes_fuzz_test` links for `DecodedValuesAgree` |
| `dump.{h,cc}` | canonical rendering of a decoded value, one function per engine: the second value oracle |
| `ir_test.cc` | generator-level properties; links neither engine |

`lower_schema` depends only on `ir.h` -- not on the bridge, not on avro-cpp.
Keeping the lowerings on disjoint dependency edges is what makes the two-binary
triage rule above possible.
