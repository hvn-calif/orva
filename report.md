# Differential fuzzing: `avro_bridge` vs Apache avro-cpp

**Branch** `avro-diff-fuzz`, worktree `/home/hvn/orva-difffuzz`, based on
`84ce4afac2dec52b54738f83d1b04f3d44ef3e15`.
**Commits** `edfd1ce`, `7285ce4`. 3,792 lines of harness across 21 files.

---

## 1. What this is and why

`orva` is migrating C++ callers off Apache Avro C++ (avro-cpp) onto
`avro_bridge`, a Crubit/corrosion binding over the Rust `apache-avro` crate.
"The migration is correct" means the two agree on every input except where the
project has deliberately chosen to deviate.

Until now that claim rested on hand-written tests and a hand-maintained
register of twelve divergences (`doc/AvrocppDivergences.md`). That register is
explicit that whole surfaces are unexamined — schema parse leniency,
reader-versus-writer resolution, default values, aliases, canonical form and
fingerprints, container framing, recursion depth — and that an unexamined
surface must be treated as *unknown*, not as agreeing
(`doc/specs/AvrocppMigration.md:222`).

`doc/specs/AvrocppMigration.md:345-369` already specified differential fuzzing
as the tool for converting those unknowns into facts, and noted it was unbuilt.
Confirmed at the outset: there was no fuzzing infrastructure anywhere in the
repository. This work builds it.

## 2. Why this baseline

The target is a worktree pinned at `84ce4af`, twelve commits behind `main`,
chosen deliberately rather than for convenience. At that commit two divergences
that `efb7162` ("Stop losing data on two avrocpp-legal inputs") later closed are
still open:

- **D1** — a `string` holding non-UTF-8 bytes. avro-cpp accepts it unvalidated;
  the bridge rejects it on write (`rust/value.rs`, `create_string` calls
  `utf8(v)?`) and on read.
- **D2** — duplicate keys in a map. avro-cpp keeps both entries; the bridge
  collapses them last-write-wins (`entries.insert` into a `HashMap`).

So the baseline has a known answer key, and the harness has a falsifiable
acceptance criterion: **it must rediscover D1 and D2 cold, from an empty
corpus, without being told to look for them.** A differential fuzzer that cannot
find known bugs is not evidence of anything.

## 3. Result: the acceptance test passed

Both were found cold, in roughly a second each.

**D1** — schema `"string"`, payload `\xc2` (a truncated UTF-8 lead byte).

**D2** — a map with the duplicate key `"aa"`, reached after muting D1 via
`AVRO_FUZZ_SUPPRESS=D1,UUID_INVALID_REJECTED`, which is the documented triage
loop rather than a workaround.

## 4. Findings

Four beyond the two rediscovered, all on surfaces the register lists as
uninvestigated. Full detail and reproducers in `fuzz/FINDINGS.md`; each is
pinned by a regression test.

### 4.1 `CanonicalForm()` is not canonical for a logical type on a primitive

The most consequential finding.

```
schema:         {"type":"int","logicalType":"time-millis"}
CanonicalForm:  {"type":"int"}          <- the spec requires "int"
```

Avro's Parsing Canonical Form has a PRIMITIVES rule: a primitive is written in
its simple form. Three consequences, all verified rather than inferred:

- the canonical form violates the spec;
- it is not idempotent — reparsing `{"type":"int"}` and taking *its* canonical
  form yields `"int"`;
- the Rabin fingerprint is wrong. The annotated schema fingerprints as
  `8145260995063234477`; a plain int is `8247732601305521295` — the value in the
  Avro spec's own test data, and the one `rust/schema.rs` already asserts.

Since PCF deliberately strips `logicalType`, those two schemas must fingerprint
identically. They do not. This is not cosmetic: fingerprints are how schema
registries establish schema identity, so a wrong one means a missed cache hit or
a failed lookup, and it affects any schema using `date`, `time-millis`,
`timestamp-millis` and similar — which is most real schemas.

