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

### 1. The bridge decoded an empty buffer into fabricated nulls (CLOSED)

**The most serious finding here.** Found by `DecodersAgreeOnArbitraryBytes` on
its first input.

```
schema: {"type":"record","name":"R","fields":[
           {"name":"a","type":"boolean"},{"name":"b","type":"boolean"}]}
input:  ""                     (zero bytes)

bridge:  ok, {"a":null,"b":null}
avrocpp: avro::decode: EOF reached
```

Two things were wrong. Decoding zero bytes should fail, and the value returned
did not inhabit its own schema: `null` is not a legal value of `boolean`. A
caller handed a truncated message got a success status and a record of nulls
instead of an error.

It was type-dependent rather than uniform, which is what made it easy to miss:

| schema | empty input, bridge | avro-cpp |
| --- | --- | --- |
| `"null"` | accepts, `null` | accepts (correct -- `null` really is zero bytes) |
| `"int"`, `"long"`, `"string"` | rejects | rejects |
| `"boolean"` | **accepts, yields Null** | rejects |
| `["int"]` | **accepts, union branch is Null** | rejects |
| record of booleans | **accepts, every field Null** | rejects |

The register's worst class is silent data loss; this was its mirror image,
manufacturing data that was never on the wire.

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

### 2. Empty union `[]` and empty enum: bridge accepted, avro-cpp rejects (CLOSED)

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

### 7. Trailing bytes after a datum: avro-cpp ignores, the bridge rejected (CLOSED)

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

**CLOSED**, and the only one in the series closed bridge-side rather than by an
apache-avro patch: `from_avro_datum` already stops at the end of the first datum
like avro-cpp, so the rejection was the bridge's own addition at
`rust/datum.rs`. It stays reachable through `SetRejectTrailingBytes`, off by
default.

It is also the only patch that **removes** a check the bridge shipped with, which
is why both halves are pinned:
`Differential.TrailingBytesAreIgnoredByBothEngines` and
`AvroBytes.TrailingBytesAreIgnoredByBothEngines` assert the engines now agree and
read the same value, and `DatumTest.TrailingBytesFollowTheSetting` in
`avro_bridge_test.cc` asserts the knob still rejects. That test needs a second
binary, because the setting is a `OnceLock` no test can reset, so CMake builds
`avro_bridge_test.cc` again as `avro_bridge_strict_test` with every setting
flipped.

Both pinned tests also assert that a **truncated** datum is still an error.
Ignoring bytes left over after a complete datum and accepting a datum with bytes
missing are different things, and conflating them would undo the strict-eof
patch.

Two things went with it, rather than being left to rot:

- The `TRAILING_BYTES` divergence ID. `suppress.h` had declared it long before
  anything could report it, and now nothing can again.
- `TrailingBytesExplainIt` in `fuzz/differential_test.cc`, which classified a
  bridge rejection as trailing bytes when avro-cpp re-encoded to fewer bytes than
  it was given. That never looked at *why* the bridge rejected. With the bridge no
  longer rejecting over leftovers, avro-cpp under-consuming while the bridge
  rejects means the datum itself failed, so the label would have named the wrong
  cause.

## Findings from the one-hour parallel run

Every property running at once for an hour, with the findings above muted so the
fuzzer reaches new ground. Setup, per-property throughput and the memory trace
are in `fuzz/run_all_parallel.sh` and its output directory.

The first three are avro-cpp bugs rather than bridge regressions: avro-cpp is the
reference wherever it has a counterpart, and in each of these the top non-harness
frame is in `avro::`. They belong upstream.

### 8. `GenericDatum` construction recurses until the stack is gone

Found by `DatumCircleAgrees` after 4.7 seconds. `avro::GenericDatum(NodePtr)`
builds a datum for every record field eagerly and guards neither depth nor
cycles, so a record that reaches itself through records alone recurses forever:
247 frames of `GenericRecord` / `GenericDatum::init` at about 1 KiB each, then
SIGSEGV.

