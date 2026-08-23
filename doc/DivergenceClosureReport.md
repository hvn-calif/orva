# Report: closing the avro-cpp divergences, Tiers A and B

Written 2026-08-19, branch `avro-diff-fuzz`, worktree `/home/hvn/orva-difffuzz`.

The differential fuzzer measured the difference between `avro_bridge` and avro-cpp.
This is the first phase of reducing it. Five divergences are closed, `kKnownDivergences`
is down from eleven entries to six, and the work turned up two defects the fuzzer had
not previously reached plus three weaknesses in the harness itself.

## Glossary

| Term | Expansion | What it is here |
| --- | --- | --- |
| apache-avro | (a name) | the Rust crate `apache-avro` (the avro-rs project), version 0.21, which the bridge binds. |
| avro-cpp | Apache Avro C++ | `release-1.11.4`. The incumbent, in production for years, and therefore the reference for behaviour. |
| `avro_bridge` | (a name) | the replacement: a Crubit/corrosion binding over apache-avro. Called "the bridge" below. |
| AVRO-3240 | (a name) | an upstream apache-avro issue whose test relied on a truncated buffer fabricating union branches. |
| D2 | (a name) | divergence register entry 2: duplicate keys in a map. |
| divergence register | (a name) | `doc/AvrocppDivergences.md` on `main`. Does not exist on this branch. |
| knob | (a name) | a process-global, set-once `OnceLock<bool>` with a public setter, off by default. |
| parity | (a name) | the bridge producing the same accept/reject verdict and the same decoded value as avro-cpp on the same input. |

## The policy the series follows

**avro-cpp's behaviour is the bridge's default.** Not the Avro specification's, and
not apache-avro's. Where the three disagree the default follows avro-cpp, because the
code being migrated was written against avro-cpp and its behaviour is what that code
depends on.

**Every deviation from avro-cpp is reachable through a knob** named after the *strict*
behaviour, which the bridge leaves off, so a caller can be stricter later without
another patch.

The full plan, its ordering, and the open questions are in
`doc/specs/DivergenceClosure.md`. That spec is APPROVED and tracks progress in a table
near the top.

## What is closed

Five commits on `avro-diff-fuzz`, one divergence each, nothing batched.

| commit | divergence closed | fix lives in |
| --- | --- | --- |
| `044c686` | `bridge-lenient` / "EOF reached" | `apache-avro-0.21-strict-eof.patch` |
| `8026bad` | `schema-acceptance` / "bad node of type union" | `apache-avro-0.21-empty-union.patch` |
| `8f1db74` | `schema-acceptance` / "bad node of type enum" | `apache-avro-0.21-empty-enum.patch` |
| `f71d34a` | `reencode-failed` / "decimal sign extension 0" | `apache-avro-0.21-empty-decimal.patch` |
| `afd4aed` | `trailing-bytes` / "trailing bytes" | the bridge, `rust/datum.rs` |

The four patches live in `/home/hvn/orva/patches/`, each a `git format-patch`
artifact with a row and a section in `patches/README.md`. They stack in the order
above on top of the two patches that were already there:

```
rel/release-0.21.0
  6ce258c  non-utf8-string      (pre-existing)
  ac9ee5b  uuid-as-string       (pre-existing)
  bb6f9a5  strict-eof
  d2b5aca  empty-union
  187ce7d  empty-enum
  9786b7e  empty-decimal
```

Applied to the local checkout at `/home/hvn/avro-rs-0.21-d1`. The redirect pointing
the bridge at that checkout is machine-local and stays uncommitted; `/.cargo/` is now
gitignored, because it had already leaked the checkout path into `rust/Cargo.lock`
once.

### 1. A truncated buffer no longer decodes to fabricated values

The most serious of the eleven. Three arms of apache-avro's `decode_internal` treated
`ErrorKind::UnexpectedEof` as a value rather than an error:

| site | returned at end of input |
| --- | --- |
| `decode.rs:96` (`Schema::Boolean`) | `Ok(Value::Null)` |
| `decode.rs:203` (`Schema::String`) | `Ok(Value::Null)` |
| `decode.rs:290` (`Schema::Union`) | `Ok(Value::Union(0, Box::new(Value::Null)))` |

