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

`llvm-symbolizer` is still not installed, so ASan reports are bare hex
addresses until `sudo dnf install llvm`.

## Expected state of the test suites

`avro_bytes_fuzz_test`: **13/13 pass** with no environment variables, and also
under `ASAN_OPTIONS=detect_leaks=0` and with `allocator_may_return_null=1`.
If it fails, the fuzz properties found something new, which is the design.

`ctest` on the whole project: **163/169**, and `ctest -R AvroBytes` is 13/13.
The six non-passes are the five `Differential.*` properties in `fuzz/`, which
fail by design at this commit, plus avro-cpp's own
`AvrogencppTestReservedWords` reporting "Not Run" because that subdirectory is
`EXCLUDE_FROM_ALL`. Do not silence the five.

`report.md` section 7 says 150/156. That was measured before
`avro_bytes_fuzz_test` was added to the build; the count above is current.

## Divergences found

Ten. Each has a test in `avro_bytes_fuzz_test.cc` or `fuzz/differential_test.cc`
named after it. Full detail in `fuzz/FINDINGS.md`.

| # | divergence | direction |
| --- | --- | --- |
| 1 | empty buffer decodes to a record of fabricated nulls | bridge accepts |
| 2 | empty union `[]` and empty enum | bridge accepts |
| 3 | `duration` fixed re-renders as `{"type":{"type":"fixed",...}}`, name and namespace dropped | bridge output unreadable by avro-cpp |
| 4 | apache-avro panics at encode time on a duplicate full name | bridge panics, contained by `catch_panic` |
| 5 | namespace `ns..bad` with an empty component | avro-cpp accepts |
| 6 | non-UTF-8 in a `string` | avro-cpp accepts |
| 7 | text that is not a well-formed uuid | avro-cpp accepts |
| 8 | trailing bytes after a single datum | avro-cpp accepts |
| 9 | truncated array block, avro-cpp returns 29 of 30 declared items | avro-cpp accepts |
| 10 | oversized `vector::resize`, avro-cpp throws `std::length_error` | **untriaged, not in any table** |

Finding 10 surfaced in a long run as `vector::_M_default_append` with schema
`{"type":"array","items":"int"}` and 10 to 14 bytes of payload. It needs the
same treatment as the others: triage, an entry in `kKnownDivergences`, a test,
and a bump of the count in `KnownDivergenceTableSizeIsPinned`.

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

1. **Triage finding 10** and add it to the table with a test.
2. **Wire `options.suppressions`** at the three `fuzz/` call sites, add a
   property asserting that a suppressed-D1 normalize yields a tree
   `ToBridgeValue` accepts, and correct the two documents.
3. **Make `min_children` mode-dependent** in `fuzz/ir.cc` so `kSchemaOnly` can
   emit empty unions and enums. Today `Normalize` legalises them away and the
   invariant tests assert that as intended behaviour, which locks the gap in
   place. The byte harness covers this from the other side, so this is about
   the tree harness's honesty rather than new findings.
4. **Reader-versus-writer schema resolution.** The register lists it as entirely
   uninvestigated, and findings 3 and 4 both land on schema handling. Sketched
   in the plan as an `EvolvePlan` edit applied to the writer tree.
5. **Object container round-trips** across the null, deflate and snappy codecs.
6. **Register entries.** The new divergences need `D13`+ entries in
   `doc/AvrocppDivergences.md` on `main`. That file does not exist on this
   branch, so nothing was added to it.

## Conventions worth keeping

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
