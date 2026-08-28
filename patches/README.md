# Apache Avro patches

Patches this repo maintains against `apache-avro` (avro-rs), each a normal
`git format-patch` artifact applicable with `git am` or `git apply`.

| Patch | Base | Purpose |
|---|---|---|
| `apache-avro-0.21-non-utf8-string.patch` | `rel/release-0.21.0` | Adds `util::set_non_utf8_string_as_bytes`, off by default. On, a `string` whose wire bytes are not valid UTF-8 decodes as `Value::Bytes` instead of failing. Closes D1; see `doc/specs/AvroStringPolicy.md` |
| `apache-avro-0.21-uuid-as-string.patch` | the non-UTF-8 patch above | Adds `util::set_uuid_as_string`, off by default. On, a `uuid` decodes as an ordinary string, so the bytes as written survive: no canonical rewriting, no reinterpretation of a 16-byte string, no rejection of text that is not a uuid |
| `apache-avro-0.21-strict-eof.patch` | the uuid patch above | Makes a truncated buffer an error. Three arms of the decoder returned a value on end of input instead of failing, so a truncated datum decoded into data that was never on the wire. **Not** off by default and **not** additive: it reverses upstream's AVRO-3240 behaviour. Closes the `bridge-lenient` / "EOF reached" divergence; see `doc/specs/DivergenceClosure.md` |
| `apache-avro-0.21-empty-union.patch` | the strict-eof patch above | Rejects a union with no members. `[]` used to parse and re-render as `[]`, though no branch index is in range for it, so it has no encodable value. **Not** additive: two upstream tests asserted the old behaviour. Closes the `schema-acceptance` / "bad node of type union" divergence |
| `apache-avro-0.21-empty-enum.patch` | the empty-union patch above | Rejects an enum with no symbols, the same defect on a different construct. Guards the parse path only: `EnumSchema` has public fields and a builder, unlike `UnionSchema`. Adds `Details::EnumEmptySymbols`. Closes the `schema-acceptance` / "bad node of type enum" divergence |
| `apache-avro-0.21-empty-decimal.patch` | the empty-enum patch above | Lets a zero-length unscaled decimal value round-trip. It used to decode into a `Decimal` that could be neither read nor re-encoded, both failing with a sign-extension error. Closes the `reencode-failed` / "decimal sign extension 0" divergence |
| `apache-avro-0.21-json-depth.patch` | the empty-decimal patch above | Adds `util::set_max_json_depth`, **unset by default**. Set, it bounds how deep a schema JSON document may nest, checked by a linear scan over the bytes *before* parsing, and switches serde_json's own 128-container limit off so exactly one limit applies. Clamped to `MAX_JSON_DEPTH_CEILING`, 600, which is avro-cpp's `DepthTracker::kMaxDepth` in the same unit. Also adds `util::json_document_end`. Closes the 128-against-600 acceptance divergence; see `doc/specs/JsonDepthLimit.md` in orva-difffuzz |
| `apache-avro-0.22-read-into.patch` | avro-rs `8000091350d32f4ed4d94166dcb7695a4a25e409` | Allocation-reusing value decoding: `read_into`, `read_value_into`, `OwnedGenericDatumReader`. Superseded by the 0.23 rewrite below; kept because docs and recorded benchmark results reference it |
| `apache-avro-0.23-read-into.patch` | avro-rs `4617efecb7159e56b122282c950ed32e04d36859` | Allocation-reusing value decoding, rewritten for the 0.23 main line: same API as the 0.22 patch plus integration with the per-datum allocation budget and recursion depth limit that landed upstream after 0.22, and a criterion bench. This is the version being submitted upstream |

## apache-avro-0.21-non-utf8-string.patch

Base: the `rel/release-0.21.0` tag, which matches the crates.io `apache-avro
0.21.0` source byte for byte (verified against
`~/.cargo/registry/src/*/apache-avro-0.21.0/`). It applies to a clean checkout
of that tag with no other patch present, and does **not** depend on the
read-into patch below.

