# Working notes for the Avro differential fuzzing work

Read this before touching `fuzz/` or `avro_bytes_fuzz_test.cc`. It records what
was built, what was measured, which of my own earlier claims turned out wrong,
and what is still open.

## Glossary

| Term | Expansion | What it is here |
| --- | --- | --- |
| ASan | AddressSanitizer | clang's memory-error detector, enabled globally in the fuzzing build only. |
| avro-cpp | Apache Avro C++ | `release-1.11.4`, the incumbent implementation we compare against. |
| `avro_bridge` | (a name) | the replacement: a Crubit/corrosion binding over the Rust `apache-avro` crate. Called "the bridge" below. |
| divergence register | (a name) | `doc/AvrocppDivergences.md` on `main`, entries `D1`..`D12`. Does not exist on this branch. |
| D1, D2, D3 | (a name) | register entries: non-UTF-8 in a string, duplicate map keys, unstable encoded map byte order. |
| FuzzTest | (a name) | Google's property-based fuzzing framework. Each `FUZZ_TEST` also runs as a one-second gtest. |
| lowering | (a name) | a function turning the generated tree into schema JSON, a bridge `AvroValue`, or an avro-cpp `GenericDatum`. |
| oracle | (a name) | the step deciding whether the two implementations agreed on one input. |
| LSan | LeakSanitizer | runs at exit under ASan unless `detect_leaks=0`. |

## Branch state

Branch `avro-diff-fuzz`, worktree `/home/hvn/orva-difffuzz`, based on `84ce4af`,
twelve commits behind `main`. Committed here:

| commit | contents |
| --- | --- |
| `edfd1ce`, `7285ce4` | the original structure-aware harness in `fuzz/` and `report.md` |
| `dbd2501` | corrections to those claims, the byte-oriented properties in `fuzz/`, `doc/CanonicalFormBug.md` |
| `4e4122f` | `avro_bytes_fuzz_test.cc`, the self-contained byte-oriented fuzzer |
| `38fa21c` | the parallel hour-long runs, findings 8 to 12, the harness fixes they needed, and the absl string conversion |

## Two harnesses, and why

`fuzz/` is structure-aware: it generates one `Node` tree and derives the schema
JSON, the bridge value and the avro-cpp datum from it, so the value matches the
schema by construction. That is what makes it productive despite the Rust half
being uninstrumented, since the oracle does the work coverage feedback would
otherwise do.

`avro_bytes_fuzz_test.cc` is byte-oriented and self-contained, because the
project wanted something committable now while `fuzz/` is not ready. It
duplicates two properties that also exist in `fuzz/differential_test.cc`
(`SchemaTextVerdictsAgree`, `DecodersAgreeOnArbitraryBytes`). **If `fuzz/` ever
lands, delete the duplicates from one side rather than maintaining both.**

Each finds things the other cannot:

- The tree harness reaches the **write** path (`CreateString`, `MapPut`), which
  a byte-oriented harness cannot touch at all. D1 and D2 both live there, so a
  byte-only harness could not run the acceptance test.
- The byte harness reaches malformed input the tree generator provably cannot
  produce, because `Normalize` legalises its output. `NormalizeChildren`
  (`fuzz/ir.cc:257-265`) tops an empty union up to one branch, so `[]` is
  unreachable however long the tree properties run.

## Build and run

The fuzzing build needs its own tree, since `FUZZTEST_FUZZING_MODE` is
configure-time. Existing trees on this host: `/opt/dfz-b1` (unit-test mode),
`/opt/dfz-fuzz` (fuzzing mode). Both point at this worktree.

```sh
cmake -S . -B build -DAVRO_BUILD_FUZZERS=ON
cmake --build build --target avro_bytes_fuzz_test --parallel 4
./build/avro_bytes_fuzz_test

cmake -S . -B build-fuzz -DAVRO_BUILD_FUZZERS=ON -DFUZZTEST_FUZZING_MODE=ON
cmake --build build-fuzz --target avro_bytes_fuzz_test --parallel 2
./build-fuzz/avro_bytes_fuzz_test --fuzz=AvroBytes.DecodersAgreeOnAcceptance --fuzz_for=10m
```