Pinned by `Differential.CanonicalFormIsNotCanonicalForLogicalTypes`.

### 4.2 apache-avro panics on a schema that defines a name twice

```
{"type":"record","name":"foo","namespace":"ns","fields":[
  {"name":"a","type":{"type":"record","name":"foo","namespace":"ns","fields":[]}}]}
```

Defining `ns.foo` twice is illegal Avro. **Both** engines accept it at parse
time — avro-cpp included, which was worth checking rather than assuming; my
first version of the test asserted avro-cpp would reject it, and that assertion
failed. The divergence is what happens next: avro-cpp carries on, while
apache-avro 0.21 panics at *encode* time rather than returning an error:

```
apache-avro-0.21.0/src/types.rs:369
Schemata didn't successfully resolve: Two named schema defined for same fullname: ns.foo
```

Two problems on the bridge side: the schema should have been rejected at parse
time rather than accepted and blown up later, and a malformed schema should
produce an error rather than a panic.

`catch_panic` contains it, so callers see an `absl::Status` and the process
survives. But that guard is the only thing between an untrusted schema and a
process abort, and `rust/vec_u8.rs` already notes apache-avro panics on some
malformed input — so any entry point missing the guard is a denial of service on
attacker-supplied schemas.

Pinned by `Differential.DuplicateFullNameParsesThenPanicsOnEncode`.

### 4.3 avro-cpp accepts a malformed namespace

`{"type":"fixed","name":"B","namespace":"ns..bad","size":16}` — `ns..bad` has an
empty middle component, which the spec disallows. avro-cpp accepts it; the
bridge rejects it. Direction is bridge-stricter, so nothing is silently
mis-decoded, but avro-cpp will ingest schemas other implementations reject, and
data written under such a schema may not be readable elsewhere. Arguably an
avro-cpp bug to report upstream.

### 4.4 The bridge rejects uuid text avro-cpp keeps verbatim

`{"type":"string","logicalType":"uuid"}` holding `\xbd`. avro-cpp treats a uuid
as an ordinary string and stores anything; the bridge runs
`uuid::Uuid::parse_str` and refuses. Same shape as D1 but a distinct site not
covered by D1's fix. A quieter second half: when the bridge *does* accept, it
re-emits the lowercase canonical form, so `0F9A…` becomes `0f9a…` and
`urn:uuid:` prefixes are dropped (`UUID_TEXT_NOT_PRESERVED`).

## 5. Harness design

**One IR tree, not schema-then-value.** A single recursive `Node` carries both
the Avro type and the payload; three lowerings derive the schema JSON, the
bridge `AvroValue` and the avro-cpp `GenericDatum` from it. "The value matches
the schema" then holds by construction, the mutator has one structure, and
shrinking stays coherent. The alternative — generate a schema, then build a
value domain for it via `FlatMap` — would rebuild a type-erased domain graph per
execution, discard value-side corpus progress on every schema mutation, and
shrink incoherently because shrinking a schema orphans its value.

**Depth is a hard structural bound.** `AnyTree` stacks finite `Domain<Node>`
layers rather than using `DomainBuilder`, whose own reference states "Recursion
limit for recursive domains is not implemented yet". An unconditionally
recursive children domain has a branching factor above one and therefore
unbounded expected size; a deep draw blows the stack while *constructing the
input*, which reads as a harness crash rather than a finding. My throwaway test
generator reproduced exactly that failure before I bounded it.

**Mode resolution at lowering time.** Every size-dependent choice is stored as
an intent plus a raw value — `IndexMode`, `ScaleMode`, `NameStrategy` — and
resolved against the real structure while lowering. Every IR value is therefore
total, so no `Filter` is needed and no generated input is wasted, while
deliberately out-of-range indices stay reachable.

