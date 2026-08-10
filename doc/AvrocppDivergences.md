# Behavioral divergences: avrocpp vs avro_bridge

Reference document. Findings only, no migration decisions: those live in
`doc/specs/AvrocppMigration.md`, which cites the IDs below.

Last updated 2026-08-06.

## What this is for

A C++ caller swapping avrocpp for `avro_bridge` keeps the same shapes of operation
but not the same behavior. This lists every place the two disagree that has been
established so far, so that a decision about any one of them starts from a fact and
a citation rather than from a guess.

IDs (`D1`, `D2`, ...) are stable. Cite them from code comments, specs, and
divergence reports rather than restating the finding.

## Versions

| Component | Pin |
|---|---|
| avrocpp | Apache avro-cpp `release-1.11.4`, the revision `CMakeLists.txt` fetches |
| apache-avro (Rust) | avro-rs rev `006ac8976f52af356beb5042788370f645f6da02`, per `rust/Cargo.toml` |
| this repo's own decoder | `rust/decode.rs`, the projected/token-advance path |

Citations under `api/`, `impl/`, and `test/` are relative to `lang/c++/` in the
avrocpp tree. Citations to `src/` are in the avro-rs tree. Everything else is
relative to this repo.

## Method

Every finding below was established by one of:

- reading both implementations at the cited line, or
- running avrocpp's own test binaries, which build and pass in this project's build
  tree (they are registered as CTest tests 80-91 under `-DAVRO_BUILD_BENCHMARKS=ON`;
  11 of 12 build, the exception being `AvrogencppTestReservedWords`, which fails on a
  clang/`constinit` problem in avrocpp's own generated header and is unrelated), or
- the value-for-value cross-read circle at `benchmarks/avro_benchmark.cc:386`.

Nothing here is inferred from documentation or from what an implementation "should"
do. Where a claim has not been checked, it is in "Not yet investigated" rather than
stated with a hedge.

## Class A: input that changes hands differently

The three findings where avrocpp-legal input silently produces different data, or
stops working. Everything in later classes changes a message, a failure point, or an
operational property, not the data.

### D1. Non-UTF-8 bytes in an Avro `string`

| | |
|---|---|
| avrocpp | **Accepted, unvalidated.** `BinaryDecoder::decodeString` (`impl/BinaryDecoder.cc:118`) reads a length, resizes a `std::string`, and copies raw bytes into it. `std::string` is byte-oriented, so arbitrary bytes survive decode, round-trip, and re-encode intact, and no caller learns anything was wrong |
| bridge | **Rejected on read** at `rust/decode.rs:733` via the helper at `:847`, and upstream at `src/decode.rs:217` and `:473` (`Details::ConvertToUtf8`, `src/error.rs:96`). **Also rejected on write** at `rust/value.rs:75` |

Both directions matter. The write-side rejection means a caller that takes bytes from
a non-Avro source and puts them in a `string` field fails at value construction, so
this reaches code that never reads a foreign Avro file.

Avro `string` is specified as UTF-8, so avrocpp is out of spec here. That does not
change the observable fact that data which decoded under avrocpp does not decode
under the bridge.

Decided in `doc/specs/AvroStringPolicy.md`: decode to `Value::Bytes`, always. That
spec also records why Latin-1 smuggling, lossy substitution, and surrogate escaping
were rejected.

Related but **not** covered by this entry, and not yet investigated: non-UTF-8 in map
keys, record field names, and enum symbols. Same underlying constraint, different
representation problem.

### D2. Duplicate keys in a map

| | |
|---|---|
| avrocpp | **Both entries kept.** `GenericMap::Value` is `std::vector<std::pair<std::string, GenericDatum>>` (`api/GenericDatum.hh:405`) |
| bridge | **Collapse, last write wins.** `Value::Map(HashMap<String, Value>)` (`src/types.rs:92`). The map arrives one entry shorter and nothing reports it |

Decode sites: avro-rs `src/decode.rs:265-290`. The `read_into` path delegates maps to
`decode_internal` (`src/decode.rs:529-533`), so the patched and unpatched apache-avro
paths share that single site.

Status: closed in this repo's projected decoder (`insert_map_entry` in
`rust/decode.rs`, tests `duplicate_map_key_is_rejected` and
`duplicate_map_key_still_collapses_upstream`), which now rejects rather than
collapses. Still open on both apache-avro paths, so the projected path is currently
stricter than the other two.

### D3. Map iteration order, and the bytes of an encoded map

| | |
|---|---|
| avrocpp | Wire order preserved on read; encoded output deterministic |
| bridge | Order not preserved. Reads are sorted lexicographically for determinism (`rust/value.rs:447`, `:978`), but the **encoder walks `HashMap` iteration order** (`src/encode.rs:261`) |

`HashMap`'s default hasher is randomly seeded per process, so **the bytes of an
encoded map field differ from run to run for identical input.** Under avrocpp they
did not. Anything comparing, checksumming, deduplicating, or content-addressing
encoded Avro is affected, and the non-determinism means it will not reproduce
reliably.

