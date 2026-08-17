# Bug: `CanonicalForm()` is not canonical for a logical type on a primitive

## Glossary

| Term | Expansion | What it is here |
| --- | --- | --- |
| `avro_bridge` | (a name) | the Crubit/corrosion binding over the Rust `apache-avro` crate. Called "the bridge" below. |
| avro-cpp | Apache Avro C++ | the implementation the bridge replaces. Relevant here only because it has no counterpart to this API. |
| PCF | Parsing Canonical Form | the Avro spec's normalized schema text, the input to schema fingerprinting. |
| Rabin fingerprint | (a name) | the 64-bit CRC-64-AVRO hash the Avro spec defines over the Parsing Canonical Form; schema registries use it as schema identity. |

## Scope: this is not a migration divergence

avro-cpp 1.11.4 has **no canonical-form and no fingerprint API at all**.
`lang/c++` contains no occurrence of `CanonicalForm`, `fingerprint`, or
`Rabin`; `ValidSchema` exposes only `toJson()` and `compactSchema()`.
`SchemaNormalization` exists in the Java and C# bindings only.

So no avro-cpp caller can be relying on this today, there is nothing to compare
the bridge against, and this is **out of scope for the differential fuzzer**.
The check that found it has been removed from `fuzz/differential_test.cc` for
that reason.

It is still a bug, because the bridge ships the surface as public API
(`avro_bridge.h:50-56`: `CanonicalForm`, `FingerprintRabin`,
`FingerprintRabinHex`, `FingerprintMd5Hex`, `FingerprintSha256Hex`) and the
project has already committed to spec conformance for it:
`rust/schema.rs:305` `rabin_fingerprints_match_apache_test_data` pins `"int"`
to the Avro spec's own test-data value. This bug contradicts that test's
premise.

## The bug

A logical type layered on a primitive produces a canonical form that keeps the
`{"type":...}` wrapper instead of collapsing to the primitive's simple form.

```
schema:         {"type":"int","logicalType":"time-millis"}
CanonicalForm:  {"type":"int"}
spec requires:  "int"
```

Avro's Parsing Canonical Form has a PRIMITIVES rule: a primitive is written in
its simple form. PCF also strips `logicalType` outright, so an annotated
primitive and its plain counterpart must produce identical canonical forms and
therefore identical fingerprints.

## Measured

Taken from `avro_bridge` at `84ce4af`, apache-avro 0.21.0:

| schema | `CanonicalForm()` | `FingerprintRabin()` |
| --- | --- | --- |
| `{"type":"int","logicalType":"time-millis"}` | `{"type":"int"}` | 8145260995063234477 |
| `"int"` | `"int"` | 8247732601305521295 |
| `{"type":"long","logicalType":"timestamp-millis"}` | `{"type":"long"}` | 7635232846682232745 |
| `"long"` | `"long"` | -3434872931120570953 |

Three consequences, each measured rather than inferred:

1. **The canonical form violates the spec.** `{"type":"int"}` where the
   PRIMITIVES rule requires `"int"`.
2. **It is not idempotent.** Reparsing `{"type":"int"}` and taking *its*
   canonical form yields `"int"`, so the function does not reach a fixed point
   in one pass.
3. **The fingerprint is wrong.** 8247732601305521295 is the value in the Avro
   spec's own test data for an int schema, and the one `rust/schema.rs:309`
   already asserts. Two schemas whose canonical forms must be identical
   fingerprint differently.

## Why it matters

Fingerprints are how schema registries establish schema identity, so a wrong
one is a missed cache hit or a failed lookup against any registry populated by
a spec-conforming writer. It affects every schema using `date`, `time-millis`,
`timestamp-millis`, `time-micros`, `timestamp-micros` and similar directly on a
primitive, which is most real schemas.

How much is at risk depends on adoption: this API has no avro-cpp counterpart,
so nothing being migrated can already depend on it. The risk is forward-looking,
against new callers.

## Reproducer

```cpp
#include "avro_bridge.h"

using ::security::avro::AvroSchema;

auto annotated =
    AvroSchema::Parse(R"({"type":"int","logicalType":"time-millis"})");
auto plain = AvroSchema::Parse(R"("int")");

// Observed at 84ce4af. Per the spec every one of these should hold the
// opposite way round.
CHECK_EQ(annotated->CanonicalForm(), R"({"type":"int"})");  // want: "int"
CHECK_EQ(plain->CanonicalForm(), R"("int")");               // correct already

auto reparsed = AvroSchema::Parse(annotated->CanonicalForm());
CHECK_NE(annotated->CanonicalForm(), reparsed->CanonicalForm());  // want: equal

CHECK_EQ(plain->FingerprintRabin(), 8247732601305521295LL);       // spec value
CHECK_NE(annotated->FingerprintRabin(), plain->FingerprintRabin());  // want: equal
```

## Where the fix belongs

`rust/schema.rs:56` `canonical_form` delegates straight to apache-avro's
`Schema::canonical_form()`, so the defect is upstream in apache-avro 0.21.0
rather than in the binding. Two options, not mutually exclusive: report it to
apache-avro, and/or normalize in `rust/schema.rs` before delegating. Whichever
is chosen, the fix should come with a test asserting the two fingerprints in
the table above are equal.