```
{"type":"record","name":"R","fields":[{"name":"a","type":"R"}]}          CRASH
A holding B holding A                                                   CRASH
{"type":"record","name":"A","namespace":"ns","fields":[
   {"name":"f","type":{"type":"record","name":"A","fields":[]}}]}        CRASH
same, with an array, map or union anywhere in the cycle                  ok
```

The third shape is the one a caller cannot see coming: the text never names a
type twice, but Avro resolves the inner `A` against the enclosing namespace, so
both definitions land on `ns.A` and avro-cpp turns the second into a symbolic
reference. A container in the cycle saves it, because an array or map builds
empty and a union builds one branch.

The generator was producing that third shape itself: `ResolveName` computed full
names without namespace inheritance, so its "two definitions may not share a full
name" uniquifier compared names the parsers never see. Fixed in
`fuzz/lower_schema.cc`. `ToAvrocppDatum` now also refuses a schema that reaches a
name reference *before* constructing the datum, since construction is what
crashes and the existing check in `Fill` never got to run.

Pinned by `Differential.RecursiveRecordSchemaCrashesAvrocppDatum` and
`Differential.RecursionUnderAContainerIsSafeInAvrocpp`.

### 9. avro-cpp reserves a declared block count before reading any item

`avro::GenericReader::read` (`Generic.cc:112`) resizes an array or map to its
declared count with no check against how much input is left, and avro-cpp has no
allocation ceiling of its own. Two measured cases: 1.6 GB reserved from five
bytes of payload, and a 837 GB request from 27 bytes, which ASan turned into an
abort. Five bytes of varint can declare more items than any machine can hold.

This is the `vector::resize` finding `AGENTS.md` recorded as untriaged. It is now
symbolized, and it applies to maps as well as arrays -- the 837 GB case was
`vector<pair<string, GenericDatum>>::resize`.

`DecodersAgreeOnArbitraryBytes` drops inputs holding an oversized varint so the
class does not end every run; `avro_bytes_fuzz_test.cc` already guards it with
`kMaxDeclaredLengthBytes` and pins the ceiling arithmetic in
`AllocationCeilingChecksCountAgainstAByteLimit`.

### 10. Vertical tab and form feed are whitespace only to avro-cpp -- two positions of three

Found by `ParsersAgreeOnSchemaAcceptance` after 23 seconds. avro-cpp skips
whitespace with `isspace()` (`JsonIO.cc:42`), which accepts vertical tab (0x0B)
and form feed (0x0C). RFC 8259 permits exactly four bytes there -- tab, newline,
carriage return, space -- and serde_json, under the bridge, enforces that.

```
0x0B "int"      avrocpp accepts   bridge rejects
"int" 0x0B      both accept, since C1        (was: bridge rejects)
{"type": 0x0B "int"}   avrocpp accepts   bridge rejects
0x09 / 0x0A / 0x0D / 0x20   both accept
every other byte 0x00 to 0x20   both agree
```

The middle row closed as a side effect of C1, and **not** because serde_json
started treating the byte as whitespace: the binding now stops at the end of the
first JSON document and never reads what follows it. Between two tokens the byte
still has to count as whitespace to be skipped, and it does not, which is why the
third row stands and why the pinning test asserts both halves.

Direction is avro-cpp-lenient, so nothing is mis-decoded. It matters the other
way round: a pipeline that fed avro-cpp a pretty-printed schema containing a form
feed starts failing to parse after moving to the bridge.

Pinned by `AvroBytes.VerticalTabAndFormFeedAreWhitespaceOnlyToAvrocpp`, with its
own `json-whitespace-leniency` tag so the `kKnownDivergences` entry that mutes it
cannot also mute an unrelated parser disagreement.

### 11. Two decoders, one input, different array lengths

Found by `DecodersAgreeOnArbitraryBytes` after 68 seconds and 430,000 runs. Four
bytes under `{"type":"array","items":"null"}` and both engines accept, both
return an array, and the lengths differ. Nothing errors, so neither caller has
any signal that the other side read different data.

```
schema: {"type":"array","items":"null"}
input:  02 01 00 00

strict reading:      2 items (one item, then a one-item sized block, then a
                     zero count ending the array)
ASan fuzzing build:  bridge 32, avro-cpp 26
plain build:         bridge 2,  avro-cpp 0
```