**Suppression removes the input class, not the report.** Divergences that are a
property of the input (D1, D2) are removed by `Normalize` *before* either
lowering runs, so both engines provably see identical input and a muted
divergence can never manufacture a false agreement. Everything else is skipped
at comparison time. Unknown IDs are fatal at startup so a typo cannot leave you
believing something was muted. `fuzz/suppressions.txt` ships empty on purpose.

**The oracle is stricter than the one on `main`.** `avro_compare::ValuesEqual`,
which lands twelve commits later, treats a `String` and a `Bytes` with equal
payload as equal — correct there, because by then the projected decoder returns
`Bytes` for invalid UTF-8. Importing that leniency here would have masked D1.
Floats are compared bit-for-bit, so NaN payloads and signed zero are
significant; the bridge's own `operator==` cannot be used for floats at all,
since it delegates to Rust `PartialEq` where `NaN != NaN` would report a
spurious difference and `-0.0 == 0.0` would hide a real one.

**Two binaries, for triage.** `avro_ir_fuzz_test` links neither engine;
`avro_differential_fuzz_test` links both. If a sanitizer report's top non-harness
frame is in `avro::`, it is an avro-cpp finding to report upstream, not a bridge
regression — we are fuzzing a 2021-era C++ parser with clang 21.

**Value comparison only, never bytes.** Container sync markers are random and
Rust `HashMap` iteration order makes encoded map bytes vary between runs (D3), so
a byte-level assertion would be flaky rather than informative.

Two guarantees deliberately *not* attempted: hash-colliding map keys are
unreachable by construction (SipHash-1-3 with a per-process random seed, so
collisions cannot be precomputed and any dependent behaviour would not
reproduce), and name references are confined to schema-only mode, because
instantiating a value for a recursive reference requires both lowerings to make
the same stopping decision.

## 6. Build integration

Several things had to be fixed before anything could run. All are in
`CMakeLists.txt` with the reasoning inline.

**The baseline did not build at all.** Not caused by this change: Crubit's
binding generation emits references to `serde::__private228` and a
`libc::tcp_info` layout assertion that does not hold on aarch64. Both disappear
at serde 1.0.229 and libc 0.2.189 — the versions `main` already resolves. This
is the only change to the bridge's own dependencies, and it is unrelated to
fuzzing.

**Abseil could not be fixed by declaration order.** `FetchContent_Declare`
de-duplicates by *name*. FuzzTest declares abseil as `abseil-cpp`; both this
repo and Crubit declare it as `absl`. Two names never dedupe, so abseil would be
`add_subdirectory`'d twice and configure would fail on duplicate `absl_base`
targets. The fix is to own the `abseil-cpp` name, pinned to `20260526.0`, and
answer Crubit's `find_package(absl)` with a stub written into
`CMAKE_FIND_PACKAGE_REDIRECTS_DIR`. Verified: exactly one `abseil-cpp-src`, no
`absl-src`.

**Crubit is now pinned.** Corrosion declares it with `GIT_TAG latest`, a moving
tag, which moved during this work and broke the build outright.

**avro-cpp is `EXCLUDE_FROM_ALL`.** It registers its own test executables with
no `BUILD_TESTING` guard, and `AvrogencppTestReservedWords` does not compile
under clang 21 (avrogencpp emits a generated header using `constinit` where
clang rejects it). Only the library is wanted, and our targets link `avrocpp_s`.

GoogleTest moved to `v1.17.0` to match FuzzTest, and its `FIND_PACKAGE_ARGS` was
dropped. Sanitizer flags are split: ASan globally and before any
`FetchContent_MakeAvailable`, because abseil's containers change layout under
`ABSL_HAVE_ADDRESS_SANITIZER` and disagreeing translation units are an ODR
violation; coverage and UBSan per target.

## 7. Verified

- Baseline worktree builds; all 54 pre-existing bridge tests pass.
- Same, with abseil pinned to `20260526.0` and GoogleTest at 1.17 — the
  highest-risk step, since Crubit tracks abseil head.
