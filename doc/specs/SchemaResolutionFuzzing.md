# Spec: Differential fuzzing of schema resolution

Status: DRAFT - awaiting approval

## Purpose & user problem

`avro_bytes_fuzz_test.cc` compares `avro_bridge` against Apache avro-cpp on
three things: whether both parsers accept a schema, whether both decoders accept
some bytes, and whether both re-encode a decoded value to the same bytes. All
three read data with the schema it was written with.

Schema resolution, reading data written with one schema through a different
reader schema, is not covered by that file, and is not covered anywhere else
either. Every property in `fuzz/differential_test.cc` is single-schema as well:
`DatumCircleAgrees`, `SchemaVerdictsAgree`, `SchemasCrossParse` and
`DecodersAgreeOnArbitraryBytes`. The word "resolve" appears in that file once, in
a comment about name collisions.

Two pieces of public bridge API therefore have no differential coverage at all:

- `AvroSchema::CanReadFrom(writer)` and `AvroSchema::MutualRead(other)`, which
  answer whether one schema can read data written with another.
- `DecodeDatumResolved(writer_schema, reader_schema, data)`.

This matters more than the gap's size suggests. Resolution is where the Avro
specification has the most rules, and where two implementations written
independently have the most room to disagree:

- numeric promotion (`int` to `long` to `float` to `double`) and `string` to
  `bytes`
- record fields matched by name, with the reader's declared default filling in
  a field the writer did not have
- an enum symbol the writer used and the reader does not declare
- union branch resolution, including a writer union read into a reader non-union
  and the reverse
- aliases, and fields appearing in a different order in the two schemas

The last two categories are the reason to prioritise this. A resolution bug does
not usually produce an error; it produces a **different value**, silently. That
is the class of bug the round-trip property was added to catch, and resolution is
where it is most likely to occur. avro-cpp's own header carries a
`// FIXME: Handle out of order fields.` on `resolvingDecoder`, so at least one
divergence is likely already known to upstream.

## Success criteria

1. Two new properties in `avro_bytes_fuzz_test.cc`, described under **Scope**.
2. Every divergence found is either fixed or recorded, following the file's
   existing convention: an entry in `kKnownDivergences`, a test named after it
   that documents the measured behaviour, and the count in
   `KnownDivergenceTableSizeIsPinned` updated.
3. Divergences are recorded **by root cause, not by error message**. See
   **Recording divergences** below; this is the single most important
   convention in this spec and the one most recently learned the hard way.
4. The reach of each property is measured, not assumed, and reported. A property
   that never gets past its guard clauses passes forever while comparing
   nothing. See **Measuring reach**.
5. `ctest` and a fuzzing-mode run of the three existing properties stay as green
   as they are today. No existing entry is removed unless the behaviour it
   records has actually changed, in which case say so.

## Scope

### Property 1: resolution verdicts agree

```cpp
void ResolutionVerdictsAgree(const std::string& writer_text,
                             const std::string& reader_text);
```

Parse both schemas with both engines. When all four parses succeed, compare:

- bridge: `reader_schema.CanReadFrom(writer_schema)` returns an `absl::Status`.
- avro-cpp: `::avro::resolvingDecoder(writer, reader, ::avro::binaryDecoder())`
  throws if the pair cannot resolve. Wrap it in the file's existing `Guarded`
  helper, which converts a throw into an `absl::Status`.

Report a divergence when one says the pair resolves and the other does not. Use
a tag of `resolution-verdict`.

Start here. It needs no byte domain, it is roughly forty lines, and it tests API
that has no coverage today. Do not begin property 2 until the verdicts are
understood, because property 2's results are hard to interpret while the two
engines disagree about which pairs are even resolvable.

### Property 2: resolved decode agrees

```cpp
void ResolvedDecodeAgrees(const std::string& writer_text,
                          const std::string& reader_text,
                          const std::string& bytes);
```

When both engines agree the pair resolves and both decode the bytes, compare
what came out. Use the same indirect comparison the file already uses: have each
engine re-encode what it decoded and compare the two byte strings.

Re-encode under the **reader** schema, not the writer schema. That is the whole
point of resolution: the value that comes back conforms to the reader schema.

- bridge: `DecodeDatumResolved(writer, reader, bytes)`, then `EncodeDatum(reader_schema, value)`.
- avro-cpp: build the decoder as above, `decoder->init(*in)`, construct the
  `::avro::GenericDatum` from the **reader** `ValidSchema`, then `::avro::decode(*decoder, datum)`.
  Re-encode with the existing `EncodeWithAvrocpp` helper against the reader
  schema.