```sh
git clone https://github.com/apache/avro-rs && cd avro-rs
git checkout rel/release-0.21.0
git am /path/to/apache-avro-0.21-non-utf8-string.patch
cargo test -p apache-avro --features derive
```

Verified on 2026-08-13: applies clean to a detached worktree at `0470799`
(`git apply --check`), `cargo fmt --check` clean, no new clippy findings in the
changed files, and the suite goes from 541 passing on the unpatched tag to 551
with the patch, the 10 tests it adds. Two pre-existing failures in that checkout
are unrelated and reproduce without the patch: the `specific_single_object`
example and the `avro-rs-226` integration test both need the `derive` feature,
so build with `--features derive`.

**The behaviour is off by default.** Nothing changes for an existing consumer of
the crate until a caller opts in, once per process and before any decoding:

```rust
// The return value is the setting actually in effect, which differs from the
// argument if something already set it. Check it rather than assume.
assert!(apache_avro::util::set_non_utf8_string_as_bytes(true));
```

What it changes, all in `avro/src/`:

- `util.rs`: the `NON_UTF8_STRING_AS_BYTES` `OnceLock`, its
  `set_non_utf8_string_as_bytes` setter and crate-internal reader. Shaped after
  the existing `set_serde_human_readable`.
- `decode.rs`, the `Schema::String` arm: with the setting on, invalid UTF-8
  yields `Value::Bytes`; off, it fails as before.
- `decode.rs`, the map-key site: still rejects either way, but reports UTF-8
  rather than reporting the key as the wrong type. Map keys cannot hold raw
  bytes because `Value::Map` is keyed by `String`.
- `encode.rs`, the `Value::Bytes` arm: with the setting on, `Schema::String` is
  accepted, so such a value round-trips byte for byte.
- `types.rs`, `validate`: with the setting on, `Value::Bytes` satisfies
  `Schema::String`.
- `types.rs`, `resolve_string`: with the setting on, invalid bytes stay
  `Value::Bytes`. Valid bytes still become a `String` either way.

Tests are split by the value of the setting, because a `OnceLock` cannot be
reset within a process: the rejecting cases are unit tests in `decode.rs` and
`types.rs` (default), and the accepting cases are their own test binary,
`avro/tests/non_utf8_string_as_bytes.rs`, mirroring how upstream tests
`serde_human_readable` with one binary per value.

No public signature changes and no new error variants, and the default preserves
every existing behaviour, so it is additive for any other consumer of the crate.

## apache-avro-0.21-uuid-as-string.patch

Base: the non-UTF-8 patch above, not the release tag. Both touch `decode.rs`,
`encode.rs`, `types.rs` and `util.rs`, so stacking them avoids a conflict; apply
them in order.

```sh
git clone https://github.com/apache/avro-rs && cd avro-rs
git checkout rel/release-0.21.0
git am /path/to/apache-avro-0.21-non-utf8-string.patch
git am /path/to/apache-avro-0.21-uuid-as-string.patch
cargo test -p apache-avro --features derive
```

Verified on 2026-08-17: `cargo fmt` clean, and the suite goes from 551 with the
non-UTF-8 patch alone to 558, the 7 tests this one adds. The same two
pre-existing failures noted above still need `--features derive`.

**The behaviour is off by default**, same shape as the patch above:

```rust
assert!(apache_avro::util::set_uuid_as_string(true));
```

Why it exists. Avro defines `uuid` as an annotation on `string`, and a reader
may leave the annotation uninterpreted; avro-cpp does, and never validates one.
Parsing it into a `Uuid` has three effects that reading it as a string does not,
each measured against avro-cpp with the differential fuzzer:

- **The bytes change.** `Uuid` re-encodes in canonical hyphenated lowercase, so
  `urn:uuid:` prefixes, braces, upper-case hex and unhyphenated 32-hex forms are
  all rewritten. A value read and written back is not the value that arrived.
