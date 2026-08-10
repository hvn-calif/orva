# Spec: Migrating a product from avrocpp to avro_bridge

Status: DRAFT - awaiting approval

Companion to `AvroSafeBinding.md`, which specifies the binding. This one specifies
how an existing avrocpp consumer gets onto it without breaking its users.

Reference implementation of avrocpp throughout: Apache avro-cpp `release-1.11.4`,
the revision `CMakeLists.txt` fetches for the benchmark. Line citations are against
that tree under `lang/c++/`. `apache-avro` citations are against the avro-rs revision
pinned in `rust/Cargo.toml`.

## Purpose & user problem

A large, long-lived, user-facing product parses untrusted Avro through avrocpp.
Replacing avrocpp with this binding moves that parsing into Rust. The safety
argument for doing so is not in question. The problem this spec addresses is the
other half: **the product's own test suite cannot tell you whether the swap is
safe for its users.**

Two independent reasons:

1. **Hyrum's Law.** Users have come to depend on observable avrocpp behavior that
   nobody specified and nobody meant to promise: byte layouts, map iteration order,
   exception text, and above all *inputs avrocpp accepted because it never checked*.
   The product's tests do not cover these, because until the implementation changed
   nobody knew they were contracts.
2. **Third-party coverage does not transfer.** The product exercises the slice of
   avrocpp it needs. Everything else about avrocpp's behavior is pinned by
   avrocpp's *own* test suite, and none of that coverage follows the swap. Those
   tests have to be ported.

The strictness asymmetry is the sharp edge. Where avrocpp was permissive and this
binding is correct, previously-working user data becomes a hard error. That is a
regression from the user's point of view no matter how defensible the new behavior
is in isolation.

## Success criteria

1. Every avrocpp symbol the product references is accounted for in the inventory
   below: mapped to a binding equivalent, or explicitly declared unused and dropped.
2. Every divergence in `doc/AvrocppDivergences.md` has a decision here, and every
   decision that affects user-visible behavior is either backed by production dual-run
   evidence or documented as an accepted change.
3. avrocpp's own test suite is ported for the features the binding has, and the
   ports pass across all three decode paths (see "Three decoders").
4. The differential fuzzers run in CI over a persistent corpus with zero
   unexplained divergences.
5. Production runs in `kBridge` mode with avrocpp unlinked. Until avrocpp is
   unlinked, the memory-safety goal is not met: the unsafe parser is still in the
   address space and still reachable.

## Scope

In scope, because the product uses all three:

- Generic datum path: `ValidSchema`, `GenericDatum`, `DataFileReader`/`Writer`,
  binary `Encoder`/`Decoder`.
- Specific path: `avrogencpp`-generated types, `Specific.hh`, `codec_traits`.
- Avro JSON wire codec: `avro::jsonEncoder` / `jsonDecoder`.

Out of scope:

- Avro RPC / protocol (`.avpr`), Trevni, IDL (`.avdl`).
- avrocpp's internal buffer and stream classes (`api/buffer/`, `StreamTests.cc`,
  `buffertest.cc`). No binding analogue exists and none is wanted; this repo has its
  own streaming design in `AvroStreamingIO.md`.
- Replacing `avrogencpp` the tool. See "Feature gap 2a" for why it can stay.

## Phase 0: call-surface inventory

Do this before anything else. The row count sets the size of every later phase, and
any avrocpp symbol with zero call sites is scope to delete rather than port.

Build it mechanically. Grep alone under-reports, because `Specific.hh` and generated
`codec_traits` reach avrocpp through templates that never name a symbol at a call
site:

1. `llvm-nm --undefined-only` over the product's object files, filtered to `avro::`,
   demangled. This is ground truth for what actually links.
2. A clang AST pass over every translation unit that includes `<avro/...>`, to
   attribute each undefined symbol to source locations and get call-site counts.
3. Grep the product for avrocpp exception *message substrings*
   (`"Invalid value for bool"`, `"Unknown codec"`, `"Sync mismatch"`,
   `"No such symbol"`, ...). Any hit is a place where error text is a contract and
   feeds D4.