Keep `--parallel` at 2 for the fuzzing build. The machine has 7 GB of RAM and
that build compiles abseil, RE2, ANTLR, FuzzTest and avro-cpp with ASan.

## Running every property at once

`fuzz/run_all_parallel.sh <duration> <output-dir> [rss_limit_mb]` runs all
thirteen properties concurrently, reading the list from the binaries rather than
hardcoding it. It writes per-property logs, a corpus database, `status.tsv` with
each exit code, and `memory.tsv` / `memory_by_job.tsv` traces.

```sh
./fuzz/run_all_parallel.sh 1h ./fuzzrun/hour 1200
```

`fuzzrun/` is gitignored, so the logs, corpora and traces from a run stay out of
the history. Three hour-long runs from this session are there under `hour`,
`hour2` and `hour3`; `hour3` is the validated one.

**Validated: thirteen of thirteen exit 0 after a full hour**, peaking at 2.1 GB
resident with 2.5 GB still available. Two earlier attempts got twelve of
thirteen; the notes below are what the difference cost.

Five things had to be true before thirteen properties could survive an hour
together, and each cost a debugging session:

- **`--continue_after_crash=true` does not do what its name suggests here.**
  These properties fail through a gtest assertion, which aborts the process in
  the FuzzTest engine's in-process mode, before FuzzTest's crash handling sees
  it. The flag is set anyway, and is not what keeps a run alive.
- **Known divergences have to be muted or five properties die in their first
  second.** Two separate mechanisms: `AVRO_FUZZ_SUPPRESS` for `fuzz/`, and
  `AVRO_BYTES_FUZZ_SKIP` for `avro_bytes_fuzz_test.cc`, which predates
  `fuzz/suppress.h` and carries its own table. The script sets the first and
  leaves the second empty. `fuzz/suppressions.txt` stays committed empty, so an
  ordinary run still rediscovers D1 and D2 unaided.
- **ASan's defaults do not fit thirteen jobs in 7.5 GB.** Peak RSS per job is
  703 MB with defaults against 146 MB with
  `quarantine_size_mb=32:malloc_context_size=10:max_redzone=16`, and throughput
  is the same either way (680k against 677k runs in 60s on
  `ValueBearingTreesAreWellFormed`). Thirteen jobs then sit at about 1.8 GB
  total. The cost is a shorter use-after-free window and thinner allocation
  stacks, which matters little while `llvm-symbolizer` is missing.
- **One input can ask for more memory than the machine has**, so
  `--rss_limit_mb` is not optional. 1200 MB per job against a 150 to 250 MB
  steady state.
- **`DecodersAgreeOnArbitraryBytes` needed its declared-count cap lowered.** At a
  cap of a million elements, avro-cpp allocates a `GenericDatum` per declared
  element, and the property's resident set climbed 112 MB to 847 MB over eighteen
  minutes and died on its RSS limit -- twice, in two consecutive hour attempts. At
  16k it ends the hour at 256 MB. Not a leak: five minutes with `detect_leaks=1`
  and 1.27 million executions exits clean.

Measured throughput over the validated hour, thirteen jobs at once: 17 to 32
million runs each for the three byte-oriented properties, 3.7 to 12 million for
the five `AvroIr` invariants, 6.1 to 12 million for the five `Differential`
properties. The slowest is `DeepChainsRenderWithoutOverflow` at about 1,100 runs a
second, the fastest `ReencodingAgreesWhenBothDecode` at about 12,000. No new
findings surfaced in that hour, which is what a clean run looks like once the
known ones are muted.

`llvm-symbolizer` is still not installed, so ASan reports are bare hex addresses
until `sudo dnf install llvm`. In the meantime `addr2line -e <binary> -f -C
<addresses>` resolves them, including the frames inside avro-cpp: that is how
findings 8 and 9 were attributed to `GenericDatum::init` and
`GenericReader::read` rather than guessed at.

## Expected state of the test suites

`avro_bytes_fuzz_test`: **22/22 pass** with no environment variables, and also
under `ASAN_OPTIONS=detect_leaks=0` and with `allocator_may_return_null=1`.
If it fails, the fuzz properties found something new, which is the design.

