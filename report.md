# Differential fuzzing: `avro_bridge` vs Apache avro-cpp

**Branch** `avro-diff-fuzz`, worktree `/home/hvn/orva-difffuzz`, based on
`84ce4afac2dec52b54738f83d1b04f3d44ef3e15`.
**Commits** `edfd1ce`, `7285ce4`. 3,792 lines of harness across 21 files.

---

## 0. Glossary

Every short form used in this document, other than ones assumed universally
known (JSON, UTF-8, RAM, NaN, ID).

| Term | Expansion | What it is here |
| --- | --- | --- |
| ASan | AddressSanitizer | clang's memory-error detector, enabled globally on the fuzzing build. |
| avro-cpp | Apache Avro C++ | the incumbent C++ Avro implementation, the thing we compare against. |
| `avro_bridge` | (a name) | the replacement: a Crubit/corrosion binding over the Rust `apache-avro` crate. Called "the bridge" below. |
| Crubit | (a name) | Google's C++/Rust interop generator, used to produce the bridge's C++ API. |
| corrosion | (a name) | the CMake plugin that drives `cargo` for the Rust half of the build. |
| D1 | (a name) | divergence register entry 1: a `string` holding non-UTF-8 bytes. Also a suppression ID accepted by `AVRO_FUZZ_SUPPRESS` (`fuzz/suppress.h`). |
| D2 | (a name) | divergence register entry 2: duplicate keys in a map. Also a suppression ID. |
| D3 | (a name) | divergence register entry 3: encoded map byte order is unstable, because Rust `HashMap` iteration order varies per process. Not a suppression ID; it is why the oracle compares values and never bytes. |
| divergence register | (a name) | `doc/AvrocppDivergences.md` on `main`: the hand-maintained list of known, accepted differences between the two implementations. |
| FuzzTest | (a name) | Google's property-based fuzzing framework, the harness's test driver. |
| intermediate representation | (a name) | the harness's single recursive `Node` tree (`fuzz/ir.cc`), the one generated structure that all three lowerings derive from. Abbreviated `ir` only inside identifiers such as `avro_ir_fuzz_test`. |
| lowering | (a name) | a function turning that tree into one concrete artifact: schema JSON, a bridge `AvroValue`, or an avro-cpp `GenericDatum`. |
| ODR | One Definition Rule | the C++ requirement that a given entity have one identical definition across all translation units. |
| oracle | (a name) | the comparison step that decides whether the two implementations agreed on one generated input. |
| suppression ID | (a name) | a name that mutes one known divergence for a run, via `fuzz/suppressions.txt` or `AVRO_FUZZ_SUPPRESS`. |
| UBSan | UndefinedBehaviorSanitizer | clang's undefined-behaviour detector, enabled per target. |

---

## 1. What this is and why

`orva` is migrating C++ callers off Apache Avro C++ (avro-cpp) onto
`avro_bridge`, a Crubit/corrosion binding over the Rust `apache-avro` crate.
"The migration is correct" means the two agree on every input except where the
project has deliberately chosen to deviate.

Until now that claim rested on hand-written tests and a hand-maintained
register of twelve divergences (`doc/AvrocppDivergences.md`). That register is
explicit that whole surfaces are unexamined -- schema parse leniency,
reader-versus-writer resolution, default values, aliases, canonical form and
fingerprints, container framing, recursion depth -- and that an unexamined
surface must be treated as *unknown*, not as agreeing
(`doc/specs/AvrocppMigration.md:222`).

`doc/specs/AvrocppMigration.md:345-369` already specified differential fuzzing
as the tool for converting those unknowns into facts, and noted it was unbuilt.
Confirmed at the outset: there was no fuzzing infrastructure anywhere in the
repository. This work builds it.

## 2. Why this baseline

The target is a worktree pinned at `84ce4af`, twelve commits behind `main`,
chosen deliberately rather than for convenience. At that commit two divergences
the register documents are open in both directions:

- **D1** -- a `string` holding non-UTF-8 bytes. avro-cpp accepts it unvalidated;
  the bridge rejects it on write (`rust/value.rs`, `create_string` calls
  `utf8(v)?`) and on read.
