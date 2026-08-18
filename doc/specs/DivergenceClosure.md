# Spec: Closing the avro-cpp divergences

Status: APPROVED - ready to implement

## Glossary

| Term | Expansion | What it is here |
| --- | --- | --- |
| apache-avro | (a name) | the Rust crate `apache-avro` (the avro-rs project), version 0.21, which the bridge is a binding over. |
| avro-cpp | Apache Avro C++ | `release-1.11.4`. The incumbent, in production for years, and therefore the reference for behaviour. |
| `avro_bridge` | (a name) | the replacement: a Crubit/corrosion binding over apache-avro. Called "the bridge" below. |
| D1, D2 | (a name) | divergence register entries 1 and 2: non-UTF-8 bytes in a `string`, and duplicate keys in a map. |
| divergence register | (a name) | `doc/AvrocppDivergences.md` on `main`, the hand-maintained list of accepted differences. Does not exist on this branch. |
| knob | (a name) | a process-global, set-once `OnceLock<bool>` in `apache_avro::util`, with a public `set_*` setter and a crate-internal reader. `set_non_utf8_string_as_bytes` is the existing example. |
| oracle | (a name) | the step in the fuzz harness deciding whether the two implementations agreed on one input. |
| parity | (a name) | the bridge producing the same accept/reject verdict and the same decoded value as avro-cpp on the same input. |

## Purpose & user problem

`avro_bytes_fuzz_test.cc` and `fuzz/` measure the difference between the bridge
and avro-cpp. They do not reduce it. Eleven entries sit in `kKnownDivergences`
and each one is a place where code that has run against avro-cpp in production
for years would behave differently after the migration.

Measuring was the previous phase and it is done. This phase closes the
divergences, one patch at a time, so that switching a caller from avro-cpp to the
bridge is not a behaviour change.

## The policy this spec is built on

**avro-cpp's behaviour is the bridge's default.** Not the Avro specification's,
and not apache-avro's. Where the three disagree, the default follows avro-cpp,
because the code being migrated was written against avro-cpp and its behaviour is
what that code depends on.

**Every deviation from avro-cpp is reachable through a knob**, so a caller who
wants to be stricter than avro-cpp can be, later, without another patch. The
knob's name always describes the *strict* behaviour, and the bridge leaves it off.

This reverses the polarity of the two patches already committed
(`apache-avro-0.21-non-utf8-string.patch`,
`apache-avro-0.21-uuid-as-string.patch`), which are off by default and turn *on*
to reach parity. Those two stay as they are; see **Reconciling the two
polarities** below.

### Where a fix lives

Preference order, and the reason for it:

1. **An apache-avro patch in `patches/`.** Preferred. The fix reaches every
   caller of the crate, including the paths the bridge does not funnel through
   its own wrappers -- the schema in an object container file header is parsed
   inside apache-avro, so a validation walk written in `rust/schema.rs` would
   not see it.
2. **A bridge patch in `rust/`.** Only when apache-avro has no site to change,
   because the behaviour is the bridge's own. The trailing-bytes check at
   `rust/datum.rs:12` is the one case: apache-avro's `from_avro_datum` already
   behaves like avro-cpp, and it is the bridge that added the rejection.

### Reconciling the two polarities

An apache-avro patch is worth more if it is additive for other consumers of the
crate, which is what the existing two are: nothing changes until a caller opts
in. But this spec's policy wants parity as the *bridge's* default, and for
several divergences upstream's current behaviour is not avro-cpp's.

Both hold at once, with the knob's default and the bridge's default kept
separate:

- **The knob's crate default stays apache-avro's current behaviour**, so each
  patch remains additive and upstreamable on its own terms.
- **The bridge sets the knobs to avro-cpp-compatible values** at one place,
  `avro_bridge_defaults()` in `rust/lib.rs`, called from the top of every bridge
  entry point. The bridge's default is therefore parity.