- **A 16-byte string is reinterpreted.** Any `uuid` field of exactly 16 bytes is
  taken as a raw `Uuid` and comes back as 36 characters of hex. Length decides
  how the field is read, which also means the UTF-8 check is skipped at exactly
  that length.
- **Malformed text is rejected**, making data other implementations wrote
  unreadable.

What it changes, all in `avro/src/`:

- `util.rs`: the `UUID_AS_STRING` `OnceLock`, its `set_uuid_as_string` setter and
  crate-internal reader, shaped after the setting above.
- `decode.rs`, a guarded `Schema::Uuid` arm placed before the existing one: with
  the setting on it delegates to the `Schema::String` path.
- `types.rs`, `resolve_uuid`: with the setting on a `String` stays a `String`,
  which is what stops resolution reintroducing the canonicalisation.
- `types.rs`, `validate`, and `encode.rs`, the `Value::Bytes` arm: accept
  `Schema::Uuid`, for the non-UTF-8 case.

It composes with `set_non_utf8_string_as_bytes` rather than duplicating it. A
`uuid` whose text is not valid UTF-8 yields `Value::Bytes` when that setting is
also on, and still fails when it is not; `non_utf8_still_fails_without_the_other_setting`
pins that the two stay independent. `Value::String` was already accepted at a
`Schema::Uuid` position by `validate` and the encoder, so nothing there needed
widening.

Tests follow the same split: the parsing behaviour stays covered by the existing
unit tests at the default, and the accepting cases live in
`avro/tests/uuid_as_string.rs`, because a `OnceLock` cannot be reset in-process.

No public signature changes and no new error variants.

## apache-avro-0.21-strict-eof.patch

Base: the uuid patch above, so the stack order is non-UTF-8, uuid, then this.
It touches `decode.rs` and `reader.rs`; the two patches below it also touch
`decode.rs`, so apply them in order.

```sh
git clone https://github.com/apache/avro-rs && cd avro-rs
git checkout rel/release-0.21.0
git am /path/to/apache-avro-0.21-non-utf8-string.patch
git am /path/to/apache-avro-0.21-uuid-as-string.patch
git am /path/to/apache-avro-0.21-strict-eof.patch
cargo test -p apache-avro --features derive
```

Verified on 2026-08-18: `cargo fmt --check` clean, `cargo clippy --all-targets
--features derive` reports nothing in either changed file, and the suite goes
from 558 with the two patches above to 562, the four tests this one adds.

**This patch is on unconditionally and it is not additive.** Every other patch
here is either a setting that defaults to off or an addition to the API. This one
changes what an existing consumer of the crate gets back, deliberately, because
what it got back before was a value that does not inhabit its own schema.

Three arms of `decode_internal` treated `ErrorKind::UnexpectedEof` as a value:

| arm | what it returned when the input ran out |
| --- | --- |
| `Schema::Boolean` | `Ok(Value::Null)` |
| `Schema::String` | `Ok(Value::Null)` |
| `Schema::Union` | `Ok(Value::Union(0, Box::new(Value::Null)))` |

`null` is not a value of `boolean` or of `string`, and branch 0 of a union is
only `null` when the union declares it first, so `["int"]` produced a union whose
payload had the wrong type. A record of two booleans decoded from **zero bytes**
into a record of two nulls with an `Ok` status. avro-cpp reports "EOF reached"
for all of them.

`Schema::Null` still decodes from an empty buffer. That is correct and is why the
fix could not be "reject an empty buffer": a `null` datum occupies no bytes.

The `Schema::String` arm was the one no existing test or fuzz property had
reached. An empty buffer under `"string"` already failed, because the length
prefix is missing, so nothing exercised a length prefix with too few bytes behind
it.