`avro_bridge_test`: **63/63**, and `avro_bridge_strict_test` is the same 63 built
from the same source with `AVRO_BRIDGE_TEST_STRICT_SETTINGS` defined, so
**126/126** across the two. Every setting `avro_bridge.h` exposes is a set-once
process global, so one process can observe only one value of each; the second
binary is how the strict value of each gets tested at all. A test whose outcome
depends on a setting branches on `kStrictSettings` rather than being duplicated.

The bridge's own Rust suite is **65 tests** across four binaries (`cargo test` in
`rust/`). It was 70 across five: `tests/reject_trailing_bytes.rs` existed only
because a `OnceLock` cannot be reset in-process, and the second C++ binary
replaced it.

`ctest` on the whole project: **249/255 or 250/255**, and `ctest -R AvroBytes` is
22/22. The total is not stable, and that is worth understanding before reading any
change in it as a regression.

Six tests do not pass. Five are by design and one is avro-cpp's own
`AvrogencppTestReservedWords` reporting "Not Run", because that subdirectory is
`EXCLUDE_FROM_ALL`. Of the five, four fail every time and one is flaky:

| property | measured over 8 runs each |
| --- | --- |
| `Differential.DatumCircleAgrees` | 8 fail |
| `Differential.SchemaVerdictsAgree` | 8 fail |
| `Differential.SchemasCrossParse` | 8 fail |
| `Differential.SchemaTextVerdictsAgree` | 8 fail |
| `Differential.DecodersAgreeOnArbitraryBytes` | **7 pass, 9 fail across 16 runs** |

Do not silence any of the five. The flaky one is flaky for a plain reason: a
`FUZZ_TEST` in unit-test mode is a one-second run from a random seed, so whether it
draws an input that reveals a divergence is chance. Pinning `FUZZTEST_PRNG_SEED`
makes it deterministic -- measured, 6 of 6 fail with the seed fixed -- so wiring a
fixed seed into the `ctest` registration would turn the suite back into a stable
regression gate. That is **not done**, and it is the single change that would most
improve the "did I break anything" question, which cost two debugging rounds during
the closure series.

Its failure rate dropped from "always" to "about half" when the cascade fix landed,
because the array-of-`null` cascade it used to trip over on almost every run is no
longer reported as a pile of independent findings. The remaining failures are other
open divergences, and `DECODE_VERDICT_AVROCPP_LENIENT` under `"string"` is the one
seen most often.

The counts have moved four times, so treat any older number in this repository as
stale rather than as a regression:

| when | ctest | AvroBytes |
| --- | --- | --- |
| before `avro_bytes_fuzz_test` joined the build | 150/156 (`report.md` section 7) | -- |
| after the one-hour runs' findings | 174/180 | 20/20 |
| after the divergence-closure series | 176-177/182 | 21/21 |
| after the closures were pinned in `avro_bridge_test` | **249-250/255** | **22/22** |

The last jump is 73 tests and almost none of it is coverage of new ground: six
pin the Tier A and Tier B closures on the bridge's side of the comparison, one
guards the toggle, two pin non-finite float handling, and the other sixty-three
are the second build of the same source under
`AVRO_BRIDGE_TEST_STRICT_SETTINGS`. The five non-passes did not change.

Two things moved the totals before that: one differential test split in two when
the empty union and empty enum closed separately, and one new test pinned finding
13. The
cascade fix in `Comparer` did not remove a non-pass, as first reported here from a
single lucky run; it turned one deterministic failure into a flaky one. That is a
harness change and not a change to either engine.

## Divergences found

Fifteen. Each has a test in `avro_bytes_fuzz_test.cc` or
`fuzz/differential_test.cc` named after it. Full detail in `fuzz/FINDINGS.md`.

**Closing them is now the work**, per `doc/specs/DivergenceClosure.md`, which
orders thirteen patches easiest first and records the policy they follow:
avro-cpp's behaviour is the bridge's default, and every deviation from it is
reachable through a knob. `kKnownDivergences` is down from 11 entries to 6; the
pin in `KnownDivergenceTableSizeIsPinned` moves with it.