- **A caller who wants strictness calls the bridge's `SetStrict*` wrapper before
  its first Avro call**, and wins the `OnceLock` because it got there first. This
  is the rule `SetMaxAllocationBytes` already documents, so it is not a new
  constraint on callers.

Five of the thirteen patches below need no knob at all, because for them avro-cpp
*is* the stricter engine and apache-avro's behaviour is unsound rather than
merely lenient. Those are straight bug fixes, on unconditionally, and each says
so.

## Success criteria

1. Each patch closes exactly one `kKnownDivergences` entry or one numbered
   finding, and lands as its own commit. Nothing is batched.
2. After each patch: the entry leaves `kKnownDivergences`, the count in
   `KnownDivergenceTableSizeIsPinned` drops by one, and the test that pinned the
   old behaviour is rewritten to pin the agreement rather than deleted.
3. **Pinning agreement means checking the value, not just the verdict.** The uuid
   patch is the precedent: accepting an input and then writing something else
   back looks closed to an acceptance property while corrupting data.
   `NonUtf8StringRoundTripsThroughBothEngines` checks the whole circle and is the
   shape to copy.
4. Each knob gets a test at both of its values. A `OnceLock` cannot be reset
   in-process, so the second value needs its own test binary, as
   `avro/tests/non_utf8_string_as_bytes.rs` does.
5. `avro_bytes_fuzz_test` stays at 20 of 20 or better in `/opt/dfz-b1`, and
   `ctest` stays at 174 of 180. The five `Differential.*` failures stay: they
   fail by design at this commit and this spec does not touch them.
6. After the last patch of each tier, `fuzz/run_all_parallel.sh 1h` runs with an
   empty suppression list and all thirteen properties exit 0. A closed
   divergence that needed muting was not closed.
7. Every patch is verified against apache-avro's own suite as well as ours:
   `cargo test -p apache-avro --features derive` in
   `/home/hvn/avro-rs-0.21-d1`, and the count is reported. The two pre-existing
   failures there are unrelated and reproduce without any patch.

## Progress

| patch | status | closes |
| --- | --- | --- |
| A1 strict-eof | **landed** | `bridge-lenient` / "EOF reached" |
| A2 empty-union | **landed** | `schema-acceptance` / "bad node of type union" |
| A3 empty-enum | **landed** | `schema-acceptance` / "bad node of type enum" |
| A4 empty-decimal | **landed** | `reencode-failed` / "decimal sign extension 0" |
| A5 container block varint | open, needs measurement | no entry yet |
| B1 trailing bytes after a datum | open | `trailing-bytes` |
| C1 trailing bytes after schema text | open | `schema-trailing-bytes` |
| C2 vertical tab and form feed | open | `json-whitespace-leniency` |
| C3 lenient namespace | open, recommended last or not at all | `schema-acceptance` / "Invalid namespace" |
| D1 duplicate full name | open | finding 4, no entry |
| D2 duration render | open | finding 3, no entry |
| E1/E2 map keys | open, needs its own spec | `avrocpp-lenient` / "Invalid utf-8 string", D2 |

`kKnownDivergences`: 11 entries at the start, 7 now.

Tier A is complete. Every one of its four patches is on unconditionally, and
**three of the four are not additive** for other consumers of apache-avro: each
changed a behaviour an upstream test asserted. Per-patch detail is in
`patches/README.md`.

## The patch series

Thirteen patches, in the order to take them. The tier boundaries are where the
kind of work changes, and each tier ends at a natural stopping point. A5 was
added during implementation, out of the review of A1.

### Tier A - unconditional bug fixes, where avro-cpp is the stricter engine

No knob. apache-avro's behaviour here is not lenient, it is wrong: it accepts
schemas that have no valid encoding, and it manufactures values that were never
on the wire. Parity and correctness point the same way, so there is nothing to
make optional.

#### A1. A truncated buffer must not decode to fabricated values

Closes `{"bridge-lenient", "EOF reached"}`. Finding 1 in `fuzz/FINDINGS.md`, and
the most serious of the eleven.

