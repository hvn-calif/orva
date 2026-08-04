# Spec: Reusable single-datum reader (`AvroDatumReader`)

Status: APPROVED (GO 2026-08-03), implemented

Outcome: all five success criteria met. `Decode` on `flat` came in at 1.07x of
avrocpp against the 1.2x target, `DecodeInto` on `nested` at 1.94x faster
against a "no slower" target, and the steady-state allocation count is 0.
Measurements in `benchmarks/datum_decode_results.txt`. The three open questions
were resolved as the spec proposed: writer-schema only, new benchmark rows
added beside the existing one rather than replacing it, and the
`AvroDatumReader` name.

## Purpose & user problem

`DecodeDatum` is slower than avro-cpp on every dataset in the comparison
matrix. Measured 2026-08-03 (medians, 7 repetitions, pinned core, CV < 1.1%;
raw output in `benchmarks/datum_decode_results.txt`):

| dataset | records | ours | avrocpp | ratio | per-datum gap |
|---|---:|---:|---:|---:|---:|
| flat | 100k small records | 146.3 ns | 70.8 ns | 2.07x slower | +75.5 ns |
| nested | 10k, 8-item array + 4-key map | 1062 ns | 904.8 ns | 1.17x slower | +157.2 ns |
| strings | 10k, 1 KiB string | 160.0 ns | 91.2 ns | 1.76x slower | +68.8 ns |

The gap is near-constant at about 70 ns on `flat` and `strings`, two datasets
whose per-record work differs by roughly 2x. That is the signature of a fixed
per-call cost rather than slow decoding. Two causes, in order of expected
size:

1. **Schema resolution is rebuilt for every datum.** `decode_datum`
   (`rust/datum.rs:47`) calls `apache_avro::from_avro_datum`, which constructs
   a `GenericDatumReader` per call, and that constructor runs
   `ResolvedSchema::try_from(writer_schema)` (`reader/datum.rs:66`), walking
   the schema tree and building a `HashMap` of named types. avro-cpp builds
   its decoder once and reuses it.
2. **No allocation reuse.** Every call builds a fresh `Value` tree. The
   container path already solved this with `NextValueInto`; the datum path has
   no equivalent. This is where `nested` loses its extra 157 ns, most likely in
   the map arm, which `decode_into_internal` does not reuse.

Callers decoding a stream of same-schema datums outside container framing pay
both costs on every datum. That is the normal way this API is used.

## Design decision

Add a reader handle that owns the writer schema, resolves it once at
construction, and exposes both an owning and an in-place decode. This mirrors
how avro-cpp is used (build the decoder once, then decode many), and it
matches the existing `DataFileReader` precedent in this binding.

The Rust side wraps `apache_avro::reader::datum::OwnedGenericDatumReader`,
which the `read-into` patch already provides. **No new upstream change is
required.**

Rejected alternatives:

- **Cache inside `AvroSchema`** (a `OnceLock<OwnedGenericDatumReader>` field).
  Makes existing `DecodeDatum` callers faster with no code change, but it adds
  interior mutability to a small value type that is cloned and shared across
  the Crubit boundary, and it forces a decision about whether `Clone` copies
  or resets the cache. Rejected as too much hidden state on a shared type.
- **Process-global cache keyed by schema.** Requires hashing or fingerprinting
  the schema per call, which reintroduces a per-call cost, and it holds
  resolution state for schemas the caller has finished with.

## Final API surface

```cpp
// Decodes many datums that share one writer schema, resolving the schema's
// named types once instead of per datum. Prefer this to the DecodeDatum free
// function when decoding more than one datum with the same schema.
class AvroDatumReader final {
 public:
  static absl::StatusOr<AvroDatumReader> Create(const AvroSchema& writer_schema);

  // Returns the decoded value. Fails with kInvalidArgument on a malformed
  // datum or on trailing bytes.
  absl::StatusOr<AvroValue> Decode(absl::string_view data);

  // Decodes into caller-owned storage, reusing compatible allocations, and
  // mirrors DataFileReader::NextValueInto. Consume or inspect `value` before
  // the next call. A null pointer is rejected.
  absl::Status DecodeInto(absl::string_view data, AvroValue* value);

  AvroSchema WriterSchema() const;
};
```

The existing `DecodeDatum`, `DecodeDatumResolved`, and `DecodeDatumSchemata`
free functions are unchanged and stay as the one-shot path.

Rust side (`rust/datum.rs`), following the `DataFileReader` conventions:

- Holds `OwnedGenericDatumReader`, built from a single `Schema::clone`.
- `Default` yields a fused moved-out state that errors on every call, for
  Crubit move semantics (`rust/container.rs:384`).
- Every entry point wraps `catch_panic` and returns `VecU8` errors.
- Both `decode` and `decode_into` keep the existing trailing-bytes check
  (`TRAILING_BYTES_ERROR`); a correctly framed datum buffer is fully consumed,
  and leftovers signal a framing error worth surfacing.

## Security / untrusted input

- `set_max_allocation_bytes` still bounds length-prefix allocations. Reuse does
  not bypass it: `decode_into_internal` resizes through the same guarded paths.
- Reuse keeps a value tree alive between calls, so the reader's memory
  high-water mark is the largest datum seen, not the current one. A caller
  feeding one huge datum then many small ones holds the large capacity until it
  drops the `AvroValue`. Document this on `DecodeInto`.
- A decode error can leave `*value` partially overwritten. `DecodeInto` must
  document that the value is unspecified but valid after an error, matching
  `NextValueInto`.

## Success criteria

Measured with the same command as the baseline above.

1. `AvroDatumReader::Decode` on `flat` lands within 1.2x of avrocpp, against
   2.07x today. This is the test of whether the ~70 ns is in fact schema
   resolution.
2. `AvroDatumReader::DecodeInto` on `nested` is at least as fast as avrocpp.
   The container analogue reached 3.46x faster on decode-only work.
3. A second same-shaped datum through `DecodeInto` performs 0 allocator
   operations, verified the way `rust/tests/manifest_alloc.rs` already does it.
4. Existing `ours/datum_decode/*` rows do not regress; the free functions are
   untouched.
5. Full C++ and Rust test suites pass, including new coverage for trailing
   bytes, decoding a differently shaped datum into a reused value, a null
   pointer, and the moved-out state.

## Out of scope

- The encode side. No `EncodeDatumInto`; `to_avro_datum` still clones the
  value, which is a separate problem.
- Reader-schema resolution (`DecodeDatumResolved`). Resolution consumes a
  `Value` and can reshape it, so it cannot reuse storage, and
  `OwnedGenericDatumReader` does not support it.
- External `writer_schemata` (`DecodeDatumSchemata`). `OwnedGenericDatumReader`
  resolves names within one self-contained schema only. Supporting external
  schemata needs an upstream addition.
- The container path, which already has `NextValueInto`.
- Byte-level projection, covered by `doc/specs/AvroTokenStream.md`.

## Open questions for review

1. **Scope of `Create`.** v1 covers the writer-schema-only case, which is what
   `OwnedGenericDatumReader` supports. Do you need the schemata or
   reader-schema variants now? Either would need an upstream change first.
2. **Benchmark rows.** Should `ours/datum_decode` switch to the handle, making
   it apples-to-apples with avrocpp (which builds its decoder once), or should
   the current row stay and new `ours_reader/*` and `ours_reader_into/*` rows
   be added alongside? Switching changes the meaning of an existing row.
3. **Naming.** `AvroDatumReader` reads consistently with `DataFileReader`, but
   avro-cpp calls the equivalent `GenericReader`. Preference?
