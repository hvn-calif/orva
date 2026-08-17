# Findings from the first differential fuzzing run

## Glossary

Every short form used here, other than ones assumed universally known (JSON,
UTF-8, ID).

| Term | Expansion | What it is here |
| --- | --- | --- |
| avro-cpp | Apache Avro C++ | the incumbent implementation, the thing we compare against. |
| `avro_bridge` | (a name) | the replacement: a Crubit/corrosion binding over the Rust `apache-avro` crate. Called "the bridge" below. |
| D1 | (a name) | divergence register entry 1: a `string` holding non-UTF-8 bytes. Also a suppression ID (`suppress.h`). |
| D2 | (a name) | divergence register entry 2: duplicate keys in a map. Also a suppression ID. |
| D3 | (a name) | divergence register entry 3: encoded map byte order is unstable, because Rust `HashMap` iteration order varies per process. Not a suppression ID; it is why only decoded values are compared. |
| divergence register | (a name) | `doc/AvrocppDivergences.md` on `main`: the hand-maintained list of known, accepted differences between the two implementations. |
| oracle | (a name) | the comparison step deciding whether the two implementations agreed on one generated input. |
| suppression ID | (a name) | a name that mutes one known divergence for a run, via `suppressions.txt` or `AVRO_FUZZ_SUPPRESS`. |
| UUID | Universally Unique Identifier | the Avro logical type `{"type":"string","logicalType":"uuid"}`. |

## Baseline

Baseline: worktree at `84ce4af`, avro-cpp `release-1.11.4`, empty suppression
file. Every finding except 4 came from unit-test mode, about one second per
property on a single core; finding 4 came from a coverage-guided run at ~9,000
executions per second.

Each finding below was reached **starting from an empty corpus**, with no seed
directing the fuzzer toward it.

Findings 1 and 2 came from the byte-oriented properties, which were added after
the rest. They are the two the tree-based properties could not have found: one
needs a truncated payload, which a lowered value never produces, and the other
needs a schema shape `Normalize` legalises away. Both surfaced on the first
input of their property.

## Acceptance test: the two known bugs were rediscovered

Both are entries in the divergence register, written down before this harness
existed, and both are open at `84ce4af` in both directions. Rediscovering them
without being told to look is what makes the harness's other results worth
anything.

`efb7162`, twelve commits later, is often read as having closed them. It did
not: it closed D1's read side, and replaced D2's silent collapse with a
rejection in the projected decoder only -- avro-cpp keeps both entries, so the
two still disagree. Neither write path was touched, and the write path is what
both pinning tests below exercise. `fuzz/README.md` has the full breakdown.

### D1 - non-UTF-8 bytes in a string

```
schema: "string"
value:  \xc2                       (a truncated UTF-8 lead byte)
```

avro-cpp stores the bytes verbatim in a `std::string`. The bridge rejects them:
`create_string` calls `utf8(v)?`. Found in roughly one second.

The read side is the same divergence from the other direction: avro-cpp encodes
such a string happily, and `DecodeDatum` then refuses the bytes.

### D2 - duplicate keys in a map

```
schema: {"type":"map","values":{"type":"record","name":"x0", ...}}
keys:   "aa", "aa"
```

avro-cpp's `GenericMap::Value` is a `std::vector<std::pair<...>>` and keeps both
entries. The bridge's `MapPut` is `entries.insert` into a `HashMap`, so the
second write silently replaces the first and one entry is lost.

Reached after suppressing D1, via `AVRO_FUZZ_SUPPRESS=D1,UUID_INVALID_REJECTED`
-- the documented triage loop.

## New findings

These are on surfaces `doc/AvrocppDivergences.md` lists as "not yet
investigated", so they are new information rather than rediscovery.

Every one of them is a difference between the two engines. A bug found on
bridge API that avro-cpp has no counterpart to is not a divergence and is not
listed here; the one such bug this work turned up is written up separately in
`doc/CanonicalFormBug.md`.

### 1. The bridge decodes an empty buffer into fabricated nulls

**The most serious finding here.** Found by `DecodersAgreeOnArbitraryBytes` on
its first input.

```
schema: {"type":"record","name":"R","fields":[
           {"name":"a","type":"boolean"},{"name":"b","type":"boolean"}]}
input:  ""                     (zero bytes)

bridge:  ok, {"a":null,"b":null}
avrocpp: avro::decode: EOF reached
```

Two things are wrong. Decoding zero bytes should fail, and the value returned
does not inhabit its own schema: `null` is not a legal value of `boolean`. A
caller handed a truncated message gets a success status and a record of nulls
instead of an error.

It is type-dependent rather than uniform, which is what makes it easy to miss:

| schema | empty input, bridge | avro-cpp |
| --- | --- | --- |
| `"null"` | accepts, `null` | accepts (correct -- `null` really is zero bytes) |
| `"int"`, `"long"`, `"string"` | rejects | rejects |
| `"boolean"` | **accepts, yields Null** | rejects |
| `["int"]` | **accepts, union branch is Null** | rejects |
| record of booleans | **accepts, every field Null** | rejects |

The register's worst class is silent data loss; this is its mirror image,
manufacturing data that was never on the wire.

Pinned by `Differential.EmptyInputDecodesToFabricatedNulls` and
`Differential.EmptyInputFabricatesNullForBooleanAndUnion`.

### 2. Empty union `[]` and empty enum: bridge accepts, avro-cpp rejects

Found by `SchemaTextVerdictsAgree` on its first input.

```
schema text: []
bridge:  accepts, and re-renders it as []
avrocpp: Schema is invalid, due to bad node of type union
```

Same for `{"type":"enum","name":"E","symbols":[]}` ("bad node of type enum")
and for `[]` nested as a record field type. So the bridge round-trips a schema
avro-cpp cannot read, which is the same interop break as finding 3 below on a
different construct.