Constraints carried over from the existing properties, both required:

- Call `DeclaresLengthBeyondLimit(bytes, kMaxDeclaredLengthBytes)` and return
  early when it is true. Rust aborts the process when an allocation fails, and
  no sanitizer setting can intercept that, so oversized declared lengths must be
  kept away from the decoder rather than caught afterwards.
- Exclude maps, as `RoundTripSchemas` already does. Avro fixes no order for map
  entries and the two engines choose differently, so a byte comparison of a
  re-encoded map fails intermittently with both engines correct. Reuse
  `MentionsMap`, and verify the exclusion through the parser rather than through
  that same predicate, as `MapsAreExcludedBecauseReencodedEntryOrderIsUnstable`
  does.

### The schema-pair domain

This is the part most likely to be got wrong, and the part that decides whether
the properties find anything.

A cross product of the existing `DecodableSchemas` list is the obvious choice and
the wrong one. Most unrelated pairs do not resolve, so both engines reject, and
the run spends its time confirming that two different schemas are different.

Build a domain of **related** pairs instead: a writer schema, and a reader schema
derived from it by one deliberate evolution. At minimum:

| Evolution | Example |
| --- | --- |
| identical | any schema with itself, the case that must always resolve |
| numeric promotion | `int` writer, `long` reader; `float` writer, `double` reader |
| field added with a default | reader has a field the writer does not, with `"default"` |
| field removed | writer has a field the reader does not |
| field reordered | same fields, different order, which is avro-cpp's FIXME |
| enum symbol added | reader declares a symbol the writer does not use |
| enum symbol removed | writer uses a symbol the reader does not declare |
| union widened | `"int"` writer, `["null","int"]` reader |
| union narrowed | `["null","int"]` writer, `"int"` reader |
| aliases | reader renames a field and declares the old name in `"aliases"` |
| unrelated | a small number of pairs that should not resolve, as a control |

Write these as an explicit list of pairs, the way `ValidEncodings` is an explicit
list. Do not generate them, since a generated pair whose expected verdict nobody
knows is not evidence of anything.

For property 2, seed the corpus. Feeding arbitrary bytes to a resolving decoder
and hoping is what makes a property look busy while comparing almost nothing. The
existing `ValidEncodings` gives a valid encoding per schema and is used both as
assertions and as `.WithSeeds(...)`; do the same here, and keep the seed list and
the deterministic test driven off one definition so a seed cannot rot into an
invalid one without a named test failing.

## Recording divergences

Follow `kKnownDivergences`, but read the last two commits on this branch first
(`git log --oneline -3 -- avro_bytes_fuzz_test.cc`) because they exist to correct
a mistake worth not repeating.

The table matches a finding by tag plus a substring of the detail message. The
first instinct is to add one entry per error message. That does not converge: a
malformed array block header produced four different messages, and a schema with
trailing bytes produced three, all for one underlying behaviour, and each fuzzing
sweep produced another spelling of something already understood.

So: **when several findings share a cause, classify them in the harness and give
them one tag and one entry.** Two examples already in the file to copy:

- `IsArraySchema` assigns `array-block-framing` by schema shape, replacing four
  message-specific entries.
- `ParsesAsSchemaPrefix` assigns `schema-trailing-bytes` when a proper prefix of
  the schema text parses on its own, replacing two and absorbing a third message
  found later.

Both are deliberately broader than the entries they replaced, and both say so in
a comment. The consolidated test is what holds the line: it pins the measured
counts, values and contrasts, so a change in known behaviour still fails even
though a new spelling of the same cause will not.

Expect resolution to need this. Numeric promotion, union resolution and default
filling will each produce several messages for one cause.

Every entry needs a test named after it, and that test must state what was
measured, including anything not understood. `AllocationCeilingChecksCountAgainstAByteLimit`
is the model: it records a count that is not explained rather than leaving it out.

`AVRO_BYTES_FUZZ_SKIP` mutes a tag for one run during triage. It is not a way to
make a run green.

## Measuring reach

Before claiming a property works, measure how often it gets past its guards to
the comparison, and report the number.