One upstream test changes with the behaviour.
`reader::tests::test_from_avro_datum_with_union_to_struct` (AVRO-3240) encoded
two of its record's five fields and let the three trailing unions decode from
nothing, which the test described as simulating missing keys. Truncation is not a
writer that lacked those fields -- that is what schema resolution is for -- so
the test now asserts the error, and gained a payload with all five fields
encoded. It still covers what it was written for, a key absent from the target
struct, through the field the struct omits.

## apache-avro-0.21-empty-union.patch

Base: the strict-eof patch above, so the full stack is non-UTF-8, uuid,
strict-eof, then this.

```sh
git am /path/to/apache-avro-0.21-non-utf8-string.patch
git am /path/to/apache-avro-0.21-uuid-as-string.patch
git am /path/to/apache-avro-0.21-strict-eof.patch
git am /path/to/apache-avro-0.21-empty-union.patch
cargo test -p apache-avro --features derive
```

Verified on 2026-08-18: `cargo fmt --check` clean, clippy reports nothing new in
the changed files (the four `result_large_err` and `useless_vec` warnings there
are present without the patch), and the suite goes from 562 to 564.

`[]` parsed and re-rendered as `[]`. No branch index is in range for an empty
union, so nothing can be encoded into one and no bytes decode under one: a schema
with no inhabitants that the crate accepted and round-tripped. avro-cpp rejects
it as a "bad node of type union", so a schema this crate produced was unreadable
there.

`UnionSchema::new` now returns the **existing** `Details::EmptyUnion`, so there is
no new error variant and no signature change. `UnionSchema`'s fields are
crate-private, so `new` is the only way an external caller builds one, and the
single struct-literal construction inside the crate (the nullable wrapper in
`schema.rs`) always supplies two branches. The check therefore covers every union
the crate can produce.

`parse_union` used to log an error for the empty case and carry on; that line is
gone, since the condition is an error where it is detected.

**Not additive**, for the same reason as strict-eof. Two upstream tests asserted
the old behaviour:

- `schema::tests::avro_3946_union_without_any_types` parsed a record with a
  `"type": []` field and asserted only that the log line was emitted.
- `schema_compatibility::tests::test_compatible_reader_writer_pairs` carried two
  rows built from an `empty_union_schema()` helper. Both were vacuous, since no
  value inhabits an empty union, and the helper is removed with them.

A one-branch union such as `["int"]` stays legal, which is what keeps the change
from over-reaching: index 0 is in range, so it has a valid encoding.

## apache-avro-0.21-empty-enum.patch

Base: the empty-union patch above. The full stack is non-UTF-8, uuid, strict-eof,
empty-union, then this.

Verified on 2026-08-18: `cargo fmt --check` clean, no new clippy findings in the
changed files, and the suite goes from 564 to 565.

`{"type":"enum","name":"E","symbols":[]}` parsed. No symbol index is in range, so
it has no valid encoding: the same defect as the empty union, on a different
construct. avro-cpp rejects it as a "bad node of type enum".

`parse_enum` returns the error once it has collected the symbols. **This guards
the parse path, not every construction.** `UnionSchema`'s fields are
crate-private, so the previous patch's check in `new` covers everything the crate
can build; `EnumSchema` has public fields and a `bon` builder, so a Rust caller
can still hand-build one with no symbols. Untrusted input arrives by parsing,
which is what this covers.

Adds `Details::EnumEmptySymbols`. The two existing enum variants describe parse
failures (`GetEnumSymbolsField`, `GetEnumSymbols`), so reusing one would report
the wrong cause. `Details` carries no `#[non_exhaustive]`, so a new variant is
breaking for a consumer matching it exhaustively.

Two upstream tests used `"symbols": []` as a placeholder while testing something
else. `avro_custom_attributes_schema_without_attributes` and
`avro_3609_custom_attributes_schema_with_attributes` both assert only on
`schema.custom_attributes()`, so each fixture gains one symbol and what they
check is untouched.

