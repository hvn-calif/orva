# Report: comparing decoded values in the byte-driven fuzzer

Written 2026-08-21, branch `avro-value-compare`, worktree
`/home/hvn/orva-difffuzz-vc`, based on `b3f73dc`.

## Glossary

| Term | Expansion | What it is here |
| --- | --- | --- |
| apache-avro | (a name) | the Rust crate the bridge binds, version 0.21, patched checkout at `/home/hvn/avro-rs-0.21-d1` revision `9786b7e`. |
| avro-cpp | Apache Avro C++ | `release-1.11.4`. The incumbent, and therefore the reference for behaviour. |
| `avro_bridge` | (a name) | the replacement: a Crubit binding over apache-avro. Called "the bridge" below. |
| D2 | (a name) | divergence register entry 2: duplicate keys in a map. |
| the dump | (a name) | the second value oracle: `DumpBridgeValue` / `DumpAvrocppDatum` in `fuzz/dump.cc`, one rendering function per engine, compared as text. |
| the walk | (a name) | the first value oracle: `CompareValues` in `fuzz/compare.cc`, which walks the two values in lockstep. |

## What the byte-driven fuzzer compared before

Three properties in `avro_bytes_fuzz_test.cc`. Two compared whether each engine
accepted an input. The third, `ReencodingAgreesWhenBothDecode`, was the only one
that looked at values, and it looked at them indirectly: each engine re-encoded
what it had decoded with its own encoder, and the two byte strings were
compared. That was the right trade when the file's premise was to include
nothing from `fuzz/`, and it cost three things.

- A mismatch could not be attributed. Two decoders disagreeing and two encoders
  disagreeing produce the same symptom.
- Every map schema was excluded. Avro fixes no order for map entries and the two
  engines choose differently, so the comparison would have failed intermittently
  with both engines correct.
- An input the two engines decoded differently but re-encoded identically passed
  as agreement.

The third of those is theoretical. The second is not: it is why D2 had never been
seen from raw bytes, which is the first thing the new property found.

## What is there now

`DecodedValuesAgree` compares the decoded values themselves. No encoder runs, so
a difference is attributable to the decoders.

It runs two oracles on the same decoded pair, because which one is better was
not decidable by reading them:

- **The walk** already existed for the tree harness. It knows about union
  transparency in avro-cpp's `GenericDatum`, array slots avro-cpp reserved and
  never read, decimal sign padding, uuid case folding, and it stops comparing
  once the two decoders have provably read different offsets. Its weakness is
  structural: the traversal is driven by avro-cpp's type switch and its payload
  reader falls back to an empty string, so a bridge value that its own accessor
  refuses can pass the walk.
- **The dump** is new. One rendering function per engine, compared as text, with
  `<unreadable>` printed where an accessor failed, so that case shows up as a
  mismatch. Its weakness is the mirror image: every formatting rule is a chance
  to manufacture noise or to hide a difference.

Running both on one input rather than as two properties is what makes the
comparison between them fair: neither gets a different corpus. Where exactly one
reports, the tag says which (`oracle-gap-walk-only`, `oracle-gap-dump-only`).

Code, by file:

| file | change |
| --- | --- |
| `fuzz/dump.{h,cc}` | new: the two rendering functions and their options |
| `fuzz/compare.{h,cc}` | one new option, `allow_string_as_bytes` |
| `fuzz/CMakeLists.txt` | `avro_fuzz_suppress` and `avro_fuzz_compare` split out so the byte fuzzer can link the oracles without the tree generator, and `avro_ir_fuzz_test` keeps linking neither engine |
| `CMakeLists.txt` | `add_subdirectory(fuzz)` moved above the byte fuzzer, which now links `avro_fuzz_compare` |
| `avro_bytes_fuzz_test.cc` | the property, three tests, the map seed list, the reach counters |

## The one normalisation rule the dump needed