**Tier B is done too**, and it is the only bridge-side patch in the series:
`SetRejectTrailingBytes` in `avro_bridge.h`, off by default, because
apache-avro's `from_avro_datum` already stops at the end of the first datum like
avro-cpp and the rejection was the bridge's own addition at `rust/datum.rs`. It is
also the only patch that **removes** a check the bridge shipped with, so the knob's
`true` value is covered by `avro_bridge_strict_test` -- the setting is a `OnceLock`
and no test can reset it, so the strict value needs a second process rather than a
second test.

Closing it deleted two things rather than leaving them to rot: the
`TRAILING_BYTES` divergence ID, and `TrailingBytesExplainIt` in
`fuzz/differential_test.cc`, which labelled a bridge rejection as trailing bytes
whenever avro-cpp re-encoded shorter than its input. That never looked at why the
bridge rejected, and with the bridge no longer rejecting over leftovers it would
have named the wrong cause.

**Tier A is done**: four unconditional apache-avro patches, no knobs, because for
all four avro-cpp is the stricter engine and apache-avro's behaviour was wrong
rather than merely lenient. They stack in this order on top of the non-UTF-8 and
uuid patches, all in orva's `patches/`:

| patch | closes |
| --- | --- |
| `apache-avro-0.21-strict-eof.patch` | `bridge-lenient` / "EOF reached" |
| `apache-avro-0.21-empty-union.patch` | `schema-acceptance` / "bad node of type union" |
| `apache-avro-0.21-empty-enum.patch` | `schema-acceptance` / "bad node of type enum" |
| `apache-avro-0.21-empty-decimal.patch` | `reencode-failed` / "decimal sign extension 0" |

Three of the four are **not additive** for other consumers of the crate, and each
had upstream tests asserting the behaviour it changes. That is recorded per patch
in `patches/README.md`; do not describe this series as purely additive.

Tier A also turned up two defects that are not bridge divergences:

- **A container file cut inside a block-count varint reads as a clean end of
  iteration**, 2 items and no error. Found by reading, not by the fuzzer, because
  the harness has no object-container coverage. A5 in the spec, with its
  reproducer; it needs a differential measurement before a fix.
- **avro-cpp fabricates an array item without changing the length.** Finding 13
  in `fuzz/FINDINGS.md`, from the Tier A checkpoint run. An avro-cpp bug for
  upstream, and the more dangerous shape of finding 9, since the two lengths
  agree so the caller gets no signal at all. Six bytes reproduce it.

Both fixed harness bugs on the way out, and the second is the more important:

- `Comparer::CompareArray` attributed an array-framing difference to `ARRAY_LEN`
  only when the lengths differed, and reported the equal-length case through the
  unscoped `VALUE_TYPE_MISMATCH` and `SCALAR_VALUE` tags. `ARRAY_ITEM_FABRICATED`
  now names it, keyed on avro-cpp holding `null` where the item schema is not
  `null`, so a real value divergence inside an array is still reported as one.
- **`Comparer` reported cascades as independent findings.** Once the two engines
  read a different number of array items they are at different offsets in the same
  buffer, so every later field reads different bytes. The comparer carried on and
  reported each, so one suppressed `ARRAY_LEN` on an `array` of `null` surfaced as
  ten thousand `SCALAR_VALUE` reports about the field that followed it. It now
  stops the walk at the first difference that proves the byte counts diverged, and
  sets that flag even when the finding is suppressed. This is why
  `Differential.DecodersAgreeOnArbitraryBytes` now passes in unit-test mode and
  `ctest` has **five** non-passes rather than six.

**Expected `ctest` state is now 249-250/255.** The five non-passes are
four `Differential.*` properties that still fail by design plus
`AvrogencppTestReservedWords` reporting "Not Run". Do not silence the four.
Entries 1 to 10 came from the first runs; 11 to 15 are in the second table below,
from the parallel hour-long runs.

| # | divergence | direction |
| --- | --- | --- |
| 1 | empty buffer decodes to a record of fabricated nulls | **CLOSED**, strict-eof patch |
| 2 | empty union `[]` and empty enum | **CLOSED**, two patches |
| 3 | `duration` fixed re-renders as `{"type":{"type":"fixed",...}}`, name and namespace dropped | bridge output unreadable by avro-cpp |
| 4 | apache-avro panics at encode time on a duplicate full name | bridge panics, contained by `catch_panic` |
| 5 | namespace `ns..bad` with an empty component | avro-cpp accepts |
| 6 | non-UTF-8 in a `string` | **read side CLOSED**, bridge default; write side open |
| 7 | text that is not a well-formed uuid | **CLOSED**, bridge default |
| 8 | trailing bytes after a single datum | **CLOSED**, bridge-side, knob |
| 9 | truncated array block, avro-cpp returns 29 of 30 declared items | avro-cpp accepts |
| 10 | oversized `vector::resize`, avro-cpp throws `std::length_error` | **untriaged, not in any table** |