**Three** sites in `decode.rs` swallow an end-of-input error and invent a value.
The spec originally named two; implementation found the third:

| site | what it returns at end of input |
| --- | --- |
| `decode.rs:96` (`Schema::Boolean`) | `Ok(Value::Null)` |
| `decode.rs:203` (`Schema::String`) | `Ok(Value::Null)` |
| `decode.rs:290` (`Schema::Union`) | `Ok(Value::Union(0, Box::new(Value::Null)))` |

None of the three inhabits its own schema: `null` is not a value of `boolean` or
of `string`, and branch 0 of a union is only `null` if the union happens to
declare it there. A record of two booleans therefore decodes from zero bytes
into `{"a":null,"b":null}` with a success status, where avro-cpp reports
`avro::decode: EOF reached`.

The `Schema::String` site is the one nothing had reached. An empty buffer under
`"string"` already failed, at the missing length prefix, so no test or property
exercised a length prefix with too few bytes behind it. `04 61` (length 2, one
byte) decoded to `Value::Null`.

The fix propagates the error at all three, about ten lines. No knob: nothing
here is worth preserving.

Test: rewrite `EmptyInputIsDecodedByTheBridgeOnly` to assert both engines
reject, keeping its per-type table (`"null"` accepts on both sides, and that
stays true -- `null` really is zero bytes), and add the truncated-payload row the
old table could not reach.

**Not additive upstream.** `reader::tests::test_from_avro_datum_with_union_to_struct`
(AVRO-3240) encoded two of its record's five fields and relied on the union arm
to fabricate the other three, described in the test as simulating missing keys.
Truncation is not a writer that lacked those fields, which is what schema
resolution is for, so the test asserts the error and gains a fully-encoded
payload. Every other patch here is either off by default or purely additive;
this one changes what an existing consumer of the crate gets back.

#### A5. A container file cut inside a block-count varint must not read clean

Found by the review of A1, by reading rather than by the fuzzer, because the
harness has **no object-container coverage at all** -- it drives
`from_avro_datum` directly. So this one has a reproducer but its avro-cpp
behaviour is not yet measured, and that measurement comes before the fix.

Same root cause as A1, one level up. `decode_variable` (`util.rs:139`) reads a
varint one byte at a time and returns the same `Details::ReadVariableIntegerBytes`
whether `read_exact` failed on the **first** byte (a clean end of file, which for
the container format is how a file legitimately ends) or on a **continuation**
byte (a varint that promised more and was cut). `Block::read_block_next`
(`reader.rs:165`) cannot tell them apart, so it treats both as a clean end and
returns `Ok(())`, leaving `message_count` at 0, and `read_next` returns
`Ok(None)`: iteration finished normally.

Measured on a two-block `"int"` file with a single `0x80` appended, a varint with
the continuation bit set and nothing behind it: `Reader` yields 2 items and
**zero errors**. A caller reading a file cut short by a crashed writer or a
truncated upload gets a successful, shortened iteration.

The fix has to distinguish the two cases at the source, since `decode_variable`
discards the information: report which byte of the varint hit the end, and let
`read_block_next` accept only a failure on the first one.

Sequenced after A4 rather than next to A1, because it needs a differential
measurement the harness cannot currently make, and building object-container
coverage is item 6 in `AGENTS.md`'s next steps.

#### A2. An empty union has no valid encoding and must be rejected

Closes `{"schema-acceptance", "bad node of type union"}`.

`UnionSchema::new` (`schema.rs:914`) accepts an empty `schemas` vector. Every
branch index into an empty union is out of range, so no byte sequence decodes
under it, yet the bridge parses `[]` and re-renders it. avro-cpp rejects it as a
"bad node of type union".

Reject in `UnionSchema::new`. Before writing it, check that no internal caller
constructs `UnionSchema::new(vec![])` on purpose -- resolution and default
handling both build unions.