Fill in per symbol:

| avrocpp symbol | Call sites | Binding equivalent | Gap | Notes |
|---|---|---|---|---|
| _(populate from step 1)_ | | | | |

## Three decoders

A constraint on everything below that does not exist in a normal migration. After
this change there are **three** decode paths that must agree, not two:

1. `rust/decode.rs`, a hand-written decoder for the projected and token-advance
   paths (`AvroProjection`, `DataFileReader::CreateWithProjection`).
2. `apache-avro` plus `patches/apache-avro-0.22-read-into.patch`, reached through
   `AvroDatumReader::DecodeInto` (`rust/datum.rs:163`) and the reuse paths.
3. Unpatched `apache-avro`, reached through `DecodeDatum` and
   `AvroDatumReader::Decode` (`rust/datum.rs:146`).

Every semantic test and every divergence decision has to land in all three or the
projected path drifts unobserved. Upstreaming the patch reduces this to two paths;
until then, treat "ran against all three" as part of the definition of done for any
ported test.

## Decisions on each divergence

The facts, citations, and severity for each item live in
`doc/AvrocppDivergences.md` under stable IDs `D1`-`D12`, plus the feature gaps and
the "Not yet investigated" list. This section carries only what to *do* about each,
so that the two documents cannot drift. Read that one first.

Three verdicts are available per item: **match avrocpp**, **deviate deliberately**
(with the user-visible consequence written down), or **policy knob**. Ship knobs only
where dual-run shows the input actually occurs, and delete the ones production proves
unnecessary.

### D1, non-UTF-8 strings: deviate from both, decided in its own spec

Invalid UTF-8 at a `Schema::String` position decodes to `Value::Bytes`, always, with
no error and no policy enum. Not avrocpp's behavior (it produced a string) and not the
binding's current behavior (it errors), but it preserves the property that matters:
byte-exact round-trip, unambiguously.

Full rationale, the six rejected alternatives, and the staged implementation are in
`doc/specs/AvroStringPolicy.md`. Two consequences that land outside that spec:

- `avro_compare` must normalize `Value::String` against `Value::Bytes` before
  comparing, or every correct decode of such a field reads as a dual-run divergence.
- Stage 1 of that spec, putting the field path and byte offset into the error, should
  ship before `kDualRun` is enabled. Without it a divergence report on this row is not
  actionable.

### D2, duplicate map keys: fix now, unconditionally

Reject the repeated key rather than letting it overwrite. This is the only item here
that needs no evidence to justify, because it can only fire on input that was already
being corrupted: a map with distinct keys decodes exactly as before.

**Status: done for the projected decoder, open upstream.** `insert_map_entry` in
`rust/decode.rs` covers both of that module's map sites. One patch entry closes both
apache-avro paths, since they share a single map-decode site. Until it lands, the
projected path is deliberately stricter than the other two, and
`duplicate_map_key_still_collapses_upstream` is the record of that gap: when the patch
lands, it flips to expecting an error.

### D3, map order and encoded bytes: OPEN, blocked on dual-run

The fix depends on whether anything downstream compares bytes, which is not yet known.

- If only run-to-run determinism is needed: sort keys at encode time in the bridge.
- If byte-identity with avrocpp is needed: insertion order must be preserved, which
  means an `IndexMap` change to `apache-avro` in `patches/`.

The dual-run re-encoded-byte comparison answers this. Do not guess it early. Note D10:
"match avrocpp" is two different targets depending on whether the call site used the
generic or the specific API.

### D4 and D5, error mechanism and text: match avrocpp selectively

Publish the `avro::Exception` to `absl::Status` mapping table below. Preserve message
text only for messages phase 0 step 3 proves the product matches on.

For D5, throw `avro::Exception` from the shim rather than reproducing avrocpp's
`std::out_of_range`, and note the change here. Reproducing a latent escape is not
worth compatibility.

