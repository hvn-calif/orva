# Spec: Non-UTF-8 Avro strings

Status: DRAFT - awaiting approval

Resolves D1 in `doc/AvrocppDivergences.md`. Read that entry first for the
citations establishing the divergence; this spec decides what to do about it and
records why the alternatives were rejected.

## Purpose & user problem

avrocpp does not validate that an Avro `string` holds UTF-8. `BinaryDecoder::decodeString`
(`impl/BinaryDecoder.cc:118`) reads a length, resizes a `std::string`, and copies raw
bytes. Because `std::string` is byte-oriented, arbitrary bytes survive decode,
round-trip, and re-encode intact, and no caller ever learns anything was wrong.

This binding rejects, on read (`rust/decode.rs:733`, upstream `src/decode.rs:217`
and `:473`) and on write (`rust/value.rs:75`).

Avro specifies `string` as UTF-8, so avrocpp is out of spec. That does not help a
product whose stored data decoded yesterday and stops decoding today. This is the
sharpest instance of the strictness asymmetry: correct new behavior turning
previously-working user data into a hard error.

The write-side rejection widens the blast radius. A caller that takes bytes from a
non-Avro source and puts them in a `string` field fails at value construction, so
this reaches code that never reads a foreign Avro file.

## The constraint that shapes every option

`apache_avro::types::Value::String(String)` holds a Rust `String`, which the type
system guarantees is UTF-8. **There is no field in the value tree to put invalid
bytes in.** Every option below is a different answer to that, and none of them is
free.

One piece of luck bounds the cost: the C++ surface is already byte-oriented.
`get_string` returns `VecU8` (`rust/value.rs:259`), surfacing as
`absl::StatusOr<std::string>`, and `std::string` holds bytes exactly as avrocpp's
did. **No C++ signature has to change.** Only the internal representation is in
question.

## Decision

A `Schema::String` position decodes **per value, by content**:

| Wire bytes | Decodes to |
|---|---|
| Valid UTF-8 | `Value::String`, exactly as before. Unchanged in behavior, allocation count, and cost |
| Not valid UTF-8 | `Value::Bytes` holding the raw bytes, with no error |

To be unambiguous, since an earlier draft of this section was not: this is **not**
"decode every string as `Bytes`". The overwhelmingly common case is untouched. Only
the bytes that would previously have failed take the new path. See "Decode every
string as Bytes" under the rejected alternatives for why the consistent-looking
option is worse.

Two properties are deliberate:

- **No configuration.** There is no policy enum, no `Strict` mode, and no error path
  for this case. A caller cannot ask for the old behavior, and nothing needs to be
  threaded through to get the new one. (This is the one thing worth taking from the
  Latin-1 proposal; see below.)
- **Representation-honest.** `Value::Bytes` is distinct from `Value::String`, so the
  map from wire bytes to value stays injective and re-encoding is unambiguous. That
  is what preserves avrocpp's byte-exact round-trip.

Accepted costs, all three of which are real:

1. **`IsString()` returns false** for such a field, and `TypeName()` says "bytes",
   even though the schema calls it a string. Documented at the accessor. The
   alternative, making predicates consult the schema instead of the value, means a
   predicate that no longer describes the value it was called on.
2. **`ToJsonString` silently changes the JSON type.** This was assumed to be a
   failure when the spec was drafted. It is not, and the truth is worse: apache-avro
   renders `Value::Bytes` as a JSON array of numbers and `Value::String` as a JSON
   string (avro-rs `src/types.rs:325-326`). So one `string` field renders as
   `"café"` in one record and `[255,254]` in the next, decided by whether that
   record's bytes happened to be valid UTF-8. A consumer parsing the output breaks on
   the bad record rather than being told anything is wrong.

   Verified by `NonUtf8StringTest.ToJsonStringSilentlyChangesTypeRatherThanFailing`
   in `avro_bridge_test.cc`, which pins both renderings.

   This is now the strongest argument for D11: implement the real Avro JSON wire
   codec (`doc/specs/AvroJsonCodec.md`) rather than leaving `ToJsonString` in any
   user-visible path. A per-record type change driven by data content is not a
   difference a downstream JSON consumer can absorb.
3. **Dual-run comparison must normalize before comparing.**
   `Value::String("a")` and `Value::Bytes(b"a")` are unequal, so with this change a
   correct decode reads as a divergence unless the comparison component knows. This
   is a requirement on `avro_compare`, not an optional refinement.

Totality also removes the ability to *reject* non-UTF-8, since with the validate arm
below a `Value::Bytes` will satisfy `Schema::String`. A future opt-in
`RejectInvalidUtf8` for new call sites with no legacy data is a reasonable follow-up
and is deliberately not built now.

## Rejected alternatives

Kept in full because each of these will be proposed again.

### Decode every string as Bytes

Never produce `Value::String` at a `Schema::String` position: always `Value::Bytes`,
whether or not the bytes are valid UTF-8.