Found in the property's first second. Under schema `"string"` with a payload
that is not valid UTF-8, the bridge returns bytes and avro-cpp returns a string,
with the same payload. That is deliberate: this binary enables
`SetNonUtf8StringAsBytes`, whose whole purpose is to make the bridge behave the
way avro-cpp does. The walk had been told to accept it; the dump had not, and
reported a tag difference on every such input.

The fix is a knob on the dump matching the one on the walk, set from one place in
the harness (`ValueCompareOptions` and `ValueDumpOptions` sit next to each
other). The rule that matters is not the code but the invariant: **both oracles
have to be told the same thing about the binary's configuration, or the
comparison between them measures the configuration rather than the oracles.**

## What it found: D2, from raw bytes

```
schema:  {"type":"map","values":"bytes"}
input:   04 02 62 02 04  02 62 02 9b  00
         ^^ two entries  ^^ "b"=0x04  ^^ "b"=0x9b  ^^ end of block
bridge:  map{62=text:9b}
avrocpp: map{62=text:04,62=text:9b}
```

avro-cpp's `GenericMap` is a vector of pairs and keeps both entries. The bridge
holds a Rust `HashMap`, so the second entry overwrites the first. Both engines
report success, and the bridge's caller is handed a map with one entry where the
wire had two, with nothing indicating that anything was dropped. Which entry
survives is pinned as well, because a caller that has to live with the collapse
needs to know: the last one on the wire wins.

`DuplicateMapKeyCollapsesInTheBridge` records it, and the table entry `D2` mutes
it so a long run continues.

**This corrects a claim in `AGENTS.md`.** That file said D2 lives on the write
path (`MapPut`) and that a byte-oriented harness therefore could not reach it.
Duplicate keys arrive on the wire just as easily. Nothing was reaching it
byte-side because the only value comparison there excluded every map schema.

## Array block framing, consolidated rather than renamed

The second thing the property reported was `ARRAY_LEN` under
`{"type":"array","items":"string"}`: the bridge decoded six items where avro-cpp
decoded four, from the same malformed block header.

That is the root cause this file already consolidates as `array-block-framing`,
and `ArrayBlockHeaderRecoveryDiverges` already holds the measured evidence for
it. Two of the walk's IDs are given that tag when the schema is an array,
`ARRAY_LEN` and `ARRAY_ITEM_FABRICATED`, and only those two: a scalar difference
inside an array item keeps its own tag, because that is a value difference
rather than framing.

## Measurements from the solo triage round

Ten minutes, one property on an otherwise idle machine, after D2 and the array
framing were tabled. This is the only run with reach counters, for the reason
recorded under "The run" below:

| measurement | value |
| --- | --- |
| inputs past the length guard | 13,251,263 |
| inputs where both engines decoded, so the values were compared | 7,439,833 |
| inputs the walk reported on | 691,022 |
| inputs the dump reported on | 691,022 |
| inputs where exactly one oracle reported | 0 |
| throughput | 24,400 runs per second |

Every one of those 691,022 reports was muted by the known-divergence table,
which is why the round exited 0.

A second ten-minute round after the run, in the final configuration -- two nested
schemas added, the dump demoted to evidence, findings under a nested collection
tagged by shape:

| measurement | value |
| --- | --- |
| inputs past the length guard | 10,016,018 |
| inputs where the values were compared | 5,508,275 |
| findings produced | 712,085 |
| findings not covered by the table | 0 |
| throughput | 18,700 runs per second |

The middle two lines are the ones that matter together. 55% of inputs reach the
comparison, and the property produced three quarters of a million findings that
the table recognised, so a green run means "everything it found is already
written down" rather than "it found nothing". Throughput is down from 24,400
because the two nested schemas are more expensive to decode than the flat ones
they joined.

The reach number is the one worth keeping: 56% of generated inputs got past both
decoders, so the property is comparing values rather than returning early. The
re-encode property's equivalent reach was much worse for some schemas -- the uuid
schema reached its comparison zero times over 3.5 million inputs -- so this was
not a given.

## What the runs found: two things, not one