## apache-avro-0.21-empty-decimal.patch

Base: the empty-enum patch above. The stack is non-UTF-8, uuid, strict-eof,
empty-union, empty-enum, then this.

Verified on 2026-08-18: `cargo fmt --check` clean, nothing from clippy in
`decimal.rs`, and the suite goes from 565 to 567.

A `bytes` decimal whose length prefix is 0 carries a zero-length unscaled value.
Decoding it succeeded, but the `Decimal` it produced could then be neither read
nor re-encoded: `to_vec`, `TryFrom<&Decimal> for Vec<u8>` and the encoder all
failed with the same sign-extension error, so a caller had no way to use or
forward the result. That is worse than either accepting or rejecting.

`Decimal::from(&[])` stores `len = 0` and `value = 0`, since
`BigInt::from_signed_bytes_be(&[])` is zero. In
`to_sign_extended_bytes_with_len`, `BigInt::to_signed_bytes_be()` of zero is one
byte, so `num_raw_bytes` is 1 and `len.checked_sub(1)` on `len == 0` underflows
into `Details::SignExtend { requested: 0, needed: 1 }`.

The guard returns the empty slice for a zero value at `len == 0`, which is the
byte sequence it was decoded from. It is narrow on purpose: a non-zero value asked
for zero bytes still fails, and a zero value asked for a wider field is still
sign-extended. Both are pinned.

Whether the input is well formed at all is a separate question this patch does not
take a position on. Avro describes an unscaled value as a two's-complement
big-endian integer, which needs at least one byte, so rejecting a zero-length one
would also be defensible. avro-cpp accepts it, so this follows avro-cpp.

## apache-avro-0.21-json-depth.patch

Base: the empty-decimal patch above. The full stack is non-UTF-8, uuid,
strict-eof, empty-union, empty-enum, empty-decimal, then this.

```sh
git am /path/to/apache-avro-0.21-non-utf8-string.patch
git am /path/to/apache-avro-0.21-uuid-as-string.patch
git am /path/to/apache-avro-0.21-strict-eof.patch
git am /path/to/apache-avro-0.21-empty-union.patch
git am /path/to/apache-avro-0.21-empty-enum.patch
git am /path/to/apache-avro-0.21-empty-decimal.patch
git am /path/to/apache-avro-0.21-json-depth.patch
cargo test -p apache-avro --features derive
```

Verified on 2026-08-24: `cargo fmt --check` clean, clippy reports nothing in any
file this patch touches (the `result_large_err` and `useless_vec` findings in
`schema.rs` are the pre-existing ones noted above), and the suite goes from 567
with the six patches above to 578, the eleven tests this one adds.

Why it exists. serde_json assigns `remaining_depth: 128` where the `Deserializer`
is constructed and offers no setter, so 128 nested containers is the hard limit on
schema JSON. avro-cpp bounds the same recursion at 600 nested values. A schema
between those two numbers is therefore readable by one implementation and not by
the other, and the crate had no way to move. Unbounded was not an option on the
Rust side whatever avro-cpp does, since a Rust stack overflow aborts the process
and no panic guard can contain it -- but avro-cpp is bounded too, so parity and
safety point the same way here, which is unusual in this stack.

**Unset by default, and unset is not a number.** While the setting is unset,
serde_json's own limit stays in force, no scan runs, and parsing behaves byte for
byte as it did before, so a consumer who never calls the setter sees no change and
pays nothing for the scan. Choosing a crate default of 128 to "match" serde_json
would have been wrong in a way worth recording: our counter counts values and
serde_json's counts containers, so 128 here would quietly have tightened every
consumer from 128 containers to 127, and picking 129 to compensate would have put
a compensation constant in the crate for a reason no reader of the crate can see.
The setter is what switches both halves at once.

```rust
// Clamped to MAX_JSON_DEPTH_CEILING, so check the return value rather than
// assume the argument took effect.
assert_eq!(apache_avro::util::set_max_json_depth(600), 600);
```