This has a real advantage over the chosen option, and it is worth stating plainly
because the chosen option's weakness is exactly its mirror image: **behavior would not
depend on data content.** Under the conditional rule a field arrives as `String` in
999 records and `Bytes` in the thousandth, which passes every test anyone would think
to write and then breaks on one record in production. Always-`Bytes` is at least
uniform, and it would remove cost 2 above entirely, since `ToJsonString` could no
longer change JSON type per record.

Rejected on three counts:

1. **It changes 100% of string data to make 0.01% of it consistent.** This spec exists
   to keep avrocpp's observable behavior for data that already worked, and avrocpp
   returned a string for valid UTF-8. Always-`Bytes` maximizes the divergence it is
   supposed to minimize.
2. **`ToJsonString` gets worse, not better.** The conditional rule breaks JSON output
   on bad records; this one renders *every* string as an array of numbers, in all
   output, always. Trading an intermittent break for a total one is not a fix.
3. **Read and write stop agreeing.** `CreateString` produces `Value::String`
   (`rust/value.rs:75`). If decode never did, every round-trip comparison would
   mismatch: `assert_matches_upstream` in `rust/decode.rs`, the cross-read circle in
   `avro_compare`, and any caller comparing what it wrote against what it read. The
   String/Bytes normalization that is currently a narrow allowance would become
   mandatory on every comparison, or `Value::String` would have to leave the binding's
   vocabulary altogether.

There is also a practical objection. The chosen option is additive upstream: one
tolerated decode path plus three accepting arms, no signature changes, no new error
variants, plausibly upstreamable. Making `decode` never return `Value::String` for
`Schema::String` is a change no other avro-rs consumer would accept, so it would be a
patch this repo carries forever.

### Lossy substitution (U+FFFD)

Replace invalid sequences with the replacement character.

Rejected: corrupts data and breaks round-trip silently. Re-encoding produces
different bytes than were read, with nothing reported. An error is strictly better
than a silent rewrite, so this is worse than the status quo, not a mitigation of it.

### Surrogate escape (PEP 383 style)

Map invalid byte `0xNN` to `U+DCNN`, as Python's `surrogateescape` does.

Rejected: not expressible. Rust `String` cannot hold lone surrogates, since they are
not Unicode scalar values and not valid UTF-8.

### Side-channel table

Keep `Value::String` valid and carry the raw bytes in a table keyed by path.

Rejected: paths shift under any mutation, every accessor has to consult the table,
and the table has to survive clone, move, and subtree extraction. Fragile in
proportion to how much of the API it touches, which is all of it.

### Latin-1 smuggling, applied only to invalid strings

Store the raw bytes as Latin-1 in the `String`: byte `0xNN` becomes code point
`U+00NN`. Latin-1 is a total bijection between bytes and U+0000..U+00FF, so the
mechanism itself is lossless.

Rejected: **not injective once mixed with strict decoding.** Two different wire
inputs collapse to the same value:

    wire ff      invalid -> latin-1 smuggle -> code points [255]
    wire c3bf    valid   -> strict utf-8    -> code points [255]
    same Value::String? yes

Byte `0xFF` smuggled, and the valid UTF-8 encoding of U+00FF, both yield
`Value::String("\u{ff}")`. On re-encode there is no way to know whether to write
`0xFF` or `0xC3 0xBF`, so one of the two inputs is silently rewritten.

That destroys byte-exact round-trip, which is the property this whole spec exists to
preserve. Strictly worse than erroring, for the same reason lossy substitution is.

### Latin-1 smuggling, applied to every string

Smuggle unconditionally on the way in, reverse it at the C++ boundary. This version
*is* correct: unconditional Latin-1 is a bijection, so it is lossless and
unambiguous, and the C++ caller gets the original wire bytes.

It also has two advantages this spec's choice does not: it is total, and type
predicates stay honest, with `IsString()` true and the value validating against
`Schema::String` with no upstream validate patch.

Rejected on cost, all of it paid on 100% of string data to accommodate the rare
broken field:

1. **Upstream's encoder writes the wrong bytes for every string.** `encode_bytes`
   takes `s.as_ref()` (`src/encode.rs:46-55`), which for a `String` is its UTF-8
   bytes, so a smuggled `0xFF` is written back as `0xC3 0xBF`. The bridge could never
   hand a value to `to_avro_datum` or the container writer again. Compare the chosen
   option, where upstream's encode needs one added match arm and is otherwise right.
2. **Storage grows on all string data.** Every byte at or above 0x80 becomes two
   bytes in the `String`: `café` is 5 bytes on the wire and 7 bytes held, and the
   worst case is 2x. This repo has measured allocation work behind it
   (`patches/apache-avro-0.22-read-into.patch`, `rust/tests/manifest_alloc.rs`, the
   `strings` benchmark dataset); doubling string storage regresses against it.
3. **`max_allocation_bytes` accounting drifts** from real storage by up to 2x.
4. **`Value::String` stops meaning what every other consumer thinks it means.** 56
   non-test sites across 19 files in avro-rs touch `Value::String`, including serde,
   schema compatibility, and the JSON path. None of them get the memo.