None inhabits the schema it was decoded under. A record of two booleans decoded from
**zero bytes** into `{"a":null,"b":null}` with an `Ok` status, where avro-cpp reports
"EOF reached". Branch 0 of a union is only `null` when the union declares it first, so
`["int"]` produced a union whose payload had the wrong type.

`Schema::Null` still decodes from an empty buffer, which is correct and is why the fix
could not be "reject an empty buffer": a `null` datum occupies no bytes.

**The spec named two sites. There were three.** The `Schema::String` arm was reached by
nothing: an empty buffer under `"string"` already failed at the missing length prefix,
so no test or property exercised a length prefix with too few bytes behind it. `04 61`
(length 2, one byte present) decoded to `Value::Null`.

### 2 and 3. Empty union and empty enum are rejected

`[]` parsed and re-rendered as `[]`; `{"type":"enum","name":"E","symbols":[]}` parsed.
No branch or symbol index is in range for either, so neither has any valid encoding:
nothing can be encoded into one and no byte sequence decodes under one. avro-cpp
rejects both.

The two needed separate patches because they guard differently, and the difference is
worth keeping in mind:

- `UnionSchema`'s fields are crate-private, so the check in `UnionSchema::new` covers
  everything the crate can build.
- `EnumSchema` has public fields and a `bon` builder, so the check in `parse_enum`
  covers the **parse path only**. A Rust caller can still hand-build one with no
  symbols. Untrusted input arrives by parsing, which is what matters.

`["int"]` and a one-symbol enum stay legal on both sides, and every pinned test asserts
that, so neither fix can creep into a construct that does have a valid encoding.

### 4. A zero-length decimal round-trips

A single `0x00` under a decimal schema is a length prefix of zero, so the unscaled value
is an empty byte array. Both engines accepted it and avro-cpp re-encoded it to the byte
it came from. The bridge produced a value that said it was a decimal and could then be
neither read nor re-encoded, both failing with the same sign-extension message, so a
caller had no way to use or forward the result.

Root cause, all arithmetic: `Decimal::from(&[])` stores `len = 0` and `value = 0`, since
`BigInt::from_signed_bytes_be(&[])` is zero. In `to_sign_extended_bytes_with_len`,
`to_signed_bytes_be()` of zero is one byte, so `num_raw_bytes` is 1 and
`len.checked_sub(1)` on `len == 0` underflows.

The guard is narrow and pinned both ways: a non-zero value asked for zero bytes still
fails, and a zero value asked for a wider field is still sign-extended.

Whether the input is well formed at all is a separate question this does not settle.
Avro describes an unscaled value as a two's-complement big-endian integer, which needs
at least one byte, so rejecting a zero-length one would also be defensible. avro-cpp
accepts it, so the bridge follows avro-cpp.

### 5. Trailing bytes after a datum are ignored

The only closure that is bridge-side rather than an apache-avro patch, because
apache-avro's `from_avro_datum` already stops at the end of the first datum like
avro-cpp: the rejection was the bridge's own addition.

It is also the only one that **removes** a check the bridge shipped with, so the strict
reading stays reachable:

```
SetRejectTrailingBytes(bool)   // avro_bridge.h, off by default
```

Off by default is avro-cpp's behaviour, and code being migrated may hand a padded or
over-allocated buffer to a decode. The knob's `true` value needs its own process,
because the setting is a `OnceLock` no test can reset. That used to be
`rust/tests/reject_trailing_bytes.rs`; it is now the second build of
`avro_bridge_test.cc`, `avro_bridge_strict_test`, which flips every setting the
header exposes and so covers both values of each without a binary per knob.

Every pinned test also asserts a **truncated** datum is still an error. Ignoring bytes
left over after a complete datum and accepting a datum with bytes missing are different
things, and conflating them would undo closure 1.

Two things were deleted rather than left to rot: the `TRAILING_BYTES` divergence ID,
which `suppress.h` had declared long before anything could report it, and
`TrailingBytesExplainIt`, which labelled a bridge rejection as trailing bytes whenever
avro-cpp re-encoded shorter than its input. That heuristic never looked at *why* the
bridge rejected, so with the bridge no longer rejecting over leftovers it would have
named the wrong cause. `TRAILING_BYTES` also left `run_all_parallel.sh`'s suppression
default, so anything that hid behind that label now reports under its real category.