- **D2** -- duplicate keys in a map. avro-cpp keeps both entries; the bridge
  collapses them last-write-wins (`entries.insert` into a `HashMap`).

Both were written down in the register before this harness existed, so the
harness has a falsifiable acceptance criterion: **it must rediscover D1 and D2
starting from an empty corpus, with no seed inputs and no test written to target
them.** A differential fuzzer that cannot find known bugs is not evidence of
anything.

**What `efb7162` twelve commits later actually changed**, since it is easy to
read that commit as having closed both and it did not:

| | what `efb7162` did | state on `main` |
| --- | --- | --- |
| D1, read | non-UTF-8 `string` decodes to `Value::Bytes`; `get_string` accepts it | closed |
| D1, write | untouched | **still rejects**: `create_string` calls `utf8(v)?`, `rust/value.rs:75` |
| D2, projected decoder | added `insert_map_entry` in `rust/decode.rs` | **rejects** duplicates. avro-cpp keeps both, so the two still disagree; the register calls the projected path "stricter than the other two" |
| D2, apache-avro paths | untouched | still collapses |
| D2, write | untouched | **still collapses silently**: `map_put` is `HashMap::insert`, `rust/value.rs:560-569` |

Two things follow. D2 was never made to agree with avro-cpp -- its decode path
swapped silent data loss for a loud rejection, a better failure mode but still a
difference. And both pinning tests exercise the **write** side of their
divergence, which is the side neither fix touched, so both describe behaviour
still live on `main` rather than a historical bug.

## 3. Result: the acceptance test passed

Both were found under those conditions, in roughly a second each.

**D1** -- schema `"string"`, payload `\xc2` (a truncated UTF-8 lead byte).

**D2** -- a map with the duplicate key `"aa"`, reached after muting D1 via
`AVRO_FUZZ_SUPPRESS=D1,UUID_INVALID_REJECTED`, which is the documented triage
loop rather than a workaround.

## 4. Findings

Seven beyond the two rediscovered, all on surfaces the register lists as
uninvestigated. Full detail and reproducers in `fuzz/FINDINGS.md`; each is
pinned by a regression test.

**Scope rule applied here.** avro-cpp is the reference wherever it has a
counterpart. Where the bridge adds API avro-cpp never had, there is nothing to
compare, and the surface is out of scope for this harness however wrong its
behaviour looks. A spec-conformance bug in the bridge's `CanonicalForm()` and
fingerprint functions -- which avro-cpp 1.11.4 has no counterpart to at all --
was found while writing the harness and is written up separately in
`doc/CanonicalFormBug.md`. It is not a divergence and is not counted below.

### 4.1 The bridge decoded an empty buffer into fabricated nulls (CLOSED)

The most serious finding. Found by `DecodersAgreeOnArbitraryBytes` on its first
input.

```
schema: {"type":"record","name":"R","fields":[
           {"name":"a","type":"boolean"},{"name":"b","type":"boolean"}]}
input:  ""                     (zero bytes)

bridge:  ok, {"a":null,"b":null}
avrocpp: avro::decode: EOF reached
```

Two things were wrong. Decoding zero bytes should fail, and the value returned
did not inhabit its own schema -- `null` is not a legal value of `boolean`. A
caller handed a truncated message got a success status and a record of nulls
rather than an error.

It was type-dependent, which is what made it easy to miss: `"null"` correctly
accepted empty input (a null really is zero bytes), `"int"` and `"string"`
correctly rejected it, but `"boolean"`, any union, and any record built from
those fabricated values instead. The register's worst class is silent data loss;
this was its mirror image, manufacturing data that was never on the wire.

**CLOSED** by `apache-avro-0.21-strict-eof.patch`. Three arms of
`decode_internal` treated `ErrorKind::UnexpectedEof` as a value rather than an
error -- `Schema::Boolean` and `Schema::String` returned `Ok(Value::Null)`, and
`Schema::Union` returned `Ok(Value::Union(0, Null))` whatever branch 0 held. All
three now propagate the error, and `Schema::Null` still decodes from an empty
buffer because a `null` datum really does occupy zero bytes.