**The check runs before the parse, and that ordering is the design.** With
serde_json's limit disabled the parse is itself the stack risk, so measuring the
depth of the resulting `Value` would be measuring it after the danger has passed.
The scan is one iterative pass over the bytes, aware of string literals and
backslash escapes because a `{` inside a `doc` string is not nesting, so a
document nested 60,000 levels deep is refused in constant stack.

**What the limit counts.** Nested *values*: a value's depth is one plus the number
of arrays and objects enclosing it, so the document is depth 1 and the `1` in
`[[1]]` is depth 3. That is the unit avro-cpp counts in, and it is one more than a
bracket count whenever a scalar sits at the bottom -- which in a schema document it
always does, since the innermost thing in a schema is a type name. So 600 admits
599 nested containers with a scalar inside them:

| shape | JSON levels per Avro level | deepest accepted | first rejected |
| --- | --- | --- | --- |
| `{"type":"array","items":X}` chained | 1 | 599 arrays, leaf at 600 | 600 arrays, leaf at 601 |
| a one-field record chained | 3: the record object, the `fields` array, the field object | 199 records, leaf at 598 | 200 records, leaf at 601 |

**The ceiling, and why the setter clamps.** A limit above what the stack can hold
turns a clean rejection into an abort, and how deep the stack reaches depends on
the build. Bisected on aarch64, one process per case, parsing and dropping a
document at the limit:

| build | 599 arrays | 199 records |
| --- | --- | --- |
| unoptimised | 6.8 MiB of stack | 6.4 MiB |
| optimised | 410 KiB | 851 KiB |

`MAX_JSON_DEPTH_CEILING` is 600, the same number as avro-cpp's, and the setter
returns what it installed so a caller asking for more can see it was clamped.

What it changes, all in `avro/`:

- `Cargo.toml`: `unbounded_depth` joins serde_json's features, which is what makes
  `Deserializer::disable_recursion_limit` available. It changes nothing on its own
  -- the default stays 128 until something calls the setter -- so it is safe under
  Cargo's feature unification.
- `src/util.rs`: `MAX_JSON_DEPTH_CEILING`, the `MAX_JSON_DEPTH` `OnceLock` and its
  setter, the scan, and the two crate-internal helpers the parse sites use. The
  reader is `OnceLock::get` rather than `get_or_init`, because there is no default
  to install and reading must not lock one in.
- `src/util.rs`: `json_document_end` is public alongside the setter -- one past the
  end of the first complete JSON document, or `None`. It exists for a caller that
  needs to cut input at that boundary: parsing to find it is the recursion the
  limit exists to prevent, so on input that has not been depth-checked it is the
  wrong order, and the scan already tracks everything finding the boundary needs.
- `src/error.rs`: `Details::JsonDepthExceeded { depth, maximum }`, so the rejection
  says the depth was exceeded rather than something about JSON syntax. `Details`
  carries no `#[non_exhaustive]`, so a new variant is breaking for a consumer
  matching it exhaustively.
- `src/schema.rs`: the four calls to `serde_json::from_str` on schema text, in
  three functions: `Parser::parse_str`, `Schema::parse_list`, and both parses
  inside `Schema::parse_str_with_list`, which handles its `schemata` argument and
  its schema separately.
- `src/reader.rs`: the container file header's `avro.schema`, which `Reader` parses
  itself with `serde_json::from_slice`. A check in `Schema::parse_str` would have
  missed it. A depth rejection is reported as itself there while every other
  failure keeps reporting `GetAvroSchemaFromMap`, so nothing but the new rejection
  changes at that site.