## Three of the four patches are not additive upstream

This matters for anyone reading `patches/` and assuming the whole directory is safe to
apply to a shared dependency. The two pre-existing patches are off-by-default settings.
Three of the four new ones change what an existing consumer of the crate gets back,
deliberately, and each had an upstream test asserting the old behaviour:

| patch | upstream test that changed | what it had asserted |
| --- | --- | --- |
| strict-eof | `reader::tests::test_from_avro_datum_with_union_to_struct` | AVRO-3240: two of a record's five fields encoded, the three trailing unions decoding from nothing, described in the test as simulating missing keys |
| empty-union | `schema::tests::avro_3946_union_without_any_types` | that a record with a `"type": []` field parses and only logs |
| empty-union | `schema_compatibility::tests::test_compatible_reader_writer_pairs` | two rows built from an `empty_union_schema()` helper, both vacuous since no value inhabits an empty union |
| empty-enum | two custom-attribute tests | nothing relevant: they used `"symbols": []` as a placeholder and assert only on `custom_attributes()`, so each fixture gained one symbol |

The AVRO-3240 case is the one to understand. Truncation is not the same thing as a
writer that lacked those fields, which is what schema resolution is for. Its test now
asserts the error and gained a payload with all five fields encoded, so it still covers
what it was written for: a key absent from the target struct.

The empty-decimal patch is the only one of the four that is additive.

## Two defects found on the way, neither a bridge divergence

### Finding 13: avro-cpp fabricates an array item without changing the length

From the Tier A checkpoint run, after 4.15 million executions. An avro-cpp bug, and a
sub-case of the declared-block-count reservation (`Generic.cc:112`) that no entry
covered.

```
schema: {"type":"array","items":"boolean"}
input:  02 00 01 04 00 00                (six bytes)

bridge:  2 items, both false, every byte consumed
avrocpp: 2 items, one of them a null datum it never read
```

The bridge's reading is the strict one: count 1, one item, then a negative count of 1
with a byte size of 2, one more item, then a 0 count ending the array.

What makes it worse than the already-recorded finding 11 is that **the two lengths
agree**. Finding 11 has them differ, which a caller comparing sizes would notice; here
the sizes match and one element is a value avro-cpp never read, so nothing about the
shape of the result gives the caller a signal.

**Measured as pre-existing, not argued.** It reproduces identically under a `long` item
schema, and the `long` decode path goes nowhere near any arm the strict-eof patch
touched. The bridge returns the same two items either way and avro-cpp is unchanged.
What Tier A changed is reachability: closing four divergences moved the coverage-guided
search onto different ground.

Pinned by `AvroBytes.AvrocppFabricatesAnArrayItemWithoutChangingTheLength`. Belongs
upstream with findings 8, 9 and 11.

### A5: a container file cut inside a block-count varint reads as a clean end

Found by reading rather than by the fuzzer, because the harness has **no
object-container coverage at all**: it drives `from_avro_datum` directly.

`decode_variable` (`util.rs:139`) reads a varint one byte at a time and returns the same
`Details::ReadVariableIntegerBytes` whether `read_exact` failed on the **first** byte (a
clean end of file, which is how a container file legitimately ends) or on a
**continuation** byte (a varint that promised more and was cut). `read_block_next`
(`reader.rs:165`) cannot tell them apart, so it treats both as a clean end and
`read_next` returns `Ok(None)`: iteration finished normally.

Measured on a two-block `"int"` file with a single `0x80` appended: `Reader` yields
**2 items and zero errors**. A caller reading a file cut short by a crashed writer or a
truncated upload gets a successful, shortened iteration.

**Unfixed**, deliberately. Its avro-cpp behaviour is unmeasured, and measuring it needs
object-container coverage the harness does not have. It is A5 in the spec with the
reproducer.

## Three harness weaknesses, all fixed except the last

Closing five divergences moved the fuzzer onto ground the suppression list had never
had to cover. That exposed problems in the harness rather than in either engine, and
this is the part most likely to matter for future work.

### The comparer reported cascades as independent findings

Once the two engines decode a different number of array items they sit at different byte
offsets in the same buffer, so **every field after that reads different bytes**.
`Comparer` walked on and reported each as its own divergence:

```
schema: record(f0: union(record(f0: array(null))), f1: int)
input:  00 02 01 00 02 00 00

reported: SCALAR_VALUE at $.f1, ten thousand times
actual:   ARRAY_LEN at $.f0.f0, suppressed, once
```

So a documented, suppressed divergence buried itself under its own consequences.
`ReportAndStopComparing` now sets `offsets_diverged`, checked at the top of `Compare`
and again at the top of each collection loop. The second check is not redundant: those
loops have branches that `Report` and `continue` without routing through `Compare`, so
gating only `Compare` would leave a reader auditing each branch.

The flag is set even when the finding is suppressed, because suppression governs whether
something counts as a failure, not whether the offsets moved. D2 is unaffected, and
correctly so: the duplicate-map-key case returns before the arity check and uses plain
`Report`, since a duplicate key means both engines consumed the same bytes and the
bridge produced fewer entries.

The cost is real and is written into the code rather than glossed: an independent
divergence in a field after a size disagreement is not reported for that input. It is
not lost, because an input reaching the same field without a preceding size divergence
still reports it, and a difference read at a misaligned offset is not evidence about
either engine.

### Array framing was attributed by length only

`CompareArray` reported the array-framing ID only when the lengths *differed*, so the
equal-length case fell through to the unscoped `VALUE_TYPE_MISMATCH` and `SCALAR_VALUE`
tags. `ARRAY_ITEM_FABRICATED` now names it, keyed on the signature that identifies it,
avro-cpp holding `null` where the item schema is not `null`, so a real value divergence
inside an array is still reported as one. That ID is verified to be recognised rather
than silently ignored: a bogus ID aborts the binary with exit 134, this one does not.

### `ctest` is not a stable number, and that is unfixed

`Differential.DecodersAgreeOnArbitraryBytes` is **flaky** in unit-test mode. A
`FUZZ_TEST` there is a one-second run from a random seed, so whether it draws an input
that reveals a divergence is chance.

| property | measured |
| --- | --- |
| `Differential.DatumCircleAgrees` | 8 of 8 fail |
| `Differential.SchemaVerdictsAgree` | 8 of 8 fail |
| `Differential.SchemasCrossParse` | 8 of 8 fail |
| `Differential.SchemaTextVerdictsAgree` | 8 of 8 fail |
| `Differential.DecodersAgreeOnArbitraryBytes` | **7 pass, 9 fail across 16 runs** |

So `ctest` is 244 **or** 245 of 250. Pinning `FUZZTEST_PRNG_SEED` makes it
deterministic, measured at 6 of 6 failures with the seed fixed.

**This is the change that would most improve the "did I break anything" question**, and
it is not done. It cost two debugging rounds during this series. Wiring a fixed seed
into the `ctest` registration would turn the suite back into a stable regression gate;
the cost is that unit-test mode stops stumbling onto anything new, which the
fuzzing-mode runs do far better anyway. Recorded in `AGENTS.md` with the measurement.

## Corrections to claims made during this work

Recorded so they are not re-derived from the older text.

1. **"The cascade fix took `ctest` from six non-passes to five."** Wrong, and it came
   from a single run. The property is flaky, 7 passes across 16 runs. What the fix
   actually did was turn one deterministic failure into a flaky one.
2. **The spec's A1 section named two fabrication sites.** There were three; the
   `Schema::String` arm was the one nothing had reached. The spec is corrected.
3. **A success criterion asked for all thirteen properties to exit 0 "with an empty
   suppression list".** That cannot pass while any divergence is open, and eight of the
   thirteen would die in their first second. It now asks that no suppression be added
   for a divergence the series claims to have closed, and that the duration run be
   stated rather than implied.
4. **`AGENTS.md` said `kKnownDivergences` was down to 7 entries** when the code said 6.
   Fixed. A reviewer caught it.
5. **A knob test asserted only a return value.** `set(true)` then `set(false)`
   returning `true` holds even for a plain mutable global with no first-call-wins
   semantics, so the test proved nothing. It now asserts the decode still rejects after
   the second call, and passes under both thread orderings.

## Verification

Per closure, the property that would catch a recurrence was run in the fuzzing tree
**with the table entry removed**, so nothing could mute it:

| property | executions |
| --- | --- |
| `DecodersAgreeOnAcceptance` | 8.0 million |
| `ReencodingAgreesWhenBothDecode` | 5.6 million, then again after the decimal closure |
| `ParsersAgreeOnSchemaAcceptance` | three separate five-minute runs |

Suite state now:

| suite | result |
| --- | --- |
| `avro_bytes_fuzz_test` | 21/21 |
| `avro_bridge_test` | 61/61, and `avro_bridge_strict_test` the same 61 |
| the bridge's Rust suite | 65/65 across four binaries |
| apache-avro's own suite | 567, up from 558 |
| `ctest` | 244-245/250, see the flakiness section |

Tier checkpoint, all thirteen properties concurrently for 20 minutes:

```
./fuzz/run_all_parallel.sh 20m ./fuzzrun/tierB 1200
```

**Thirteen of thirteen exit 0**, 63.1 million executions total, peak resident 1.87 GB
against 2.5 GB available. Per-property throughput ranged from 1.1 million runs
(`DeepChainsRenderWithoutOverflow`) to 11.8 million (`ParsersAgreeOnSchemaAcceptance`).

Read `status.tsv`, not the script's exit code. The wrapper exited 0 on an earlier
attempt while one property had exited 134.

## What is left

Six entries remain in `kKnownDivergences`. The spec has the full ordering; in short:

| next | divergence | note |
| --- | --- | --- |
| A5 | container file cut inside a block-count varint | reproducer in hand, needs object-container coverage to measure against avro-cpp |
| C1 | trailing bytes after the schema JSON | first patch needing the `apache_avro::util` knob mechanism |
| C2 | vertical tab and form feed as JSON whitespace | needs a pre-pass that respects string literals and escapes |
| C3 | namespace with an empty component | **recommended last or not at all**: parity here makes the bridge's output less portable, since other implementations reject such a schema |
| D1 | duplicate full name panics at encode | the panic fix is not in question; whether parse-time rejection should be the default is |
| D2 | `duration` fixed re-renders unparseable by avro-cpp | flatten the render first, then the breaking `Schema::Duration` variant change that recovers name and namespace |
| E1/E2 | non-UTF-8 and duplicate map keys | needs `Value::Map` to stop being `HashMap<String, Value>`; deserves its own spec |

Not convergence targets, and the reasons are in the spec: the array and map block
framing family, where avro-cpp is the worse engine and matching it would mean
reproducing its fabrication; and the three avro-cpp bugs with no bridge-side fix (no
allocation ceiling, `GenericDatum` stack exhaustion on a name cycle, and the
declared-count `resize`).

## The decision that was needed before Tier C, and what was done

Tier C is the first group whose knob must live in `apache_avro::util` rather than in the
bridge, and that forced a question this report originally left open.

Tier A flipped crate defaults outright, which was defensible because upstream's
behaviour was *unsound*. Tier C is different: upstream's behaviour is **correct**. RFC
8259 permits exactly four whitespace bytes, and the Avro specification forbids an empty
namespace component. A patch making `Schema::parse_str` accept trailing garbage by
default would rightly be refused upstream, and would leave the crate
specification-noncompliant for anyone else using it.

So the knob defaults to upstream's strict behaviour and the bridge flips it, which needs
an initialisation point that runs before the first parse and still lets a caller who
wants strictness win the `OnceLock`. Three options were weighed, differing in how easy
it is to silently miss a site:

1. **Call it at the top of every affected bridge entry point.** `rust/schema.rs` alone
   has 29 `pub fn`. A missed one is a silent inconsistency.
2. **Hook it inside `catch_panic`**, which every parse, decode and container entry point
   already wraps. One chokepoint, but it couples panic containment to settings
   initialisation, and a future entry point not needing panic containment would silently
   skip it.
3. **Flip the crate default anyway**, as Tier A did, and drop the plumbing. Simplest,
   no site to miss, but it makes three patches unupstreamable and the crate
   specification-noncompliant by default.

**Option 2 is what shipped.** `install_avro_cpp_defaults()` in `rust/datum.rs` is called
from `catch_panic`, and `catch_panic`'s doc comment names the coupling so it is
discoverable. The set of entry points needing settings and the set needing panic
containment are the same set for a reason: both are the untrusted-input surface. It
installs `non_utf8_string_as_bytes` and `uuid_as_string`, the two settings whose crate
default is not avro-cpp's behaviour, and deliberately not the allocation ceiling, since
avro-cpp has no ceiling and parity there would mean removing a bound on untrusted input.