Entries 6 and 7 closed without a patch of their own. Both patches were already in
`patches/` and off by default, and `install_avro_cpp_defaults` now turns them on,
so the bridge accepts a non-UTF-8 `string` on decode and leaves a `uuid`
uninterpreted, as avro-cpp does. Entry 6's **write** side is untouched:
`create_string` (`rust/value.rs:66`) validates UTF-8 whatever the setting says, so
`AvroValue::CreateString("\xff\xfe")` still fails, which is what
`Differential.D1NonUtf8StringIsRejectedByTheBridge` pins and what keeps the
harness's acceptance test meaningful.

Finding 10 is **triaged**: it is `avro::GenericReader::read` (`Generic.cc:112`)
resizing an array or map to its declared block count before reading any item,
with no check against remaining input and no allocation ceiling on the avro-cpp
side. Symbolized during the one-hour run, where it also appeared as a map:
`vector<pair<string, GenericDatum>>::resize` requesting 837 GB from 27 bytes, and
1.6 GB from five bytes in another case. `fuzz/FINDINGS.md` finding 9 has the
detail. It is an avro-cpp bug for upstream, guarded in the harness rather than
suppressed.

Five more divergences came out of the parallel hour-long runs, all written up in
`fuzz/FINDINGS.md` as findings 8 to 12:

| # | divergence | direction |
| --- | --- | --- |
| 11 | `GenericDatum(NodePtr)` recurses to SIGSEGV on a record-only name cycle | avro-cpp crashes |
| 12 | declared block count reserved before reading items | avro-cpp allocates unboundedly |
| 13 | vertical tab and form feed accepted as JSON whitespace | avro-cpp accepts |
| 14 | array of `null`: both accept, lengths differ, and differ by build | **value divergence** |
| 15 | a 16-byte string under `uuid` is read as a binary uuid | **CLOSED**, bridge default |

Number 14 is the one to look at next: a value divergence where neither engine
errors, so a caller gets no signal at all, and the counts move between the ASan
and plain builds, which means at least one side's length does not come from the
input alone.

Number 15 closed without a patch. The uuid-as-string patch was already in
`patches/`, and `install_avro_cpp_defaults` now turns it on, so the bridge leaves
the annotation uninterpreted as avro-cpp does. `avro_bytes_fuzz_test.cc` set the
setting itself and had always seen the engines agree; `fuzz/differential_test.cc`
did not, so it was measuring the crate default rather than the bridge's.

`DecodersAgreeOnArbitraryBytes` also grows its resident set monotonically under
load, 112 MB to 847 MB over eighteen minutes, which a corpus of 1,789 small inputs
does not explain. `detect_leaks=0` is in force everywhere, so LSan is not
watching. See the end of `fuzz/FINDINGS.md`; it is unexplained.

Separately, `doc/CanonicalFormBug.md` records a spec-conformance bug in
`CanonicalForm()` and the Rabin fingerprint. It is **not** a divergence, because
avro-cpp 1.11.4 has no canonical-form or fingerprint API at all, so there is
nothing to compare. Keep it out of the differential harness.

## Scope rule

avro-cpp is the reference wherever it has a counterpart. Where the bridge adds
API avro-cpp never had, there is nothing to compare and the surface is out of
scope for a differential harness however wrong its behaviour looks. Such a bug
is a plain bridge bug and belongs in its own document.

## Claims I got wrong, corrected here so they are not repeated

1. **"efb7162 closed D1 and D2."** It did not. It closed D1's read side only;
   `create_string` still rejects on write at `rust/value.rs:75` on `main`. For
   D2 it added `insert_map_entry` in `rust/decode.rs` which **rejects**
   duplicates, where avro-cpp keeps both, so the two still disagree. `map_put`
   is untouched and still collapses silently at `rust/value.rs:560-569`. Both
   pinning tests exercise the write side, so both describe live behaviour.