A `null` item occupies zero bytes, so a declared count needs no payload behind it
and neither decoder runs out of input to notice the framing is wrong. Both
fabricate, by different amounts.

The counts move with the build, which means at least one side's length does not
come from the input alone -- a pointer past the end of the buffer is the obvious
suspect and this has not been chased down. Neither pair of numbers is explained.

Pinned by `Differential.ArrayOfNullLengthsDisagree`, which asserts the
disagreement rather than the counts, since a pinned regression must not depend on
the build. This is `ARRAY_LEN`, another ID that had a reporting site but had never
fired.

### 12. A 16-byte uuid string is read as a binary uuid by the bridge -- CLOSED

Found by `DecodersAgreeOnArbitraryBytes` after 293,811 runs. Both engines accepted
and neither errored, so the caller had no signal that the value changed meaning.

```
schema: {"type":"string","logicalType":"uuid"}
input:  20 00*12 62 6f 6c 73     (length 16, twelve NULs, then "bols")

avrocpp: the sixteen bytes verbatim
bridge:  "00000000-0000-0000-0000-0000626f6c73"
```

The tail of the bridge's rendering, `626f6c73`, is "bols" read as hex: the bytes
were reinterpreted, not reformatted. The specification puts `uuid` on `string`
and defines its encoding as the 36-character text form, so avro-cpp is reading
what the spec says is there; apache-avro additionally accepts a 16-byte payload
as a binary uuid, which is what a uuid-on-`fixed(16)` field carries in Avro 1.12.

Reachable only at length exactly 16, which is why five figures of runs were
needed. This was a different failure from finding 6: there the bridge rejected
text avro-cpp kept, here both succeeded and disagreed on the value.

**Closed by a default rather than by a patch.** `install_avro_cpp_defaults` turns
`uuid_as_string` on, so the bridge leaves the annotation uninterpreted as avro-cpp
does and the sixteen bytes come back verbatim. The uuid-as-string patch had been
in `patches/` all along and `avro_bytes_fuzz_test.cc` set it, which is why that
binary already saw the two engines agree; `fuzz/differential_test.cc` never set
it, so this finding was measured at the crate default rather than the bridge's.

Pinned by `Differential.SixteenByteUuidStringSurvivesInBothEngines`, which now
asserts the agreement and the whole circle back to the input bytes.

### 13. avro-cpp fabricates an array item without changing the length

Found by the Tier A checkpoint run of `Differential.DecodersAgreeOnArbitraryBytes`
after 4.15 million runs. An avro-cpp bug, and a sub-case of finding 9's declared
block count reservation that no entry covered.

```
schema: {"type":"array","items":"boolean"}
input:  02 00 01 04 00 00                (six bytes)

bridge:  2 items, both false, every byte consumed
avrocpp: 2 items, one of them a null datum it never read
```

The bridge's reading is the strict one: count 1, one item, then a negative count
of 1 with a byte size of 2, one more item, then a 0 count ending the array.

What makes this worse than finding 11 is that **the two lengths agree**. Finding
11 has them differ, which a caller comparing sizes would notice; here the sizes
match and one element is a value avro-cpp never read, so nothing about the shape
of the result gives the caller a signal. avro-cpp resizes to the declared count
before reading any item (`Generic.cc:112`), so a slot it never fills comes back
as a default-constructed `GenericDatum`, which is `AVRO_NULL`.

**Not caused by the strict-eof patch**, which is the first thing to rule out
since it changed the `Schema::Boolean` decode arm. Measured under a `long` item
schema as well, where it reproduces identically, and the `long` path goes nowhere
near any arm that patch touched. The bridge returns the same two items either way
and avro-cpp is unchanged, so the divergence predates Tier A. What Tier A changed
is reachability: closing four divergences moved the coverage-guided search onto
different ground.