Two consequences worth knowing:

- **The `fuzz/` properties now run at parity on both settings.** `fuzz/differential_test.cc`
  never called either setter, so it ran at the crate defaults; `fuzz/README.md`'s claim
  that D1's read side was closed was therefore true of the patch and not of that binary.
  It is true of both now. D1's **write** side is unaffected: `CreateString` still rejects
  non-UTF-8 at `rust/value.rs:75`, which is what
  `Differential.D1NonUtf8StringIsRejectedByTheBridge` pins.
- **Per-instance settings were considered and rejected.** A constructor argument would
  let one process hold several instances with different settings, which is what the
  trailing-bytes knob got for free had it been the only one. The other three are read
  inside apache-avro, so per-instance there costs either a parameter threaded through
  the crate's recursion, breaking its public API and conflicting on every rebase, or
  thread-local ambient state with an install point at every entry point that can be
  silently missed. They stay process-global, and the second test binary is how both
  values of each get tested.

### The checkpoint run

Thirteen properties concurrently for five minutes in the fuzzing tree, with the
new defaults in force:

```
./fuzz/run_all_parallel.sh 5m ./fuzzrun/defaults 1200
```

**Thirteen of thirteen exit 0**, 19.0 million executions total. Read
`status.tsv`, not the wrapper's exit code. Nothing new surfaced from the default
flip, which is the result the flip needed: it changes what five of the thirteen
properties compare, since `fuzz/differential_test.cc` had never set either
setting.

## How a set-once setting is tested at both values

The knobs are first-call-wins, so one process observes one value of each and no test can
reset one. That used to mean a Rust integration test per value per knob:
`rust/tests/reject_trailing_bytes.rs` existed for exactly that, and its own header
recorded that a test wanting the value *off* would need a third binary.

`avro_bridge_test.cc` is now built twice instead. `avro_bridge_strict_test` is the same
source with `AVRO_BRIDGE_TEST_STRICT_SETTINGS` defined; a `::testing::Environment` flips
every setting the header exposes in `SetUp`, which is early enough because gtest runs it
before the first test body and nothing in the binary touches the bridge earlier. Its
return values are asserted, so a setting that failed to take fails the run rather than
quietly reporting the other value's behaviour.

A test whose outcome depends on a setting branches on `kStrictSettings` rather than being
duplicated, so the two binaries cannot drift apart. One extra binary covers every knob,
where the crate side needs one per knob per value.

Six tests pin the closures on the bridge's side, which is what the always-built suite was
missing: the closures were pinned only in `avro_bytes_fuzz_test.cc`, which needs avro-cpp
linked and only builds under `AVRO_BUILD_FUZZERS`. The differential half stays there. A
seventh, `DatumTest.SettingsMatchTheBuild`, asserts that the settings in force are the
ones the build asked for, so the strict binary losing the race fails rather than quietly
reporting the default's behaviour.

| test | closure |
| --- | --- |
| `DatumTest.TruncatedInputDoesNotFabricateValues` | A1, including the `Schema::String` site and `"null"` still decoding from zero bytes |
| `AvroSchemaTest.EmptyUnionIsRejected` | A2, with `["int"]` still legal |
| `AvroSchemaTest.EmptyEnumIsRejected` | A3, with a one-symbol enum still legal |
| `DatumTest.EmptyDecimalRoundTrips` | A4, the full circle back to the input byte |
| `DatumTest.TrailingBytesFollowTheSetting` | B1, both values, all three decode entry points |
| `DatumTest.NonUtf8StringFollowsTheSetting`, `DatumTest.UuidFollowsTheSetting` | the two pre-existing patches, both values |

The Rust suite drops from 70 tests across five binaries to 65 across four:
`rust/tests/reject_trailing_bytes.rs` is deleted, along with the two trailing-bytes unit
tests in `rust/datum.rs` and issue 1 of `rust/tests/security_properties.rs`. What stays
is what has no C++ counterpart: the allocation ceiling, which needs its own process for
the same reason, and the two documented residual limitations.