2. **"24 bytes aborts the process deterministically."** Too strong. The bridge's
   ceiling compares a declared collection **count** against a limit expressed in
   **bytes**, so a count under 64 MiB passes and the map reserved costs about
   166 bytes per entry. Whether that reservation aborts depends on host memory:
   with `max_allocation_size_mb=1024` ASan refuses it and Rust aborts, and on
   this host with default options overcommit satisfies it and the decode returns
   a clean error. Only the ceiling arithmetic is deterministic, so only that is
   asserted.
3. **"The canonical-form bug is the most consequential finding."** It is not a
   divergence at all, see the scope rule.
4. **"There is no Bazel anywhere."** True of `orva`, but upstream
   `~/safe-bindings` is Bazel-based. It does not change the conclusion that this
   repo stays on CMake, since avro-cpp has no Bazel build.

5. **"The array-of-`null` lengths are 32 and 26."** Those are the ASan fuzzing
   build's numbers. The plain build gives 2 and 0 for the input the fuzzer
   printed. Since the counts move with the build, at least one engine's length
   does not come from the input alone, and the pinned test asserts the
   disagreement rather than either pair. Finding 15's write-up says so.
6. **"The death test's child dies by SIGSEGV, or SIGABRT under ASan."** Neither,
   in the fuzzing build: it links absl's failure signal handler, which prints a
   trace and exits with a *code*, so a `WIFSIGNALED` predicate fails there while
   passing in the plain build. `DiedRatherThanReturned` now accepts any death.
   Caught only because the test was run in both trees; a death test that passes
   where you developed it can still fail under the sanitizer.

## Traps

- **`options.suppressions` is never set by any `fuzz/` property.**
  `NormalizeOptions{}` defaults it to `nullptr`, so the `SanitizeUtf8` path at
  `fuzz/ir.cc:321-324` is dead during fuzzing. Only `fuzz/ir_test.cc` sets it,
  which is why its unit tests pass and hid the gap. Suppressing D1 therefore
  drops the input instead of repairing it. Safety is unaffected, since no
  comparison runs, but `report.md` section 5 and `fuzz/README.md` describe the
  repair as if it happens. **Unfixed.**
- **13 of the 39 divergence IDs in `fuzz/suppress.h` have no reporting site**,
  so nothing can ever report them. They are placeholders for unbuilt properties
  (containers, resolution, streaming, byte identity) plus register entries
  D5 to D9. The table is a vocabulary of difference categories, not a list of
  discovered bugs, and its header comment calling the non-`D` entries "findings"
  invites exactly that misreading. **Unfixed.**
- **`allocator_may_return_null=1` does not cover every oversized allocation.**
  It handles a request above `max_allocation_size_mb`, which returns null and
  becomes a `std::bad_alloc` the harness can report as a verdict. A request too
  large to map at all takes ASan's out-of-memory path instead, which aborts
  regardless of the flag: an 837 GB `vector::resize` ended a run that way. Set
  `max_allocation_size_mb` as `fuzz/README.md` prescribes, and note the asymmetry
  -- the bridge's half cannot lean on it, because Rust aborts on a failed
  allocation instead of throwing.
- **`AVRO_FUZZ_SUPPRESS` does not reach `avro_bytes_fuzz_test.cc`.** That file
  predates `fuzz/suppress.h` and has its own table plus its own variable,
  `AVRO_BYTES_FUZZ_SKIP`, with `TAG[:substring]` entries. Muting a divergence in
  one harness leaves the other reporting it.
- **`--continue_after_crash=true` does not keep a property alive.** The FuzzTest
  engine runs these in-process, so a gtest assertion aborts before its crash
  handling applies. Suppression is the only way past a known finding.
- **The value-bearing well-formedness invariant applies to every node, whatever
  its kind.** `ValueBearingTreesAreWellFormed` checks `precision`, scale and
  `fixed_size` on all of them, including nodes `Normalize` has collapsed to
  `kNull`, where those fields no longer mean anything. Anything added to
  `Normalize` that returns early has to normalise the scalars first; that
  ordering bug survived until an hour-long run hit it.