Test: rewrite `EmptyUnionIsAcceptedByTheBridgeOnly` to assert both reject,
including the nested-as-a-record-field-type case the existing test covers.

#### A3. An empty enum must be rejected

Closes `{"schema-acceptance", "bad node of type enum"}`.

`parse_enum` (`schema.rs:1859`) accepts an empty `symbols` array. Same shape as
A2: no symbol index is in range. avro-cpp rejects it as a "bad node of type
enum".

Test: rewrite `EmptyEnumIsAcceptedByTheBridgeOnly`.

#### A4. A zero-length decimal must round-trip

Closes `{"reencode-failed", "decimal sign extension 0"}`.

A single `0x00` under a decimal schema is a length prefix of zero, so the
unscaled value is an empty byte array. Both engines accept it. avro-cpp
re-encodes it to the byte it came from. The bridge produces a `Value::Decimal`
that neither `GetDecimalBytes` nor its own encoder will take back, both failing
with the same message, so a caller that decodes this input cannot use or forward
the result.

Root cause, at `decimal.rs:91-99`: `Decimal::from(&[])` stores `len = 0` and
`value = 0`. `BigInt::to_signed_bytes_be()` of zero is one byte, `0x00`, so
`num_raw_bytes = 1` and `len.checked_sub(1)` underflows into
`Details::SignExtend { requested: 0, needed: 1 }`.

The fix, in `to_sign_extended_bytes_with_len`: when `len == 0` and the value is
zero, return an empty vector. That re-encodes to the zero-length byte array
avro-cpp produces, closing the circle. Three lines.

Test: rewrite `EmptyDecimalDecodesToAValueTheBridgeCannotReadOrReencode` to
assert both engines complete the circle back to the input byte.

### Tier B - the bridge's own added strictness

#### B1. Trailing bytes after a single datum

Closes `{"trailing-bytes", "trailing bytes"}`. Finding 7.

avro-cpp stops at the end of the first datum and ignores what follows: `02 ff`
under `"int"` decodes to `1`. The bridge rejects it, and this rejection is the
bridge's own -- `TRAILING_BYTES_ERROR` at `rust/datum.rs:12`, checked at four
call sites. apache-avro's `from_avro_datum` already behaves like avro-cpp, so
there is no apache-avro site to patch and this one is bridge-side by necessity.

The bridge's four `if !reader.is_empty()` checks become conditional on a new
bridge-side knob:

```
SetRejectTrailingBytes(bool)   // avro_bridge.h, default false
```

Default false is avro-cpp's behaviour. The existing `decode_rejects_trailing_bytes`
unit test moves under the knob's `true` value, in its own test binary.

Callers relying on the rejection: this is the one patch in the series that
*removes* a safety check the bridge shipped with. Trailing bytes after a single
datum usually mean framing has gone wrong, and the bridge's behaviour is the more
defensible of the two. The policy says parity wins and the knob is how a caller
gets the check back, but this is the item most worth a second look before it
lands. Flagged under **Open questions**.

Test: rewrite `TrailingBytesAreAcceptedByAvrocppOnly` to assert both accept and
both decode to the same value at the default, plus a knob-on test asserting the
bridge rejects.

### Tier C - avro-cpp leniency the bridge does not have

Each of these three is a knob in `apache_avro::util`, crate default matching
apache-avro's current behaviour, set to avro-cpp's by `avro_bridge_defaults()`.

#### C1. Trailing bytes after the schema JSON

Closes `{"schema-trailing-bytes", "disagree on whether this schema is legal"}`.

avro-cpp stops once it has one complete JSON value and never looks at what
follows, so `"int"` followed by anything still parses. `Schema::parse_str` calls
`serde_json::from_str`, which rejects trailing content. The bridge reports it
three different ways depending on where the trailing bytes fail first, which is
why the harness classifies this by shape (`ParsesAsSchemaPrefix`) rather than by
message.

Fix: parse through `serde_json::Deserializer::from_str` and take the first value,
ignoring the remainder, under