- Unit-test mode: 12 generator-level properties and tests pass; 5 pinning tests
  pass; `ctest` is 145/149.
- Fuzzing mode (`-DFUZZTEST_FUZZING_MODE=ON`, ASan + coverage):
  **549,195 runs in 60s, ~9,159/sec, 326 edges covered, corpus grown to 468.**
- `Normalize` invariants over 3,000 adversarial trees: depth and node bounds,
  idempotence, unique record field names, non-empty enums, no nested unions,
  scale within precision.
- D2 reachable (4,138 duplicate keys generated across the sample) and fully
  removed under suppression; D1 invalid UTF-8 survives without suppression.

The three differential properties **fail by design** at this commit — that is
the acceptance criterion, documented in `fuzz/README.md` so nobody silences them
by editing `suppressions.txt`.

Two bugs in my own code were caught by these checks rather than by inspection:
`Normalize` was not idempotent (demoting a nested union to null left its
children attached), and FuzzTest's generic aggregate printer walked every field
of every node, slow enough on a real tree to look like a hang during shrinking —
fixed with an `AbslStringify`.

## 8. Not done, and open questions

**Not built**, all scoped in the plan: object-container round-trips across the
null, deflate and snappy codecs; reader-versus-writer schema resolution via an
`EvolvePlan` edit applied to the writer tree; the deep-nesting termination
property. Resolution is the one to do next — the register lists it as entirely
uninvestigated, and finding 4.2 suggests the schema-handling paths are where the
bugs are.

**Open question worth resolving before trusting the schema-side properties
further.** The fuzzer produced a duplicate-fullname schema even though
`lower_schema.cc` uniquifies names against the defined set, and
`SchemaJsonIsStructurallySound` asserts no duplicate definitions and passes.
Either the generator has a leak that property does not catch, or the duplicate
arises inside apache-avro's own schemata resolution rather than in the emitted
JSON. I have not determined which. Finding 4.2 stands either way, since it
reproduces from a hand-written schema.

**Phase 1 instruments C++ only.** Corrosion drives cargo, so C++ `-fsanitize=`
flags never reach the Rust staticlib: roughly none of the decode logic is
visible to the mutator, and the counters amount to "which entry point was called
and did it return ok". This is survivable only because generation is
structure-aware — the oracle does the work coverage feedback would otherwise do.
Rust heap errors are still caught, since Rust uses the system allocator and ASan
intercepts it. Phase 2 (`corrosion_add_target_rustflags(rust
-Cpasses=sancov-module …)`) was verified as feasible during design but is not
wired up.

**Environment.** `llvm-symbolizer` is still not installed; ASan reports will be
bare hex addresses until `sudo dnf install llvm` (the packaged 21.1.8 matches
clang). The machine has 7 GB of RAM, so builds need `--parallel 2` to 4, not
`-j10`.

## 9. Running it

```sh
cmake -S . -B build -DAVRO_BUILD_FUZZERS=ON
cmake --build build --parallel 4
ctest --test-dir build -R 'AvroIr|Divergence|Suppression|Differential'

cmake -S . -B build-fuzz -DAVRO_BUILD_FUZZERS=ON -DFUZZTEST_FUZZING_MODE=ON
cmake --build build-fuzz --parallel 2
export ASAN_OPTIONS='detect_leaks=0:allocator_may_return_null=1:max_allocation_size_mb=1024'
./build-fuzz/fuzz/avro_differential_fuzz_test \
    --fuzz=Differential.DatumCircleAgrees --fuzz_for=10m
```

`allocator_may_return_null=1` matters: Avro is length-prefixed, so the mutator
produces multi-gigabyte lengths within minutes, and without it ASan hard-aborts
and FuzzTest records each one as a crash.

To dig past a known finding, mute it for the session rather than editing the
file: `AVRO_FUZZ_SUPPRESS=D1,D2,UUID_INVALID_REJECTED`.