Triage turned up a **third site the table above could not reach**: a `string`
whose length prefix declares more bytes than are present. An empty buffer under
`"string"` already failed, at the missing length prefix, so nothing exercised a
truncated payload. `04 61` (length 2, one byte) decoded to `Value::Null` before
the patch.

Closing it also reversed an upstream behaviour rather than only adding to it.
`reader::tests::test_from_avro_datum_with_union_to_struct` (AVRO-3240) encoded
two of its record's five fields and relied on the union arm to fabricate the
other three, described in the test as simulating missing keys. That is what
schema resolution is for, not truncation, so the test now asserts the error.

Now pinned by `Differential.EmptyInputIsRejectedByBothEngines`,
`Differential.EmptyInputIsRejectedForBooleanAndUnion` and
`AvroBytes.TruncatedInputIsRejectedByBothEngines`, which assert the agreement.

### 4.2 Empty union `[]` and empty enum: bridge accepted, avro-cpp rejects (CLOSED)

`[]` was accepted by the bridge, re-rendered as `[]`, and rejected by avro-cpp
with "bad node of type union". Same for `{"type":"enum","name":"E","symbols":[]}`
and for `[]` nested as a record field type. So the bridge round-tripped a schema
avro-cpp cannot read.

Worth recording how long this went unseen: the tree-based generator **cannot**
produce it. `NormalizeChildren` tops an empty union up to one branch
(`ir.cc:257-265`), so no amount of running `SchemaVerdictsAgree` would have
reached it. Two bytes of schema text found it on the first input.

**CLOSED**, in two patches, because the two constructs guard differently.
`apache-avro-0.21-empty-union.patch` rejects an empty member list in
`UnionSchema::new`, which covers everything the crate can build since that
type's fields are crate-private. `apache-avro-0.21-empty-enum.patch` rejects an
empty symbol list in `parse_enum`, which covers the parse path only: `EnumSchema`
has public fields and a builder, so a Rust caller can still hand-build one.
Untrusted input arrives by parsing.

`["int"]` and a one-symbol enum stay legal on both sides, since index 0 is in
range for each, and both pinned tests assert that so neither fix can creep.

Pinned by `Differential.EmptyUnionIsRejectedByBothEngines`,
`Differential.EmptyEnumIsRejectedByBothEngines`,
`AvroBytes.EmptyUnionIsRejectedByBothEngines` and
`AvroBytes.EmptyEnumIsRejectedByBothEngines`.

### 4.3 The bridge re-renders a `duration` fixed in a shape avro-cpp cannot read

The most consequential finding for the migration itself.

```
in:   {"type":"fixed","name":"B","namespace":"ns","size":12,"logicalType":"duration"}
out:  {"type":{"type":"fixed","name":"duration","size":12},"logicalType":"duration"}
      avro-cpp: Json field "type" is not a string
```

Both engines parse the input. The rendering is where they part company, and it
goes wrong twice over:

- the fixed is nested inside `"type"` as an object, which avro-cpp rejects
  outright. A schema that has passed through the bridge can no longer be read
  by avro-cpp -- exactly the direction that breaks a partly-migrated
  deployment, where a bridge-side writer publishes a schema an avro-cpp-side
  reader must consume;
- name and namespace are dropped. `ns.B` comes back as `duration`, so schema
  identity does not survive the round trip even for a reader that can parse the
  output at all.

Found by `SchemasCrossParse` in unit-test mode and reproduced from a
hand-written schema. Pinned by
`Differential.DurationFixedRendersUnparseableByAvrocpp`.

### 4.4 apache-avro panics on a schema that defines a name twice

```
{"type":"record","name":"foo","namespace":"ns","fields":[
  {"name":"a","type":{"type":"record","name":"foo","namespace":"ns","fields":[]}}]}
```

Defining `ns.foo` twice is illegal Avro. **Both** engines accept it at parse
time -- avro-cpp included, which was worth checking rather than assuming; my
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
malformed input -- so any entry point missing the guard is a denial of service on
attacker-supplied schemas.