```
util::set_strict_schema_trailing_bytes(bool)   // crate default true
```

Both `parse_str` and `parse_list` need it.

#### C2. Vertical tab and form feed as JSON whitespace

Closes `{"json-whitespace-leniency", "disagree on whether this schema is legal"}`.
Finding 10.

avro-cpp skips whitespace with `isspace()` (`JsonIO.cc:42`), which accepts
vertical tab (0x0B) and form feed (0x0C). RFC 8259 permits exactly four bytes
there, and serde_json enforces that. A pipeline that fed avro-cpp a
pretty-printed schema containing a form feed starts failing to parse after the
migration, which is the direction that matters.

Fix: a pre-pass in `Schema::parse_str` replacing 0x0B and 0x0C with 0x20
**outside string literals**, under

```
util::set_strict_json_whitespace(bool)   // crate default true
```

The pre-pass has to track string-literal state and backslash escapes, or it
corrupts a schema whose `doc` or enum symbol legitimately contains a form feed.
That is the whole risk in this patch and it needs its own tests: a form feed
inside a string stays a form feed, one outside becomes a space, and
`\\` before a quote does not end the literal.

Cheaper alternative worth weighing: leave the bytes alone and pre-scan only to
decide whether to reject, matching avro-cpp by *accepting* rather than by
rewriting. Rejected here because serde_json still has to be handed something it
will parse. Noted so the decision is visible.

#### C3. A namespace with an empty component

Closes `{"schema-acceptance", "Invalid namespace"}`. Finding 5.

`{"type":"fixed","name":"B","namespace":"ns..bad","size":16}` has an empty middle
component. The Avro specification does not allow it; avro-cpp accepts it and
apache-avro rejects it at `schema.rs:288` (`validate_namespace`).

Fix: skip the empty-component check under

```
util::set_strict_namespace(bool)   // crate default true
```

**This is the patch this spec recommends landing last, or not at all.** The other
eleven either fix something wrong or match a tolerance that costs nothing. This
one makes the bridge accept a schema that other Avro implementations reject, so
data written under it may be unreadable outside this deployment. Parity says do
it; the cost is real and belongs to the reader of this spec, not to me. Flagged
under **Open questions**.

### Tier D - larger apache-avro changes

#### D1. A duplicate full name must not panic

Closes finding 4. No `kKnownDivergences` entry, because it is pinned in
`fuzz/differential_test.cc` rather than the byte harness.

```
{"type":"record","name":"foo","namespace":"ns","fields":[
  {"name":"a","type":{"type":"record","name":"foo","namespace":"ns","fields":[]}}]}
```