| avrocpp throw site | Message | absl code | Preserve text? |
|---|---|---|---|
| _(populate from phase 0 step 3)_ | | | |

### D6, where enum and union checks happen: deviate, document

Same accept/reject verdict, so no data changes hands differently. The message and
failure-point differences roll up into D4.

### D7, array continuation blocks: deviate deliberately

Input that failed before now succeeds. Recorded so that nobody later "fixes" the
bridge toward avrocpp's behavior, which is inconsistent with avrocpp's own handling of
map blocks and array starts.

### D8, container codecs: freeze the write codec

Do not change the codec the product writes during the migration. Files it emits may be
read by other avrocpp-based consumers, which reject zstd. Deflate level also changes
bytes. Change codecs afterward as its own deliberate, separately-announced change.

Also worth a one-line correction to `rust/Cargo.toml`'s comment, which claims the
codec set matches avrocpp including zstd.

### D9, allocation ceiling: document, do not match

Call `SetMaxAllocationBytes` exactly once at process startup, from the product's own
init rather than from a library. Record the chosen bound here, and confirm before
enabling `kBridge` that no legitimate product datum exceeds it.

### D10, avrocpp's two map orders: input to D3

No independent decision. It constrains what "match avrocpp" can mean.

### D11, datum as JSON: implement the real thing

See feature gap 2b. `ToJsonString` is not a substitute, and if any user-visible output
goes through `jsonEncoder` a shape change is a visible break.

### D12, aliasing aborts: accept, and account for it

Note it in the product's runbook. The shim is where aliasing mistakes would be
introduced, so it gets the qa review pass the engineering rules call for.

### The "Not yet investigated" list

Each surface listed at the end of `doc/AvrocppDivergences.md` needs a decision *only
once it has been investigated*. Schema parse leniency and schema resolution are the
two largest and are the reason `SchemaTests.cc` and `CodecTests.cc` sit at priority 1
and 2 in the port register below. Treat an unexamined surface as unknown, not as
agreeing.

## Test port register

Source: `lang/c++/test/` in the avrocpp tree. Porting avrocpp's suite does two jobs:
tests that compile are parity checks, and tests that **cannot** compile are the
feature-gap list, discovered mechanically instead of by reading specs.

**The reference suite already builds and runs here.** Because `CMakeLists.txt` pulls
avrocpp in through `FetchContent` under `-DAVRO_BUILD_BENCHMARKS=ON`, avrocpp's own
test targets are registered in this project's CTest as tests 80-91, and Boost.Test is
already satisfied. Verified: 11 of the 12 build and pass unmodified, `CodecTests`
included, and the whole set runs in 1.34s.

    cmake --build . --target CodecTests DataFileTests SchemaTests unittest ...
    ctest -R "CodecTests|DataFileTests|SchemaTests|^unittest$"

The one exception is `AvrogencppTestReservedWords`, which fails to compile for a
reason unrelated to this migration: avrocpp's own generated
`cpp_reserved_words.hh` uses `constinit` in a position clang rejects. Ignore it.

This changes the porting method. Do **not** read a test and reason about what
avrocpp would do; run it, and where behavior is in question, add a case to the
avrocpp binary and observe. Every claim in `doc/AvrocppDivergences.md` was
established this way or by reading both sources, and every claim a port raises
should be too.

Placement rule: wire-format and schema semantics go in Rust tests (no FFI, and they
can exercise `rust/decode.rs` directly). API shape, error mapping, and object
lifetime behavior go in `avro_bridge_test.cc`.