## Class B: error surface

### D4. Error mechanism and error text

| | |
|---|---|
| avrocpp | Throws `avro::Exception`, derived from `std::runtime_error`, with Boost-formatted messages |
| bridge | Returns `absl::Status` with different codes and different text |

Every catch site changes. Any log parsing, error classification, alerting rule, or
user-facing string keyed on avrocpp's message text changes with it.

### D5. avrocpp does not throw `avro::Exception` for every malformed input

An out-of-range union branch reaches `GenericUnion::selectBranch`
(`api/GenericDatum.hh:259`), which does not bounds-check and calls
`schema()->leafAt(branch)`. That bottoms out in `attrs_.at(index)`
(`api/NodeConcepts.hh:147`), so the exception is **`std::out_of_range`**, not
`avro::Exception`.

Consequence independent of the migration: a caller whose decode is wrapped in
`catch (const avro::Exception&)` does not catch this today.

### D6. Where enum and union range checks happen

| | |
|---|---|
| avrocpp | Not in the decoder. `BinaryDecoder::decodeEnum` (`impl/BinaryDecoder.cc:157`) returns the raw value with no range check, and the container path uses that unvalidating decoder rather than `ValidatingDecoder` (`impl/DataFile.cc:266`). Rejection happens later and elsewhere: `GenericEnum::set` (`api/GenericDatum.hh:486`) throws `avro::Exception("Not as many symbols")`; unions throw as in D5 |
| bridge | Rejected inside the decoder |

Same accept/reject verdict, so no data changes hands differently. Only the message
and the failure point differ.

Note that avrocpp *does* have validating variants that check these at decode time
(`impl/parsing/ValidatingCodec.cc:276`, `:358`); the generic and container paths just
do not use them.

## Class C: accepted-input differences

### D7. Array continuation blocks using the negative-count-plus-byte-size form

| | |
|---|---|
| avrocpp | **Fails.** `arrayNext` (`impl/BinaryDecoder.cc:174`) calls `doDecodeLong` with no negative handling; the negative value casts to a huge `size_t`, `GenericReader` resizes by it, and the read throws |
| bridge | Handled correctly |

Inconsistent within avrocpp itself: array *start* (`doDecodeItemCount`,
`impl/BinaryDecoder.cc:165`) and every map block (`mapNext`, `:194`) do handle the
form. Arrays-only, and continuation-blocks-only.

Direction is bridge-more-permissive: input that failed before now succeeds.

### D8. Container codecs

| | |
|---|---|
| avrocpp | `null`, `deflate`, `snappy` only. `enum Codec` at `api/DataFile.hh:40`, with `SNAPPY_CODEC` behind `SNAPPY_CODEC_AVAILABLE`. Anything else in a file header is rejected at `impl/DataFile.cc:489` |
| bridge | Adds `zstandard` (`rust/Cargo.toml`) |

Reading is a superset, so no read regression. Writing zstd produces files that other
avrocpp-based readers reject with "Unknown codec in data file".

Note `rust/Cargo.toml`'s comment says the codec set "deliberately matches avrocpp
(null/deflate/snappy/zstd)". avrocpp 1.11.4 has no zstd, so the parenthetical
overstates the match; the surrounding point, that bzip2 and xz are deliberately
excluded, is unaffected.

### D9. Allocation ceiling

| | |
|---|---|
| avrocpp | None |
| bridge | 512 MiB default via `SetMaxAllocationBytes` (`avro_bridge.h`), a **process-global settable only once**, and only before the first decode or container read. Later calls are no-ops returning the value already in effect |

Two effects: legitimate large datums that avrocpp decoded can now fail, and a
set-once process global is awkward when more than one library in a process might want
to set it.

The projected paths are not governed by this. They are bounded structurally, with
`DataFileReader::SetMaxBlockSize` (128 MiB default) as the effective ceiling. See the
comment on `SetMaxAllocationBytes` in `avro_bridge.h`.

## Class D: representation and operational

### D10. Map order differs between avrocpp's own two APIs

| | |
|---|---|
| avrocpp, generic path | `vector<pair<string, GenericDatum>>`, wire order (`api/GenericDatum.hh:405`) |
| avrocpp, specific path | `std::map<std::string, T>`, sorted by key (documented in `api/Specific.hh`) |

Recorded because it constrains D3: "match avrocpp" is not one target. A generic call
site and a specific call site observed different orders from the same file.

### D11. Datum rendered as JSON

| | |
|---|---|
| avrocpp | `avro::jsonEncoder` writes the Avro JSON **wire format**: unions as `{"type": value}`, `\u00XX` escapes for `bytes` and `fixed`, defined logical-type renderings |
| bridge | `ToJsonString` goes through `serde_json::Value::try_from`, a different shape. `doc/specs/AvroSafeBinding.md` scopes it as a datum dump and puts wire-format parity out of scope |