Worth recording how this was missed for as long as it was: the tree-based
generator **cannot** produce it. `NormalizeChildren` tops an empty union up to
one branch (`ir.cc:257-265`), so no amount of running `SchemaVerdictsAgree`
would have reached it. Two bytes of schema text found it immediately.

Pinned by `Differential.EmptyUnionAndEnumAcceptedOnlyByTheBridge`.

### 3. The bridge re-renders a `duration` fixed in a shape avro-cpp cannot read

**The most consequential for the migration itself.**

```
in:   {"type":"fixed","name":"B","namespace":"ns","size":12,"logicalType":"duration"}
out:  {"type":{"type":"fixed","name":"duration","size":12},"logicalType":"duration"}

avro-cpp on the output: Json field "type" is not a string
```

Both engines parse the input schema; `SchemasCrossParse` only reaches the
rendering comparison once they have. Two distinct defects in the output:

- **The fixed is nested inside `"type"` as an object.** avro-cpp rejects that
  outright, so a schema that has passed through the bridge can no longer be
  read by avro-cpp. That is the direction that breaks a partly-migrated
  deployment: a bridge-side writer publishes a schema an avro-cpp-side reader
  must consume.
- **Name and namespace are dropped.** `ns.B` comes back as `duration`, so
  schema identity does not survive the round trip even for a reader that can
  parse the output.

Pinned by `Differential.DurationFixedRendersUnparseableByAvrocpp`, which
reproduces from the hand-written schema above rather than from a generated
tree.

### 4. apache-avro panics on a schema that defines a name twice

Found by `DatumCircleAgrees` under coverage-guided fuzzing, within a second.

```
{"type":"record","name":"foo","namespace":"ns","fields":[
  {"name":"a","type":{"type":"record","name":"foo","namespace":"ns","fields":[]}}]}
```

Defining `ns.foo` twice is illegal Avro -- a name may be defined once. **Both**
engines nevertheless accept it at parse time; that was worth checking rather
than assuming, and avro-cpp accepts it too. The divergence is what happens
next. avro-cpp carries on; apache-avro 0.21 panics at *encode* time:

```
apache-avro-0.21.0/src/types.rs:369
Schemata didn't successfully resolve: Two named schema defined for same fullname: ns.foo
```

Two problems on the bridge side. The schema should have been rejected at parse
time rather than accepted and blown up later, and a malformed schema should
produce an error rather than a panic.

`catch_panic` contains it, so the process survives and the caller gets an
`absl::Status`. But that guard is the only thing standing between an untrusted
schema and a process abort, and `rust/vec_u8.rs` already warns that apache-avro
panics on some malformed input. Any entry point missing the guard is a denial
of service on attacker-supplied schemas.

Pinned by `Differential.DuplicateFullNameParsesThenPanicsOnEncode`.

### 5. avro-cpp accepts a malformed namespace

```
schema: {"type":"fixed","name":"B","namespace":"ns..bad","size":16}
```

`ns..bad` has an empty middle component, which the Avro spec does not allow: a
namespace is a dot-separated sequence of names, and a name cannot be empty.
avro-cpp accepts it; the bridge rejects it.

Direction is bridge-stricter, so nothing is silently mis-decoded, but it means
avro-cpp will ingest schemas that other Avro implementations reject -- data
written under such a schema may not be readable elsewhere. Arguably an avro-cpp
bug to report upstream rather than a bridge one.

### 6. The bridge rejects uuid text avro-cpp keeps verbatim

```
schema: {"type":"string","logicalType":"uuid"}
value:  \xbd
```

avro-cpp treats a `uuid` as an ordinary string and stores whatever it is given.
The bridge runs `uuid::Uuid::parse_str` and refuses anything that is not a
well-formed UUID.

Same shape as D1 -- the bridge validates where avro-cpp does not -- but a
distinct site, and not covered by D1's fix. Tracked as
`UUID_INVALID_REJECTED`.

There is a second, quieter half to this one that the oracle also checks: when
the bridge *does* accept the text, it re-emits the lowercase canonical form, so
`0F9A...` in becomes `0f9a...` out and `urn:uuid:` prefixes are dropped. That
is `UUID_TEXT_NOT_PRESERVED`.

### 7. Trailing bytes after a datum: avro-cpp ignores, the bridge rejects

```
schema: "int"
input:  02 ff        (one int, then a stray byte)

avrocpp: ok, 1 -- stops at the end of the first datum
bridge:  trailing bytes after single Avro datum
```

Direction is bridge-stricter, so nothing is mis-decoded, and the bridge's
behaviour is the more defensible of the two: trailing bytes after a single
datum usually mean framing has gone wrong. Recorded because callers moving off
avro-cpp will hit it wherever they relied on a padded or over-allocated buffer
being tolerated.

This is `TRAILING_BYTES`, one of the divergence IDs `suppress.h` declared with
no code able to report it until `DecodersAgreeOnArbitraryBytes` existed.

Pinned by `Differential.TrailingBytesAcceptedOnlyByAvrocpp`.

## Not yet covered

The harness currently exercises single-datum encode and decode, decode of
arbitrary bytes under a generated schema, and schema parsing and rendering from
both trees and raw text. Still to build: object-container round-trips across
the null, deflate and snappy codecs; reader-versus-writer schema resolution,
which the register also lists as entirely uninvestigated and which the plan
sketches as an `EvolvePlan` edit applied to the writer tree; and the
deep-nesting termination property.

Byte-level comparison is deliberately out of scope -- random container sync
markers and Rust `HashMap` iteration order (D3) make encoded bytes vary between
runs, so only decoded values are compared.