| Source | Lines | Target | Priority | Notes |
|---|---|---|---|---|
| `CodecTests.cc` | 1808 | `rust/tests/` | 1 | The prize. A schema x operation matrix that generates random data, round-trips it, and replays each case while *skipping* elements and while *resolving* against a reader schema. Already shaped like a differential oracle. Port the case table verbatim so schema and "calls" strings stay comparable against upstream |
| `DataFileTests.cc` | 1148 | Rust + `avro_bridge_test.cc` | 2 | Container framing, sync markers, metadata, codecs, sync/seek semantics |
| `unittest.cc` | 1094 | Rust | 3 | Broad encoder/decoder and schema behavior |
| `SchemaTests.cc` | 503 | `rust/schema.rs` | 2 | Schema parse accept/reject. Highest-yield place to find leniency differences, and cheap to port |
| `CompilerTests.cc` | 93 | Rust | 3 | Plus the ~30 fixtures in `lang/c++/jsonschemas/` as a parse-parity corpus |
| `JsonTests.cc` | 208 | blocked | - | Needs gap 2b |
| `SpecificTests.cc` | 202 | blocked | - | Needs gap 2a |
| `AvrogencppTests.cc` | 279 | blocked | - | Needs gap 2a |
| `StreamTests.cc`, `buffertest.cc` | 1335 | skip | - | avrocpp-internal buffer/stream classes, out of scope |

Also port the Avro cross-language corpora from a full avro checkout:
`share/test/schema-tests.txt` (parse expectations) and the golden container files
`share/test/weather.avro`, `weather-snappy.avro`, `syncInMeta.avro`. Files written
by a foreign writer catch drift that self-round-tripping cannot. Note `share/` is
absent from the tree `CMakeLists.txt` fetches, because that declaration uses
`SOURCE_SUBDIR lang/c++`; fetch it separately.

A ported test that fails is a **divergence row until triaged**, not a bug. Some of
them will be new entries in `doc/AvrocppDivergences.md`.

## Feature gaps

### 2a. Specific types and avrogencpp

Do not write a new code generator. Everything `avrogencpp` emits, and all of
`Specific.hh`, depends only on the abstract `avro::Encoder` / `avro::Decoder` token
interfaces. The generated `codec_traits` specializations call `d.decodeEnum()`,
`e.encodeUnionIndex()`, `d.decodeInt()` and so on; see the generator's own emission
code at `impl/avrogencpp.cc:527-643`.

So: implement bridge-backed classes with those two interfaces, and existing
generated headers compile and work unchanged. The generated struct definitions are
plain data types that parse nothing.

`avrogencpp` the tool can keep running as a build step. It parses your own `.avsc`
at build time with trusted input, so it is not part of the untrusted-input surface
this migration exists to remove. Replacing it is a separate, later project.

**Spike before committing.** A token API is chatty where the value-tree API
amortizes: one FFI call per primitive rather than one per datum. Benchmark a
representative generated type against avrocpp using the existing harness, which
already builds and links avrocpp under `-DAVRO_BUILD_BENCHMARKS=ON`. If per-call
cost is prohibitive, the fallback is a Rust-side cursor returning batched token
runs, which is a larger design and needs its own spec.

### 2b. Avro JSON wire codec

Port `avro::jsonEncoder` / `jsonDecoder` semantics into the Rust crate, driven by
`JsonTests.cc`. Union encoding, `bytes` and `fixed` escaping, and logical-type
rendering all have to match. Spec separately in `doc/specs/AvroJsonCodec.md` before
implementing.

## Migration mechanism: the compat shim

`avrocpp_compat.h` / `.cc`: avrocpp's names and signatures, throwing
`avro::Exception`, implemented over `avro_bridge`. Call sites do not change, which is
what makes this tractable in a large codebase, and it gives one place to encode every
decision recorded above.

Three modes, selectable per process and per operation class:

- `kAvrocpp` - delegate to real avrocpp. The rollback path. avrocpp stays linked for
  the whole transition.
- `kBridge` - delegate to the binding. The destination.
- `kDualRun` - run **both**, compare, log divergences with enough context to
  reproduce, and return **avrocpp's** answer. Behavior is unchanged while this mode
  is on, which is what makes it deployable to a user-facing product.

Because there is no offline corpus of real user files, `kDualRun` in production is
the primary discovery mechanism for real Hyrum's Law dependencies. It is the
instrument, not just an adapter. Requirements:

- Compare at the level the caller observes: **decoded value equality, re-encoded
  byte equality, and threw-versus-did-not**. Register row 2's order problem shows up
  only in bytes; row 1 shows up only as an error. A value-only comparison misses both.
- Log a reproducer: schema fingerprint plus the input bytes, size-capped, and handled
  under whatever rules apply to user content. A divergence report without the bytes
  that caused it is not actionable.
- Sample, rate-limit, and budget the added latency and memory. Dual-run doubles parse
  cost and holds two value trees; the divergence logger must not become the outage.
- Per-operation kill switch back to `kAvrocpp`, plus a divergence-rate metric broken
  out by divergence ID, so each one gets closed with evidence rather than argument.

Build the comparison on what already exists. `ValidateCrossRead`
(`benchmarks/avro_benchmark.cc:386`) already writes with the binding, relays through
avrocpp, reads back both whole-buffer and through 4099-byte chunks, and compares
every value. Extract it into `avro_compare.{h,cc}` and have the shim, the benchmark,
and the fuzzers all call it, rather than writing a third copy that can disagree.

## Differential fuzzing

Two targets, both calling the shared comparison component.

- **Structure-aware.** Generate a random schema, then a random datum valid for it,
  then compare the full circle: encode with each engine, decode each output with
  each engine, require agreement on value, bytes, and error-or-not. Bias the schema
  generator at the known divergences: maps with duplicate and hash-colliding keys, strings
  holding invalid UTF-8 and lone surrogates and embedded NULs, enums and unions at
  index boundaries, deep nesting, recursive named types, aliases, logical types with
  extreme precision and scale.
- **Mutational.** Byte-mutate valid encodings and container files. Require that both
  engines either both accept with equal results or both reject. Divergence in
  *which* inputs are rejected is the finding; crashes are a bonus, not the goal.
  Seed from the phase 1 corpora and from every dual-run reproducer.

On the Rust side add `cargo-fuzz` targets for what the type system does not give you
even under `#![forbid(unsafe_code)]`: panics escaping `catch_panic`, allocation
beyond `max_allocation_bytes`, and stack overflow on deep nesting. That last one
aborts and cannot be caught, so it needs a depth limit rather than a test.
`rust/tests/security_properties.rs` and `rust/tests/max_allocation.rs` are the
existing home for these properties; the fuzzers extend them rather than replace them.

`rust/Cargo.toml` has no property-testing or fuzzing dependency today, so this is new
infrastructure.

## Rollout gates

1. Ship the shim in `kAvrocpp` mode. Behavior identical; this proves the shim's own
   plumbing in isolation.
2. Enable `kDualRun` on a sampled slice and widen it. **Hold** until every divergence
   is closed: fixed, or accepted with the user-visible consequence written down.
   Duration is set by evidence, not by calendar. A rare schema shape appears when it
   appears.
3. Flip to `kBridge` per operation class, coarse to fine, **read paths before write
   paths**: a bad read is reversible, a bad write emits bytes users keep. Kill switch
   stays live.
4. Unlink avrocpp. Only now is the memory-safety goal met.
5. Optional, last: unwind the shim, migrate call sites to the native
   `absl::StatusOr` API, delete `avrocpp_compat.h`.

## Verification

- `cargo test` in `rust/` covers every ported avrocpp case, each run against all
  three decode paths.
- `cmake --build build && ctest` covers the bridge and shim API surface including the
  exception mapping and the mode switch.
- Differential fuzzers in CI over a persistent corpus; any divergence fails the
  build. New corpus entries come from dual-run findings.
- `cmake -B build -DAVRO_BUILD_BENCHMARKS=ON && ./build/avro_benchmark` shows no
  regression at the call shapes phase 0 found, and the 2a spike shows the token path
  is viable before 2a is built out.
- Production dual-run divergence rate per divergence ID, trending to zero or to an
  explicitly accepted number, is the release gate. Nothing above substitutes for it,
  because nothing above sees real user input.