Pinned by `Differential.DuplicateFullNameParsesThenPanicsOnEncode`.

### 4.5 avro-cpp accepts a malformed namespace

`{"type":"fixed","name":"B","namespace":"ns..bad","size":16}` -- `ns..bad` has an
empty middle component, which the spec disallows. avro-cpp accepts it; the
bridge rejects it. Direction is bridge-stricter, so nothing is silently
mis-decoded, but avro-cpp will ingest schemas other implementations reject, and
data written under such a schema may not be readable elsewhere. Arguably an
avro-cpp bug to report upstream.

### 4.6 The bridge rejects uuid text avro-cpp keeps verbatim

`{"type":"string","logicalType":"uuid"}` holding `\xbd`. avro-cpp treats a uuid
as an ordinary string and stores anything; the bridge runs
`uuid::Uuid::parse_str` and refuses. Same shape as D1 but a distinct site not
covered by D1's fix. A quieter second half: when the bridge *does* accept, it
re-emits the lowercase canonical form, so `0F9A…` becomes `0f9a…` and
`urn:uuid:` prefixes are dropped (`UUID_TEXT_NOT_PRESERVED`).

### 4.7 Trailing bytes after a datum: avro-cpp ignores, the bridge rejects

Schema `"int"`, input `02 ff`. avro-cpp stops at the end of the first datum and
returns 1; the bridge rejects the remainder. Direction is bridge-stricter and
its behaviour is the more defensible of the two, since trailing bytes usually
mean framing has gone wrong. Recorded because callers who relied on avro-cpp
tolerating a padded buffer will hit it. Pinned by
`Differential.TrailingBytesAcceptedOnlyByAvrocpp`.

## 5. Harness design

**One intermediate-representation tree, not schema-then-value.** A single recursive `Node` carries both
the Avro type and the payload; three lowerings derive the schema JSON, the
bridge `AvroValue` and the avro-cpp `GenericDatum` from it. "The value matches
the schema" then holds by construction, the mutator has one structure, and
shrinking stays coherent. The alternative -- generate a schema, then build a
value domain for it via `FlatMap` -- would rebuild a type-erased domain graph per
execution, discard value-side corpus progress on every schema mutation, and
shrink incoherently because shrinking a schema orphans its value.

**And two byte-oriented properties, because that tree has a blind spot.**
Generating a tree means only ever testing structures the generator knows how to
build, and `Normalize` then legalises them further. Findings 4.1 and 4.2 are
both outside what it can reach: a lowered value always encodes to well-formed
bytes, so no generated input is ever truncated, and `NormalizeChildren` tops an
empty union up to one branch, so `[]` is unreachable however long the
tree-based properties run. `SchemaTextVerdictsAgree` takes schema *text*
straight from the mutator, and `DecodersAgreeOnArbitraryBytes` keeps the
generated schema but feeds arbitrary bytes as the payload. Both found a
divergence on their first input.

The reason this is a supplement rather than the whole design is the write path.
Byte-oriented fuzzing only exercises decode; D1 and D2 both live in
`create_string` and `map_put`, reachable only by *constructing* a value, so a
byte-only harness could not have run the acceptance test at all. The schema
text domain also mixes in every schema the tree generator can produce, since
neither engine's Rust half is instrumented and a mutator starting from noise
has nothing to climb.

**Depth is a hard structural bound.** `AnyTree` stacks finite `Domain<Node>`
layers rather than using `DomainBuilder`, whose own reference states "Recursion
limit for recursive domains is not implemented yet". An unconditionally
recursive children domain has a branching factor above one and therefore
unbounded expected size; a deep draw blows the stack while *constructing the
input*, which reads as a harness crash rather than a finding. My throwaway test
generator reproduced exactly that failure before I bounded it.

