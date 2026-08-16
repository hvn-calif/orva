# Findings from the first differential fuzzing run

Baseline: worktree at `84ce4af`, avro-cpp `release-1.11.4`, empty suppression
file, unit-test mode (about one second per property, single core).

Each finding below was reached **cold from an empty corpus**, with no seed
directing the fuzzer toward it.

## Acceptance test: the two known bugs were rediscovered

The point of pinning this worktree at `84ce4af` is that two divergences closed
twelve commits later by `efb7162` are still open here, so the harness has a
known answer key.

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

### 1. `CanonicalForm()` is not canonical for a logical type on a primitive

**The most consequential of the three.**

```
schema:         {"type":"int","logicalType":"time-millis"}
CanonicalForm:  {"type":"int"}          <- should be "int"
```

Avro's Parsing Canonical Form has a PRIMITIVES rule: a primitive is written in
its simple form, `"int"`, never `{"type":"int"}`. Three consequences follow:

- the canonical form violates the spec;
- it is **not idempotent** -- reparsing `{"type":"int"}` and taking its
  canonical form gives `"int"`;
- the Rabin fingerprint is wrong. The annotated schema fingerprints as
  `8145260995063234477`; a plain int is `8247732601305521295`, which is the
  value in the Avro spec's own test data and the one `rust/schema.rs` already
  asserts.

Since PCF deliberately strips `logicalType`, these two schemas must fingerprint
identically. They do not.

This is not cosmetic: fingerprints are how schema registries establish schema
identity, so a wrong one is a missed cache hit or a failed lookup. Any schema
using `date`, `time-millis`, `timestamp-millis` and friends on a primitive is
affected, which is most real schemas.

Pinned by `Differential.CanonicalFormIsNotCanonicalForLogicalTypes`.

### 2. avro-cpp accepts a malformed namespace

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

### 3. The bridge rejects uuid text avro-cpp keeps verbatim

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

## Not yet covered

The harness currently exercises single-datum encode and decode plus schema
parsing and rendering. Still to build: object-container round-trips across the
null, deflate and snappy codecs; reader-versus-writer schema resolution, which
the register also lists as entirely uninvestigated and which the plan sketches
as an `EvolvePlan` edit applied to the writer tree; and the deep-nesting
termination property.

Byte-level comparison is deliberately out of scope -- random container sync
markers and Rust `HashMap` iteration order (D3) make encoded bytes vary between
runs, so only decoded values are compared.