Three symptoms came out of the first run, all from the tree harness's
`DecodersAgreeOnArbitraryBytes`: a map entry count at six minutes, a map key set
46 minutes later, an int field after a collection 40 minutes after that. They
looked like one class. Measuring how far each decoder read split them in two,
and the split is the point.

### The real one: same bytes read, different values

```
schema:  {"type":"map","values":{"type":"record","name":"N0","fields":[
            {"name":"f0","type":{"type":"array","items":"int"}}]}}
input:   02 00 02 00 01 00 02 02 00 00 00      eleven bytes
bridge:  map{=rec{f0=arr[int:0,int:1,int:0]}}
avrocpp: map{=rec{f0=arr[]},00=rec{f0=arr[]}}
read:    bridge 11 of 11, avro-cpp 11 of 11
```

Neither engine stopped early and neither ignored a tail, so this is not an
offset artefact: they read the same bytes and disagree about what they say. The
bridge's answer is what a byte-by-byte parse produces, and the parse is written
out in `NestedCollectionsDecodeToDifferentValuesFromTheSameBytes` accounting for
all eleven bytes. avro-cpp returns two entries holding empty arrays, under two
keys, one of which appears nowhere in the input as a key.

The combination is what does it: a positive block, then a negative one, then a
positive one. Seven variants with the pieces separated -- a single positive
block, a single negative block, a negative block with a declared size of 2, two
positive blocks, an empty array -- all decode identically on both engines.

`[CONFIRMED]` both accept, both read all eleven bytes, the values differ.
`[CONFIRMED]` the hand parse accounts for every byte and matches the bridge.
`[INFERRED]` avro-cpp is the one misreading. Reading its decoder is the next
piece of work and has not been done.

Both harnesses find this independently. The byte harness reaches it from a fixed
schema and raw bytes; the tree harness reached it through its own generator on
`{"type":"map","values":{"type":"array","items":"long"}}`, bridge 1 entry against
avro-cpp 2. It surfaces as an entry count or as a key set depending on the
input, which is why two IDs are tabled for one class.

### The other one: not attributable to either engine

```
schema:  record of map of record of array of array of null
input:   04 02 01 04 00 00 03 00 00 00 00 02 00 00 00   fifteen bytes
read:    bridge 15 of 15, avro-cpp 11 of 15
```

Here the decoders stop in different places, both entitled to ignore what is left.
Everything after byte eleven is read from different offsets, so comparing it says
nothing about either engine. `DecodedValuesAgree` reports this as
`consumption-differs`, once, instead of as whichever field happened to surface
it. `NestedCollectionsCanLeaveTheDecodersAtDifferentOffsets` pins it.

### How "how far did it read" is measured

Neither engine will say. avro-cpp's `InputStream::byteCount` counts bytes handed
to the decoder, which for a memory stream is the whole buffer at once, and the
bridge's strict trailing-bytes mode is a set-once process global a harness
cannot toggle per call. So it is measured from the outside, identically for
both: **the shortest prefix that reproduces the value the whole buffer
produced**, found by binary search in about six decodes.

The first version of this asked for the shortest prefix that merely *decodes*,
and that is wrong. avro-cpp accepts truncated input, because it reserves a
collection to its declared count and leaves the slots it never read, so it
decodes prefixes far shorter than what it consumes and two engines can report
the same number while reading different amounts. Comparing the value instead of
the verdict fixes it, and it is the one place the second oracle's rendering does
real work rather than decorating a message.

What the measurement does and does not establish, stated exactly because the
difference is the whole point:

- **Different lengths** prove the two ended at different offsets. Nothing they
  returned after that is attributable to either engine.
- **Equal lengths** do not prove they read the same bytes the same way. Both can
  consume the whole buffer and still disagree about what it says. That is a real
  divergence and is reported as one.

## The runs

Two, both with fourteen properties under `fuzz/run_all_parallel.sh`, against
worktree `b3f73dc` plus this branch's changes and apache-avro `9786b7e`.