Tests are split by the value of the setting, because a `OnceLock` cannot be reset
within a process. The scan's own unit tests and the unset cases are in
`src/util.rs`; `tests/max_json_depth.rs` runs at the ceiling and pins the boundary
as documents in both shapes, all five parse sites, and refusal at 6,000 and 60,000
levels; `tests/max_json_depth_lowered.rs` runs at a limit of 8, which is what
distinguishes a setting that honours its argument from one that always installs
600. The at-the-limit tests spawn a thread with an explicit 32 MiB stack, and a
comment carries the measurements above so a later reader does not shrink it.

No public signature changes, and the default preserves every existing behaviour,
so it is additive for any other consumer of the crate.

## apache-avro-0.22-read-into.patch

Targets 0.22 and adds the reusable decoder API (`Reader::read_into`,
`GenericDatumReader::read_value_into`, `OwnedGenericDatumReader`).

**This patch is not currently in use.** The 0.21 line is the target, and it is
raw 0.21 without this patch. Note what that costs, because it is not free:

`rust/datum.rs` and `rust/container.rs` call `OwnedGenericDatumReader` and
`read_value_into`, which raw 0.21 does not have. Building against raw 0.21
therefore requires reimplementing or removing:

- `AvroDatumReader` in `rust/datum.rs`, and with it `AvroDatumReader::Create`,
  `Decode`, `DecodeInto` and `WriterSchema` in `avro_bridge.h` (see
  `doc/specs/AvroDatumReader.md`).
- the allocation-reusing container read path in `rust/container.rs`
  (`reuse_decoder`), and with it `DataFileReader::NextValueInto`.

`AvroProjection` and the projected container read do not depend on this patch:
`rust/decode.rs` is a hand-written decoder that does its own byte walking.

Instructions for a receiving workspace, if this patch is ever reinstated:
check out avro-rs `8000091350d32f4ed4d94166dcb7695a4a25e409` (it also applies
to `006ac8976f52af356beb5042788370f645f6da02`, which differ only in a
dependency bump and added documentation, not in the files touched), apply the
patch, and redirect the `apache-avro` dependency to the patched crate with a
path dependency or Cargo source override. Do not commit the receiving
machine's checkout path to this repository.

## apache-avro-0.23-read-into.patch

The 0.22 read-into patch above, rewritten for the current avro-rs main line
(crate version 0.23.0) with upstream submission as the goal. Same public API:
`Reader::read_into`, `GenericDatumReader::read_value_into`,
`OwnedGenericDatumReader`. New relative to the 0.22 patch:

- Integrates with `DecodeContext`, which landed upstream after the 0.22 base:
  the per-datum allocation budget and the recursion depth limit apply to the
  reusing path exactly as to the owned path. The budget is charged by decoded
  size rather than by net-new allocation, so both paths accept and reject the
  same inputs.
- Preserves the zero-bytes-consumed block guard from upstream #643.
- Adds eight tests (allocation reuse proven by pointer stability, array
  truncation, variant replacement, shared budget, bounded recursion,
  reader-schema resolution, post-error behaviour) and a criterion bench,
  `avro/benches/read_into.rs`.

Base: avro-rs `4617efecb7159e56b122282c950ed32e04d36859` (origin/main on
2026-08-28). It applies to a clean checkout of that commit with no other patch
present:

```sh
git clone https://github.com/apache/avro-rs && cd avro-rs
git checkout 4617efecb7159e56b122282c950ed32e04d36859
git am /path/to/apache-avro-0.23-read-into.patch
cargo test -p apache-avro --all-features
```

Verified on 2026-08-28 on the commit it was exported from (`6666ea2` on the
`read-into-v2` branch of the local avro-rs checkout, which folds in the
findings of a multi-angle code review): `git apply --check` clean against a
detached worktree at `4617efe`, `cargo fmt --check` clean, zero clippy
findings with `--all-features --all-targets`, 747 tests plus 36 doctests
passing with `--all-features`, and the bench reads 10,000 nested records in
8.91 ms through the iterator against 3.91 ms through `read_into` (2.3x) on
the aarch64 build machine.