The method used previously: add temporary atomic counters for total calls, each
early-return path, and comparisons reached; run in fuzzing mode for two minutes;
print at exit; remove the counters. That measurement is what showed the
round-trip property reaching its comparison on 386,094 of 3,561,756 inputs, and,
broken down per schema, that one schema reached it **zero** times because random
bytes never spell a valid value for it. Seeding fixed that and the next divergence
appeared within four seconds.

Do the same per schema pair. A pair that never resolves contributes nothing, and
you want to know which ones those are before drawing conclusions from a clean run.

## Technical considerations

### Building

Two configured build directories exist:

- `/opt/dfz-b1` - ordinary build. `cmake --build /opt/dfz-b1 --target avro_bytes_fuzz_test`,
  then run `/opt/dfz-b1/avro_bytes_fuzz_test`. Each `FUZZ_TEST` runs about a
  second against random inputs.
- `/opt/dfz-fuzz` - configured with `FUZZTEST_FUZZING_MODE=ON`. Required for
  `--fuzz=AvroBytes.<name> --fuzz_for=<duration>`. The ordinary build rejects
  those flags with "To fuzz, please build with --config=fuzztest".

**The branch does not compile without a local dependency redirect.** It calls
`apache_avro::util::set_non_utf8_string_as_bytes` and
`apache_avro::util::set_uuid_as_string`, which exist only with the two patches in
orva's `patches/` applied to a local apache-avro 0.21 checkout. The redirect
lives in an untracked `.cargo/config.toml` at the worktree root, because
`patches/README.md` rules out committing a machine-local path. If that file is
missing, recreate it:

```toml
[patch.crates-io]
apache-avro = { path = "/path/to/patched/avro-rs/avro" }
```

A `[patch]` section in `rust/Cargo.toml` is silently ineffective here. Crubit
invokes cargo twice and the second pass uses a synthesized root manifest that
drops it, so the first pass builds against the patched checkout and the second,
which is the one that compiles the crate, builds against the registry copy. The
symptom is `cannot find function ... in module apache_avro::util` while a plain
`cargo check` on the same source succeeds. The redirect has to be in a cargo
config.

`rust/Cargo.lock` is rewritten by every build while the redirect is active. Do
not commit that change; the committed lock points at the crates.io release, which
is how `main` handles its own patched dependency.

### File conventions

- **Self-contained.** `avro_bytes_fuzz_test.cc` must not include anything from
  `fuzz/`. That is deliberate and is why the value comparison is done by
  re-encoding rather than by reusing the comparator in `fuzz/compare.cc`. Do not
  "fix" the duplication by linking the two.
- avro-cpp signals errors by throwing, the bridge returns `absl::Status`. The
  `Guarded` helper converts one to the other at the boundary. Use it for every
  avro-cpp call.
- Google C++ style, 80 columns. Load the `cpp-google-style` skill before writing.
  Check with `awk 'length>80' avro_bytes_fuzz_test.cc` and format only the lines
  you changed: `clang-format -style=Google -i --lines=a:b`, since a whole-file
  reformat buries the change in unrelated churn.
- Prefer spelling a byte string out with `Varint(...)` over embedding hex, so a
  reader can see what it means. `FromHex` exists for a fuzzer counterexample kept
  verbatim, where the bytes carry no structure worth spelling out.

### A trap worth knowing

`"\xffb"` in a C++ string literal lexes as one out-of-range hex escape, not
`\xff` followed by `b`. Split the literal: `"\xff" "b"`.

## Out of scope

- Object container files. `DataFileWriter` and `DataFileReader` are not touched.
- `DecodeDatumSchemata` and `EncodeDatumSchemata`, the multi-schema forms.
- Fixing any divergence found. Record them; closing them is separate work, and
  for apache-avro it means a patch in orva's `patches/` following the two already
  there.
- The structure-aware harness in `fuzz/`. If a divergence found here is easier to
  express there, say so rather than reaching across.
- Changing the three existing properties, beyond what is needed to share the
  schema-pair or seed definitions.

## Open questions for the reviewer

1. Should `MutualRead` be covered as well as `CanReadFrom`, or is the one-way
   verdict enough for a first pass?
2. Property 2 compares re-encoded bytes, which cannot say whether a mismatch came
   from the decoders or the encoders. That was an accepted trade for the existing
   round-trip property. Does it stay accepted here, where the reader schema adds
   a third thing that could be wrong?
3. Aliases are in the domain table above. They are the most likely place for the
   two engines to differ and also the most likely to need a patch rather than an
   entry. Include them in the first pass or defer?