The underlying choice is where to put the lie. `Value::Bytes` lies in the
**predicate**, discoverable at the one place you check it. Latin-1 lies in the
**representation**, invisible to every consumer that did not get the memo, including
upstream's own encoder. A wrong answer from `IsString()` is a bug you find; wrong
bytes out of `to_avro_datum` is a bug you ship.

Note the contrast with this repo's legitimate Latin-1 use.
`doc/specs/PngTextMetadata.md:38` transcodes PNG `tEXt`/`zTXt` to UTF-8 because those
chunks *are* Latin-1 by spec: the charset is a fact about the source, the conversion
is semantically correct, and nothing downstream reverses it. Using Latin-1 here would
be the same mechanism with none of that justification.

### Change `Value::String`'s inner type upstream

Patch `Value::String(String)` to hold a byte-oriented type.

Rejected on maintenance: 56 non-test sites across 19 files, including serde, schema
parsing, and compatibility. It changes public API and would conflict on every rebase.
The chosen option needs four small arms instead.

## Implementation

Staged. Each stage is independently shippable, and stage 1 is what tells you whether
2 and 3 are needed at all.

### Stage 1: make the failure say where

Unconditional, do first. Today the error is `"Avro string is not valid UTF-8"`
(`rust/decode.rs:847`) with no field path and no byte offset. Nobody can act on that
in a dual-run divergence report.

Add the record-field path and the offset within the datum. Useful whichever stage
this stops at, and it is the instrument that measures whether non-UTF-8 input occurs
at all.

### Stage 2: read path

| Site | Change |
|---|---|
| `rust/decode.rs:733` | Emit `Value::Bytes` rather than erroring |
| `src/decode.rs:217`, `:473` | Same, as a `patches/` entry. Both apache-avro paths share these, since `read_into` delegates strings there |
| `rust/value.rs:259`, `:928` | `get_string` and `get_string_at` accept `Value::Bytes`. **No C++ API change** |
| `avro_compare` | Normalize `String` against `Bytes` before comparing (see cost 3). Done, in `ValuesEqual`, unconditionally: no policy flag, since nothing wants the stricter comparison |

After stage 2 such a value can be read but not re-encoded or validated. Document
that; do not leave it to be discovered.

### Stage 3: write and round-trip

Only if dual-run shows read-modify-write call sites, or the product writes non-UTF-8
itself.

| Site | Change |
|---|---|
| `src/encode.rs:188` | Add `Schema::String` to the `Value::Bytes` arm. The wire encoding is already identical: `Value::String` also goes through `encode_bytes` (`:199`) |
| `types.rs:494` | Add a `(Value::Bytes, Schema::String)` validate arm |
| `types.rs:1097` | `resolve_string` accepts `Bytes` but calls `from_utf8`; needs a raw mode |
| `rust/value.rs:75` | `CreateString` accepts non-UTF-8, producing `Value::Bytes` |

## Success criteria

1. A container file holding a non-UTF-8 `string` field reads through every path
   (`DecodeDatum`, `AvroDatumReader::Decode` and `DecodeInto`, `AvroProjection`,
   `DataFileReader`) and `GetString` returns the original bytes.
2. After stage 3, that file round-trips byte-for-byte: decode then re-encode yields
   input bytes exactly, including the invalid field.
3. Distinct wire inputs never collapse to the same value. The Latin-1 collision above
   is a test case, not just a note.
4. Valid UTF-8 strings are unaffected in behavior, in allocation count, and in
   throughput. The `strings` benchmark dataset shows no regression.
5. Ported avrocpp cases involving non-UTF-8 agree with avrocpp's observed behavior,
   established by running its test binaries rather than by reading them.

## Out of scope

- `Lossy` / U+FFFD substitution, in any form.
- An opt-in `RejectInvalidUtf8` strict mode. Reasonable later, not built now.
- Non-UTF-8 in *schema* JSON, which is a separate surface with its own rejection at
  `rust/schema.rs:99` and is not covered by D1.
- Non-UTF-8 in map keys, record field names, and enum symbols. Same underlying
  constraint, different representation problem, and not yet investigated. Deliberately
  not decided here.
- Avro JSON wire-format rendering of such values, which belongs with D11 in
  `doc/specs/AvroJsonCodec.md`.

## Test & verification plan

1. Rust unit tests per decode path, using hand-assembled bytes (a valid encoder
   cannot produce an invalid `Value::String`, so the fixtures must be built by hand,
   as `map_bytes` does in `rust/decode.rs` for D2).
2. A regression test for the Latin-1 collision: byte `0xFF` and the UTF-8 encoding of
   U+00FF must decode to values that are not equal, and each must re-encode to its own
   input bytes.
3. Golden round-trip: a container file with an invalid `string` field, decoded and
   re-encoded, compared byte-for-byte.
4. C++ GoogleTest cases in `avro_bridge_test.cc` covering `GetString` on such a
   field, the `IsString()` wart, and the `ToJsonString` failure, so the documented
   warts are pinned rather than merely described.
5. Differential fuzzing with the schema generator biased toward invalid UTF-8, lone
   surrogates, embedded NULs, and overlong encodings, comparing against avrocpp.
6. The `strings` benchmark row, to confirm criterion 4.