- **Coverage flags are easy to forget.** A target without
  `${AVRO_FUZZ_COVERAGE_FLAGS}` gets almost no feedback. Adding it to
  `avro_bytes_fuzz_test` took a 90-second run from 9 edges and a corpus of 20
  to 96 edges and 186.
- **Rust is uninstrumented.** Corrosion drives cargo, so C++ `-fsanitize=` and
  coverage flags never reach the staticlib. Roughly none of the decode logic is
  visible to the mutator. Phase 2, `corrosion_add_target_rustflags(rust
  -Cpasses=sancov-module ...)`, was judged feasible but is not wired up.
- **`SetMaxAllocationBytes` is first-call-wins.** Both fuzz binaries set it from
  a static initialiser so it is in force before any property runs. You cannot
  lower it later in a test.
- **Do not use `setrlimit(RLIMIT_AS)` under ASan.** ASan reserves terabytes of
  address space at startup, so any limit low enough to matter is already
  exceeded and the process dies on an unrelated mmap.
- **`ASAN_OPTIONS` in the environment overrides `__asan_default_options()`.**
  Do not rely on the latter for correctness. It was removed from
  `avro_bytes_fuzz_test.cc` for that reason.

## Next steps, in the order I would take them

1. **Chase finding 14, the array-of-`null` length disagreement.** Both engines
   accept, neither errors, and the counts differ *and* move between the ASan and
   plain builds, so at least one length does not come from the input alone. Read
   past the end of the buffer is the obvious suspect. This is the only value
   divergence found so far where the caller gets no signal whatsoever.
2. **Fuzz the depth axis.** Nothing generates nesting past 6: `kMaxDepth` is 5,
   `NormalizeOptions::max_depth` is 6, the schema-text seeds reach 2 or 3, and
   the free-form string domains cap at 64 and 96 bytes. `AnyDeepChain(24)` exists
   but only `AvroIr.DeepChainsRenderWithoutOverflow` uses it, and 24 is below the
   first threshold that matters. Measured by hand: the bridge rejects from 128
   nesting levels, which is serde_json's default recursion limit, while avro-cpp
   accepts and only segfaults at about 11,600 nested arrays, 4,900 nested records
   or 35,000 unbalanced `[`. So there is an acceptance divergence across the whole
   band from 43 or 128 up to roughly 11,600, and an unbounded recursion in
   avro-cpp's `readEntity` (`JsonDom.cc:46`) above it, and no property covers
   either. A depth parameter on the schema-text domain covers the first cheaply;
   the second needs a fork-per-case pinning test, since the crash kills the
   process without unwinding. Note that FuzzTest's `--stack_limit_kb` defaults to
   128, so a depth property hits that soft limit long before the real stack.
3. **Wire `options.suppressions`** at the three `fuzz/` call sites, add a
   property asserting that a suppressed-D1 normalize yields a tree
   `ToBridgeValue` accepts, and correct the two documents.
4. **Make `min_children` mode-dependent** in `fuzz/ir.cc` so `kSchemaOnly` can
   emit empty unions and enums. Today `Normalize` legalises them away and the
   invariant tests assert that as intended behaviour, which locks the gap in
   place. The byte harness covers this from the other side, so this is about
   the tree harness's honesty rather than new findings.
5. **Reader-versus-writer schema resolution.** The register lists it as entirely
   uninvestigated, and findings 3 and 4 both land on schema handling. Sketched
   in the plan as an `EvolvePlan` edit applied to the writer tree.
6. **Object container round-trips** across the null, deflate and snappy codecs.
7. **Register entries.** The new divergences need `D13`+ entries in
   `doc/AvrocppDivergences.md` on `main`. That file does not exist on this
   branch, so nothing was added to it.

## Conventions worth keeping

- **absl for strings, not the standard library's spelling of it.**
  `absl::StrCat` and `absl::StrAppend` to build, `absl::StrFormat` /
  `absl::PrintF` / `absl::StrAppendFormat` to format, `absl::StrJoin`,
  `absl::StrSplit`, `absl::StripAsciiWhitespace`, `absl::StrContains`,
  `absl::AsciiStrToLower`, `absl::ConsumePrefix` for the rest. Not
  `std::to_string`, not `operator+` chains, not `snprintf` into a buffer, not
  `std::ostringstream`, not `.find(...) != npos`. `StrAppend` formats integers
  itself, which is what removes most `std::to_string` calls. absl has **no**
  string-repeat helper: `std::string(count, char)` for one character, a loop over
  `StrAppend` for anything longer. `avro_fuzz_ir` links `absl::strings` and
  `absl::str_format` for this.