| | first run | second run |
| --- | --- | --- |
| length | 7h14m, stopped on request | 3h, ran to its deadline |
| inputs, all properties | 1,263,820,000 | 510,470,000 |
| properties exiting 0 | 13 of 14 | 13 of 14 |
| peak resident, all jobs | 2.05 GB | 2.08 GB |
| lowest MemAvailable | 1.94 GB | 1.79 GB |

The second run is the one to read, because it had the consumption measurement
and the first did not. Its numbers for the value comparison:

| measurement | value |
| --- | --- |
| inputs past the length guard | 66,304,123 |
| inputs where the values were compared | 34,636,907 |
| findings produced | 6,381,793 |
| findings not covered by the table | 0 |

52% of inputs reached the comparison and it produced six million findings, every
one of them already written down. That is what a green run is supposed to mean,
and the first run could not say it: `run_all_parallel.sh` did not export
`AVRO_BYTES_FUZZ_STATS` then, so its byte harness printed no counters at all.
It does now.

### What the measurement bought, in one number

`Differential.DecodersAgreeOnArbitraryBytes` is the property the first run had to
abandon after 1h45m, because one class kept resurfacing under new IDs. With the
consumption measurement in front of the value comparison it ran **30 minutes**
before reporting instead of five, and what it reported was the real class rather
than an offset artefact. Muted, relaunched, it then ran the remaining **2h27m
and 15.6 million inputs with no findings at all**.

## The oracle comparison, decided

Two oracles were written and both ran on every compared input for one run, with
the case where exactly one reported given its own tag.

| | the walk | the dump |
| --- | --- | --- |
| unique findings | 0 | 0 |
| normalisation rules needed to go quiet | 0 (it already had them) | 1, the string-versus-bytes tag |
| names the differing node | yes | no, it prints two whole values |
| lines of code | 0 new, it existed | about 200 |

Over 7.4 million compared inputs in the solo round, neither ever reported
without the other, and over 271 million inputs in the long run no oracle-gap tag
fired. By the rule fixed before the measurement, the walk is the oracle. The
dump was not deleted: it renders the evidence in every failure message, which is
what makes the findings above readable as `map{=rec{f0=arr[]},00=rec{f0=arr[]}}`
rather than as two hex blobs. It is now rendered only on the reporting path, so
the hot loop does not pay for it.

The hypothesis that motivated writing it -- that the walk can pass a bridge value
its own accessor refuses, because the traversal is driven by avro-cpp's types
and the payload reader falls back to an empty string -- was not confirmed and was
not refuted either. No input in this run had that shape. The weakness is still
real in the code; it just did not matter here.

## Limits

- Object container files and schema resolution are still not compared at all.
- The value comparison only runs when both engines accept the input, so it
  depends on the acceptance properties to catch verdict disagreements.
- Both oracles are configured for this binary's two enabled patches. With
  `string_as_bytes` on, a real string-versus-bytes divergence is invisible to
  both. That is the configuration's choice, not a property of either oracle.
- The dump renders logical types as the payload underneath them, so an
  annotation difference is the walk's to catch.
- Equal consumption is not proof of equal reading, only of equal total. Two
  engines can each read every byte and still parse them differently, which is
  exactly the open class above, so a value difference with equal consumption is
  reported and has to be triaged by hand.
- The consumption measurement costs about a dozen extra decodes and runs only
  once the values already disagree. On an input where they agree it is never
  paid, and on one where they disagree it is what makes the report worth
  reading.
- `AvroBytes.ArrayBlockHeaderRecoveryDiverges` aborts any fuzzing-mode build at
  `avro/GenericDatum.hh:577`, "reference binding to null pointer", because
  UndefinedBehaviorSanitizer is enabled per target there and not in the
  unit-test build the project runs tests in. This predates the work here: the
  untouched build from the other worktree does the same. Worth its own
  investigation, since it is undefined behaviour in the incumbent library
  reached from decoded data.