Defining `ns.foo` twice is illegal Avro. Both engines accept it at parse time.
apache-avro then panics at encode time (`types.rs:369`, "Schemata didn't
successfully resolve"). `catch_panic` contains it, so the caller gets a status
rather than an abort, but that guard is the only thing between an untrusted
schema and a process abort, and any entry point missing the guard is a denial of
service.

Parity is not available here, because avro-cpp is also broken on this input: it
turns the second definition into a symbolic reference and then recurses to
SIGSEGV building a `GenericDatum` from it (finding 8, an avro-cpp bug for
upstream). Reproducing that is not a goal.

Proposal, in two parts:

- **Unconditional**: turn the `types.rs:369` panic into an error. A malformed
  schema must not panic, whatever else is decided.
- **Under a knob**: reject at parse time, so the error arrives where the schema
  does rather than at the first encode.

```
util::set_strict_duplicate_names(bool)   // crate default false
```

The default is `false` here, not `true`, because parse-time rejection is
*stricter* than both engines and the policy leaves the bridge at the more lenient
default. That makes the panic fix the part that ships on by default. Flagged
under **Open questions** since the argument for rejecting at parse is strong.

#### D2. A `duration` fixed must re-render in a shape avro-cpp can read

Closes finding 3, the most consequential for the migration itself.

```
in:   {"type":"fixed","name":"B","namespace":"ns","size":12,"logicalType":"duration"}
out:  {"type":{"type":"fixed","name":"duration","size":12},"logicalType":"duration"}

avro-cpp on the output: Json field "type" is not a string
```

A schema that has passed through the bridge can no longer be read by avro-cpp,
which breaks a partly-migrated deployment in the direction that hurts: a
bridge-side writer publishes a schema an avro-cpp-side reader must consume.

Two defects, two sizes of fix, at `schema.rs:2262`:

- **The fixed is nested inside `"type"` as an object.** apache-avro serializes
  `Schema::Duration` by synthesizing an inner `FixedSchema` and writing it as a
  nested value. Flattening it to
  `{"type":"fixed","name":...,"size":12,"logicalType":"duration"}` is about
  fifteen lines and is what restores avro-cpp's ability to read the output.
- **Name and namespace are dropped.** `ns.B` comes back as `duration` because
  `Schema::Duration` is a unit variant carrying no name: the information is lost
  at parse, before the serializer sees it. Fixing this means the variant carries
  a `FixedSchema`, which is a breaking change to a public enum and touches every
  `match` over `Schema` in the crate and in `rust/schema.rs`.

Land the flattening first as its own commit, then the variant change. The first
closes the interop break; the second closes schema identity.

### Tier E - apache-avro data-model changes

Both of these need `Value::Map` to stop being a `HashMap<String, Value>`. Doing
them together is the only sensible order, because they are the same change viewed
from two sides. This tier is where the series stops being a set of small patches,
and it deserves its own spec before implementation.

#### E1. Non-UTF-8 map keys, and E2. duplicate map keys

Closes `{"avrocpp-lenient", "Invalid utf-8 string"}` and D2.

avro-cpp's `GenericMap::Value` is a `std::vector<std::pair<std::string,
GenericDatum>>`: keys are byte strings, and duplicates are kept. apache-avro's
`Value::Map` is a `HashMap<String, Value>`, so a non-UTF-8 key cannot be
represented at all, and a duplicate key silently replaces the first -- which is
also what `MapPut` does at `rust/value.rs:560-569`, losing an entry with no
error.

Silent data loss is the divergence register's worst class and this is squarely in
it. It is last in the series because the representation change is wide, not
because it matters least.

Options to weigh in that spec, not here: change the key type to `Vec<u8>`; add a
parallel `Value::MapBytes`; or keep `HashMap` and carry duplicates in a
side-table. The first is cleanest and the most breaking.

#### Not in the series: array and map block framing

`{"array-block-framing", "\"type\":\"array\""}` covers four messages and one
root cause, and the array-of-`null` length disagreement (finding 11) is the same
territory. **Parity is the wrong goal here**, because avro-cpp is the engine
behaving worse: on one 20-byte input it invents nine items that were never
encoded, and on another it dies of `std::length_error` sizing a vector before
reading anything.

Recommendation: leave the bridge strict, keep the entry as an accepted
difference, and write the avro-cpp side up for upstream. Converging would mean
reproducing avro-cpp's fabrication.

The array-of-`null` case additionally has no root cause yet: the counts move
between the ASan and plain builds, so at least one side's length does not come
from the input alone, and a read past the end of the buffer is the obvious
suspect. That needs chasing before anything is decided, and `AGENTS.md` already
lists it as the first next step.

### Out of scope: avro-cpp bugs with no bridge-side fix

Three entries are not divergences the bridge can close, because the defect is on
the avro-cpp side and the top non-harness frame is in `avro::`. They belong
upstream and stay in `kKnownDivergences` as accepted differences:

| entry | avro-cpp defect |
| --- | --- |
| `{"alloc-ceiling", "Unable to allocate"}` | avro-cpp has no allocation ceiling of any kind |
| finding 8 (no entry) | `GenericDatum(NodePtr)` recurses to SIGSEGV on a record-only name cycle |
| finding 9 (no entry) | `GenericReader::read` (`Generic.cc:112`) resizes to a declared block count before reading any item: 837 GB requested from 27 bytes |

Matching avro-cpp on these would mean removing the bridge's allocation ceiling
and adding an unbounded recursion, which is the opposite of the reason for the
migration.

## Scope & constraints

- **Thirteen patches, thirteen commits**, in the order above. A checkpoint after each
  tier, not after each patch.
- **`patches/` lives in `/home/hvn/orva`, not in this worktree.** Each apache-avro
  patch is a `git format-patch` artifact there, stacked on the two existing ones
  in order, with a `patches/README.md` row recording base, purpose, and the
  verification numbers.
- **The stack order is fixed** by the files the existing patches touch
  (`decode.rs`, `encode.rs`, `types.rs`, `util.rs`): non-UTF-8, then uuid, then
  this series in the order above.
- **The machine-local redirect stays uncommitted.** `.cargo/config.toml` in this
  worktree points at `/home/hvn/avro-rs-0.21-d1`; `patches/README.md` rules out
  committing a checkout path and `rust/Cargo.lock` stays pointed at the crates.io
  release. Note the trap already recorded there: a `[patch]` section in
  `rust/Cargo.toml` is silently ineffective, because Crubit invokes cargo twice
  and the second pass uses a synthesized root manifest that drops it.
- **Both build trees get used.** `/opt/dfz-b1` (unit-test mode) for the test
  suites, `/opt/dfz-fuzz` (fuzzing mode) for the hour-long runs.
  `--parallel 2` in the fuzzing tree: 7 GB of RAM, and that build compiles
  abseil, RE2, ANTLR, FuzzTest and avro-cpp under AddressSanitizer.
- **A knob is set-once.** `avro_bridge_defaults()` must run before the first
  decode and after any caller-supplied `SetStrict*`, which means it runs at the
  top of each bridge entry point rather than from a constructor.

## Out of scope

- The five `Differential.*` failures in `fuzz/`. They fail by design at this
  commit and `AGENTS.md` says not to silence them.
- `doc/CanonicalFormBug.md`. avro-cpp 1.11.4 has no canonical-form or
  fingerprint API, so there is nothing to compare and it is not a divergence.
- The three unfixed harness traps in `AGENTS.md` (`options.suppressions` never
  set, the 13 unreachable suppression IDs, the coverage-flag omission). Real, and
  separate work.
- `D13`+ entries in the divergence register. That file does not exist on this
  branch.
- Anything upstream of the bridge: reporting the three avro-cpp bugs to Apache is
  worth doing and is not this spec.

## Open questions

Four, and each changes what gets written. My recommendation is given, but these
are yours.

1. **B1, trailing bytes after a datum.** This is the only patch that removes a
   check the bridge shipped with, and the bridge's behaviour is the more
   defensible of the two. Land it (parity, knob to restore), or leave the entry
   open? *Recommendation: land it. It is exactly the case the policy was written
   for -- production code may well pass a padded or over-allocated buffer.*
2. **C3, the malformed namespace.** Parity means accepting a schema the spec
   forbids and other implementations reject. Land it, or leave the entry open and
   report the avro-cpp side upstream? *Recommendation: leave it open. It is the
   one divergence where parity makes the bridge's output less portable rather
   than more compatible.*
3. **D1, the duplicate full name.** The panic fix is not in question. Should
   parse-time rejection be the default rather than knob-gated? It is stricter
   than both engines, so the policy says knob-gated, but an illegal schema
   reaching encode before failing is a poor trade. *Recommendation: knob-gated
   as written, and revisit once the panic is gone.*
4. **Tier E.** Confirm it gets its own spec rather than being attempted here.
   *Recommendation: yes. Changing `Value::Map`'s key type touches the crate's
   public API, `rust/value.rs`, `rust/decode.rs` and the bridge header, and the
   choice between three representations is a design decision, not a patch.*

## Does this capture your intent? Any changes needed?

Spec looks good? Type 'GO!' when ready to implement.