**Mode resolution at lowering time.** Every size-dependent choice is stored as
an intent plus a raw value -- `IndexMode`, `ScaleMode`, `NameStrategy` -- and
resolved against the real structure while lowering. Every generated value is therefore
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
payload as equal -- correct there, because by then the projected decoder returns
`Bytes` for invalid UTF-8. Importing that leniency here would have masked D1.
Floats are compared bit-for-bit, so NaN payloads and signed zero are
significant; the bridge's own `operator==` cannot be used for floats at all,
since it delegates to Rust `PartialEq` where `NaN != NaN` would report a
spurious difference and `-0.0 == 0.0` would hide a real one.

**Two binaries, for triage.** `avro_ir_fuzz_test` links neither engine;
`avro_differential_fuzz_test` links both. If a sanitizer report's top non-harness
frame is in `avro::`, it is an avro-cpp finding to report upstream, not a bridge
regression -- we are fuzzing a 2021-era C++ parser with clang 21.

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
at serde 1.0.229 and libc 0.2.189 -- the versions `main` already resolves. This
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
dropped. Sanitizer flags are split: AddressSanitizer (ASan) globally and before any
`FetchContent_MakeAvailable`, because abseil's containers change layout under
`ABSL_HAVE_ADDRESS_SANITIZER` and disagreeing translation units are a One
Definition Rule (ODR) violation; coverage and UndefinedBehaviorSanitizer
(UBSan) per target.

## 7. Verified

- Baseline worktree builds; all 54 pre-existing bridge tests pass.
- Same, with abseil pinned to `20260526.0` and GoogleTest at 1.17 -- the
  highest-risk step, since Crubit tracks abseil head.
- Unit-test mode: 12 generator-level properties and tests pass; 9 pinning tests
  pass; `ctest` is 150/156 (the 6 non-passes are the 5 differential properties
  that fail by design and avro-cpp's own `AvrogencppTestReservedWords`, which
  reports "Not Run" because the avro-cpp subdirectory is `EXCLUDE_FROM_ALL`).
- Fuzzing mode (`-DFUZZTEST_FUZZING_MODE=ON`, ASan + coverage):
  **549,195 runs in 60s, ~9,159/sec, 326 edges covered, corpus grown to 468.**
- `Normalize` invariants over 3,000 adversarial trees: depth and node bounds,
  idempotence, unique record field names, non-empty enums, no nested unions,
  scale within precision.
- D2 reachable (4,138 duplicate keys generated across the sample) and fully
  removed under suppression; D1 invalid UTF-8 survives without suppression.

All five differential properties **fail by design** at this commit -- that is
the acceptance criterion, documented in `fuzz/README.md` so nobody silences them
by editing `suppressions.txt`.

Two bugs in my own code were caught by these checks rather than by inspection:
`Normalize` was not idempotent (demoting a nested union to null left its
children attached), and FuzzTest's generic aggregate printer walked every field
of every node, slow enough on a real tree to look like a hang during shrinking --
fixed with an `AbslStringify`.

## 8. Not done, and open questions

**Not built**, all scoped in the plan: object-container round-trips across the
null, deflate and snappy codecs; reader-versus-writer schema resolution via an
`EvolvePlan` edit applied to the writer tree; the deep-nesting termination
property. Resolution is the one to do next -- the register lists it as entirely
uninvestigated, and findings 4.3 and 4.4 both land on schema handling, which is
where the bugs appear to be.

**Open question worth resolving before trusting the schema-side properties
further.** The fuzzer produced a duplicate-fullname schema even though
`lower_schema.cc` uniquifies names against the defined set, and
`SchemaJsonIsStructurallySound` asserts no duplicate definitions and passes.
Either the generator has a leak that property does not catch, or the duplicate
arises inside apache-avro's own schemata resolution rather than in the emitted
JSON. I have not determined which. Finding 4.4 stands either way, since it
reproduces from a hand-written schema.

**Phase 1 instruments C++ only.** Corrosion drives cargo, so C++ `-fsanitize=`
flags never reach the Rust staticlib: roughly none of the decode logic is
visible to the mutator, and the counters amount to "which entry point was called
and did it return ok". This is survivable only because generation is
structure-aware -- the oracle does the work coverage feedback would otherwise do.
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