Not a substitute for one another. If any output a user reads or any file a user
consumes goes through `jsonEncoder`, the shape change is visible to them.

Sharper than a fixed shape difference: the bridge's JSON type depends on the *value*,
not just the schema. `Value::Bytes` renders as a JSON array of numbers and
`Value::String` as a JSON string (`src/types.rs:325-326`), so once D1 decodes invalid
UTF-8 to `Value::Bytes`, a single `string` field renders as `"café"` in one record and
`[255,254]` in the next, decided by that record's bytes. avrocpp's `jsonEncoder` had no
such per-record variation. Pinned by
`NonUtf8StringTest.ToJsonStringSilentlyChangesTypeRatherThanFailing`.

### D12. C++ aliasing mistakes

| | |
|---|---|
| avrocpp | Undefined behavior |
| bridge | Crubit's runtime mutable-aliasing guard aborts the process (`check_no_mutable_aliasing.cc`, compiled into `avro_bridge` in `CMakeLists.txt`) |

A new operational failure shape for a long-running service, in exchange for UB.

## Feature gaps

Absence rather than divergence. Nothing to compare because the bridge has no
equivalent.

| Gap | Detail |
|---|---|
| Specific types / avrogencpp | No equivalent. Generated `codec_traits` and all of `Specific.hh` depend only on the abstract `avro::Encoder` / `avro::Decoder` token interfaces; see the generator's emission code at `impl/avrogencpp.cc:527-643`. The generated struct definitions themselves parse nothing |
| Avro JSON codec | No equivalent (D11) |
| RPC / `.avpr`, Trevni, IDL / `.avdl` | Out of scope by design in `doc/specs/AvroSafeBinding.md` |
| avrocpp internal buffer and stream classes | Out of scope. `api/buffer/`, exercised by `test/StreamTests.cc` and `test/buffertest.cc`. This repo has its own design in `doc/specs/AvroStreamingIO.md` |

## Verified equivalent

Checked, and the two agree. Listed so the divergence list above is read as bounded
rather than as a sample.

- Bool byte validation: avrocpp rejects any byte other than 0 or 1
  (`impl/BinaryDecoder.cc:69`); the bridge rejects too (`rust/decode.rs:707`).
- Avro `int` range checking on decode.
- Negative length prefixes rejected.
- Float and double byte layout on little-endian.
- Container round-trips agree value-for-value in both directions across null,
  deflate, and snappy, over 2000-value multi-block files, including reads split at
  arbitrary 4099-byte chunk boundaries that land mid-varint, mid-block-header, and
  mid-sync-marker (`benchmarks/avro_benchmark.cc:386`).
- Identity-projection decodes agree with apache-avro value-for-value and
  byte-for-byte across scalars, logical types, arrays, maps, unions, and nested
  records (`assert_matches_upstream` in `rust/decode.rs`).

## Not yet investigated

Surfaces where a divergence would be plausible and no check has been run. Absence
from the list above is not evidence of agreement here.

- **Schema parse leniency.** avrocpp's `impl/Compiler.cc` versus avro-rs schema
  parsing: unknown attributes, duplicate field names, invalid names, reserved words,
  namespace edge cases, `doc` handling. `test/SchemaTests.cc` (503 lines) and the ~30
  fixtures in `jsonschemas/` are the instrument; neither has been run against the
  bridge.
- **Schema resolution.** `impl/Resolver.cc` versus avro-rs `resolve`: type promotion,
  union-to-plain and plain-to-union, field add/remove/reorder, default substitution,
  enum default.
- **Default value handling and validation** at parse time.
- **Aliases** on records, fields, and enums.
- **Canonical form and fingerprints.** Rabin, MD5, SHA-256 against known Avro spec
  values.
- **Logical type edge cases.** Decimal precision and scale bounds, negative scale,
  `duration`, `uuid` acceptance of non-canonical forms.
- **Recursion depth.** Both recurse on nested structures. The bridge has no depth
  limit (`rust/tests/security_properties.rs::deeply_nested_values_decode_without_a_depth_limit`),
  and a Rust stack overflow aborts rather than unwinding, so the failure modes on
  deeply nested input may differ even where the verdict does not.
- **Container framing details.** Sync marker handling, `sync`/`seek` semantics,
  metadata keys beyond `avro.codec` and `avro.schema`, behavior on a truncated final
  block.
- **Snappy CRC32 handling** (`impl/DataFile.cc:395-411`) and deflate level, which
  affects output bytes even where the codec name matches.
- **Big-endian hosts.** avrocpp's `decodeFloat`/`decodeDouble` read raw bytes into a
  native float (`impl/BinaryDecoder.cc:89`, `:95`) with no byte swap; the bridge uses
  explicit little-endian conversion. These would disagree on a big-endian host.
- **Concurrency.** avrocpp object thread-safety expectations versus what the Crubit
  boundary permits (relates to D12).