- **Read-only string parameters are `absl::string_view`.** Not
  `const std::string&`, which forces an allocation at any call site holding a
  literal or a view. Construct a `std::string` at the point of storage instead.
  Two exceptions, both real: a `FUZZ_TEST` property function's signature has to
  match its domain, so `SchemaTextVerdictsAgree(const std::string&)` and
  `DecodersAgreeOnArbitraryBytes(const Node&, const std::string&)` cannot take
  views and will not compile if you change them; and watch the lifetime when a
  view is passed across a `fork()`, as `run_all_parallel.sh`'s probes do.
- **Comments earn their place or go.** Only *why*, or something without which the
  code cannot be followed. No restating what the line does, and no bird's-eye
  narration either. Prefer making the comment unnecessary: an `enum class` with
  named values beats a `// 0 = accepted, 1 = rejected` legend.

- **Compare encoded bytes, not values, for anything holding a float or double.**
  `AvroValue::operator==` delegates to Rust `PartialEq`, which is IEEE-754
  equality: two NaNs with identical bits compare unequal, and `-0.0` compares
  equal to `0.0` with different bits. `fuzz/compare.h` sets
  `strict_float_bits` for this reason; `AvroValueTest.EqualityIsIeeeEqualityForFloats`
  pins both directions. NaN payloads and signed zero survive both engines
  bit-exact, measured, so `FLOAT_NAN_PAYLOAD` and `FLOAT_SIGNED_ZERO` have never
  fired and neither is suppressed.
- **A set-once global is tested by building the test twice, not by a second test.**
  Every setting `avro_bridge.h` exposes is first-call-wins, so one process
  observes one value. `avro_bridge_test.cc` is built as itself and as
  `avro_bridge_strict_test` with `AVRO_BRIDGE_TEST_STRICT_SETTINGS`, and a
  `::testing::Environment` flips every setting in `SetUp` -- which is early
  enough, since gtest runs it before the first test body and nothing in the
  binary touches the bridge earlier. A test whose outcome depends on a setting
  branches on `kStrictSettings`; it is not duplicated. This replaced
  `rust/tests/reject_trailing_bytes.rs`, whose only reason to exist was that a
  `OnceLock` cannot be reset in-process.
- **The bridge's defaults are avrocpp's, and they are installed from one place.**
  `install_avro_cpp_defaults` in `rust/datum.rs`, called from `catch_panic`,
  turns on `non_utf8_string_as_bytes` and `uuid_as_string`, whose *crate*
  defaults are apache-avro's so that each patch in `patches/` stays additive for
  other consumers. Two defaults, both deliberate. It is in `catch_panic` because
  the entry points needing panic containment and the entry points needing these
  defaults are the same set: the untrusted-input surface. An entry point added
  without that guard skips both. The allocation ceiling is deliberately not
  installed there, since avrocpp has no ceiling at all and parity would mean
  removing a bound on untrusted input.
- Assert what was measured, including numbers that are not yet explained, and
  say in a comment that they are unexplained. `TruncatedArrayBlockIs...` asserts
  29 items without knowing why it is not 30.
- Do not embed raw byte blobs in tests. Build inputs from `Varint(...)` and
  named parts so a reader can see what the bytes mean.
- A pinned regression must not depend on host memory, thread scheduling, or
  environment variables. If the interesting behaviour does, assert the part that
  does not and record the rest in a comment.
- Prefer `absl::Status` over a local outcome type. avro-cpp throws and the
  bridge returns `absl::Status`, so `Guarded` flattens the throw into a
  `Status` and both sides compare with the same syntax.
- When a guard, table or option stops being needed, delete it. Two overlapping
  mechanisms where one would do is how the 70 lines of `operator new`
  replacement got in, and it silently gave up ASan's new/delete mismatch
  checking for nothing.