It surfaced now rather than in the validated hour-long run because of a harness
asymmetry, since fixed. `Comparer::CompareArray` reported the array-framing ID
only when the lengths differed; with them agreeing it fell through to per-element
comparison and reported `VALUE_TYPE_MISMATCH` and `SCALAR_VALUE`, neither scoped
to arrays and neither muted. `ARRAY_ITEM_FABRICATED` now names it, keyed on the
signature that identifies it -- avro-cpp holding `null` where the item schema is
not `null` -- so a real value divergence inside an array is still reported as one.

Pinned by `AvroBytes.AvrocppFabricatesAnArrayItemWithoutChangingTheLength`, which
drives both item schemas. It belongs upstream with findings 8, 9 and 11.

### An unexplained memory growth, not a divergence

`DecodersAgreeOnArbitraryBytes` grows its resident set steadily under load: 112 MB
after a minute, 237 MB at five, 412 MB at fifteen, and 847 MB at eighteen, where
it hit its RSS limit and ended the run. Growth is monotonic rather than spiky, so
a corpus of 1,789 small inputs does not explain it.

It is **not** a leak in the malloc sense. A five-minute run with
`detect_leaks=1`, 1.27 million executions, exits clean with nothing from
LeakSanitizer. So the memory is retained and still reachable -- FuzzTest's corpus
and coverage state, or allocator fragmentation under inputs that reserve a
hundred megabytes and drop it again -- rather than lost.

Reducing `kMaxDeclaredCount` from a million to 16k cuts the transient allocation
per input from over 100 MB to a couple. That is what made the property survive a
full hour. What remains unexplained is why the retained set grew as fast as it
did at the larger cap, since the corpus is a few thousand small inputs either way.

### A harness bug worth its own entry: cascade reporting

Not a divergence, and it was masking one. Found by the Tier B checkpoint run,
where `Differential.DecodersAgreeOnArbitraryBytes` reported ten thousand
`SCALAR_VALUE` differences and one `DECIMAL_VALUE`, each carrying the note
"(1 further difference(s) suppressed)".

The suppressed sibling was the finding: `ARRAY_LEN` on an `array` of `null`
(finding 11), muted for the run. Once the two engines read a different number of
items, they are at different offsets in the same buffer, so **every field after
the array reads different bytes**. `Comparer` carried on comparing them and
reported each as an independent difference:

```
schema: record(f0: union(record(f0: array(null))), f1: int)
input:  00 02 01 00 02 00 00

reported: SCALAR_VALUE at $.f1, thousands of times
actual:   ARRAY_LEN at $.f0.f0, suppressed, once
```

`Comparer` now stops the walk at the first difference that proves the two sides
consumed a different number of bytes: an array length or map entry count that
disagrees, or a slot avro-cpp reserved and never read (finding 13). The flag is
set even when the finding is suppressed, because suppression governs whether it
counts as a failure, not whether the offsets moved.

The cost, stated because it is real: an independent divergence in a field after a
size disagreement is not reported for that input. It is not lost -- an input that
reaches the same field without a preceding size divergence still reports it -- and
a difference read at a misaligned offset is not evidence about either engine.

This changed `Differential.DecodersAgreeOnArbitraryBytes` in unit-test mode from
failing every time to failing about half the time, because the array-of-`null`
cascade it used to trip over on nearly every run is no longer a pile of
independent findings. Nothing about either engine changed.

The first report of this said it took `ctest` from six non-passes to five. That was
one lucky run. Measured over 16: **7 passes, 9 failures**, so the `ctest` total is
176 or 177 of 182 depending on the draw. A `FUZZ_TEST` in unit-test mode is a
one-second run from a random seed; pinning `FUZZTEST_PRNG_SEED` makes it
deterministic, 6 of 6 failures. Wiring a fixed seed into the `ctest` registration
would make the suite a stable regression gate, and is not done.

### Harness bugs the run also surfaced

Neither is a divergence; both were breaking the run rather than the product.

- `Normalize` applied its value-bearing scalar clamps *after* the out-of-budget
  early return, so a collapsed node kept `precision 40, scale -116` and violated
  the well-formedness invariant that applies to every node. Found by
  `ValueBearingTreesAreWellFormed`. Fixed by clamping before the collapse.
- `ResolveName` ignored namespace inheritance, described under finding 8.

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
