# Spec: Avro Safe Binding (avrocpp replacement)

Status: DRAFT - awaiting approval

## Purpose & user problem

avrocpp is a large C++ codebase doing untrusted-input parsing (schemas, binary
datums, object container files) in a memory-unsafe language. The goal is a
memory-safe replacement: C++ callers keep a familiar avrocpp-shaped API, but
all parsing/encoding/decoding happens in Rust via the
[apache-avro](https://github.com/apache/avro-rs) crate (v0.21.0,
[docs.rs/apache-avro](https://docs.rs/apache-avro/latest/apache_avro/)),
exposed to C++ through Crubit-generated headers, following the existing
`safe-bindings` pattern (closest analogue: `serde_json/`).

## Architecture (3 layers, same as serde_json binding)

```
C++ app
  v  (absl::StatusOr API, avrocpp-like)
avro/avro_bridge.{h,cc}        hand-written C++ bridge
  v  (Crubit-generated rust.h: rs_std::Result, spans, bridged structs)
avro/rust/                     Crubit-friendly "rust" crate (lib.rs + modules)
  v  (plain Rust)
apache-avro 0.21               the actual implementation
```

Build: CMake + Corrosion fork + `corrosion_experimental_crubit(rust)`,
identical to the other bindings. Added to the CI matrix in
`.github/workflows/ci.yml`.

Layout:

```
safe-bindings/avro/
  CMakeLists.txt
  README.md
  avro_bridge.h / avro_bridge.cc      C++ bridge (absl::StatusOr API)
  avro_bridge_test.cc                 C++ round-trip tests (GoogleTest)
  main.cc                             example binary
  rust/
    Cargo.toml                        crate name "rust" (Crubit requirement)
    lib.rs
    crubit_vec_util.rs                make_vec_type! macro (copied pattern)
    raw_string.rs                     RawString = VecU8 (Crubit: no String yet)
    schema.rs                         AvroSchema
    value.rs                          AvroValue (generic datum)
    datum.rs                          single-datum encode/decode
    container.rs                      OCF reader/writer
    tests in-module (#[cfg(test)])    cargo test
```

## API surface (comprehensive)

All fallible Rust functions return `Result<T, RawString>`, which Crubit
bridges to `rs_std::Result` and the C++ layer converts to
`absl::StatusOr<T>` / `absl::Status`. Strings cross the boundary as
`&[u8]` / `RawString` (Crubit does not bridge `String` yet); C++ side uses
`absl::string_view` / `std::string`.

### 1. AvroSchema (schema.rs / C++ `AvroSchema`)

Wraps `apache_avro::Schema`.

- `parse(json)` - from `Schema::parse_str`
- `parse_list(jsons)` - cross-referencing schemata from `Schema::parse_list`;
  returns `VecAvroSchema`
- `canonical_form()` - Parsing Canonical Form string
- `fingerprint_rabin() -> u64`, `fingerprint_md5() / fingerprint_sha256()`
  - hex strings, from `schema.fingerprint::<Rabin|Md5|Sha256>()`
- `name()` / `namespace()` / `full_name()` for named schemas
- `to_json_string()` - serialized schema JSON
- type predicates: `is_record() / is_enum() / is_array() / is_map() /
  is_union() / is_fixed()` plus primitive checks
- `can_read(writer_schema)` - schema evolution compatibility from
  `SchemaCompatibility::can_read` (both `can_read` directions exposed)
- equality

### 2. AvroValue (value.rs / C++ `AvroValue`) - replaces avrocpp GenericDatum

Wraps `apache_avro::types::Value`. Tree API mirroring the SerdeJson binding
style.

Constructors (static): `create_null, create_boolean, create_int(i32),
create_long(i64), create_float(f32), create_double(f64),
create_bytes(&[u8]), create_string(&[u8]), create_record(),
create_array(), create_map(), create_enum(position, symbol),
create_fixed(&[u8]), create_union(branch_index, value)`.

Logical types: `create_decimal(bytes), create_uuid(string), create_date(i32),
create_time_millis(i32), create_time_micros(i64), create_timestamp_millis(i64),
create_timestamp_micros(i64), create_timestamp_nanos(i64),
create_local_timestamp_{millis,micros,nanos}(i64),
create_duration(months, days, millis)`.

Accessors: `get_boolean, get_int, get_long, get_float, get_double, get_bytes,
get_string, get_enum_position, get_enum_symbol, get_union_branch,
get_union_value, get_fixed_bytes`, logical-type getters mirroring the
constructors, `get_array_len / get_array_item(i)`, `get_map_keys /
get_map_value(key)`, `get_record_field_names / get_record_field(name)`.

Mutators: `record_put(name, value)`, `array_push(value)`,
`map_put(key, value)`.

Type predicates: `is_null, is_boolean, is_int, ...` (one per variant; a
`type_name()` helper returns the variant name for diagnostics).

Validation/resolution: `validate(schema) -> bool`,
`resolve(schema) -> Result<AvroValue>` (schema resolution of a value).

JSON: `to_json_string()` via `serde_json::Value::try_from(value)` - covers
the avrocpp JSON-encoder use case of dumping datums as JSON (full Avro JSON
wire-format parity is out of scope, see below).

### 3. Single-datum encode/decode (datum.rs / C++ free functions)

Replaces avrocpp binary `Encoder`/`Decoder` on raw buffers.

- `encode_datum(schema, value) -> VecU8` - `to_avro_datum`
- `decode_datum(writer_schema, data) -> AvroValue` - `from_avro_datum`
- `decode_datum_resolved(writer_schema, reader_schema, data)` - schema
  evolution on single datums
- schemata variants: `encode_datum_schemata`, `decode_datum_schemata` for
  cross-referencing schemas
- Safety valve: `set_max_allocation_bytes(n)` exposed so C++ can bound
  decoder allocations on untrusted input (`apache_avro::max_allocation_bytes`).

### 4. Object Container Files (container.rs / C++ `DataFileWriter`, `DataFileReader`)

Replaces avrocpp `DataFileWriter<GenericDatum>` / `DataFileReader<GenericDatum>`.
Buffer-oriented (like `BufferedZipArchive` in the zip binding) plus
path-based convenience:

Writer:
- `DataFileWriter::create(schema, codec)` - codec enum: `NULL_CODEC, DEFLATE,
  SNAPPY, ZSTANDARD, BZIP2, XZ` (all features enabled in Cargo.toml)
- `append(value)` (validates against schema), `flush()`,
  `into_bytes() -> VecU8`, `write_to_path(path)`

Reader:
- `DataFileReader::from_bytes(data)` - writer schema auto-detected
- `DataFileReader::from_bytes_with_schema(data, reader_schema)` - resolution
- `DataFileReader::from_path(path)` (+ `_with_schema`)
- `writer_schema()`, `has_next()`, `next() -> AvroValue` (iterator surface
  flattened; Crubit cannot bridge Rust iterators)

### 5. C++ bridge niceties

- `operator==` on AvroSchema/AvroValue
- All string parameters `absl::string_view`, all returned strings
  `std::string`
- Error mapping: parse failures -> `absl::InvalidArgumentError`; wrong-type
  access -> `absl::FailedPreconditionError`; not-found ->
  `absl::NotFoundError`

## Success criteria

1. `cmake -B build && cmake --build build` succeeds in `safe-bindings/avro/`
   (same toolchain pin: `nightly-2026-05-19`).
2. `cargo test` in `avro/rust/` passes; tests cover every exposed function
   (happy path + error path) including round-trips for all primitive,
   complex, and logical types, all codecs, schema resolution, and
   fingerprints checked against known Avro spec values.
3. C++ GoogleTest binary (`avro_bridge_test`) passes: schema parse,
   record round-trip through single-datum and OCF paths, codec round-trips,
   schema-evolution read, error statuses for malformed input.
4. `main.cc` example demonstrates an avrocpp-style workflow (parse schema,
   build record, write OCF, read it back).
5. `unsafe` count in our Rust crate: 0 (`#![forbid(unsafe_code)]` in lib.rs).
   C++ bridge contains only the same `reinterpret_cast<const uint8_t*>`
   string-view/span casts the existing bindings use.
6. QA agent review of the full diff with findings addressed before handoff.

## Technical considerations / constraints

- Crubit limitations (observed in existing bindings): no `String` in
  signatures (use `RawString`/`&[u8]`), no generic functions, no `()` in
  `Result` (use `Result<u8, RawString>` Status pattern), no trait objects or
  iterators across the boundary, no borrowed returns with lifetimes (values
  are cloned out, as SerdeJson does).
- `apache-avro` `Reader`/`Writer` are generic over `Read`/`Write`; the
  binding crate monomorphizes them over `Cursor<Vec<u8>>`/`Vec<u8>`
  internally (same approach as the zip binding).
- Cargo features: `bzip`, `snappy`, `xz`, `zstandard` enabled so the codec
  enum is complete.
- Clone cost: accessors clone subtrees out of values (Crubit cannot return
  references). Acceptable for a v1 safety-focused binding; documented in the
  README.
- The `rust` crate is never meant to be used from Rust directly (same
  WARNING header as the other bindings).

## Out of scope

- Code generation (avrogencpp equivalent: .avsc -> C++ structs)
- Avro RPC / protocol (.avpr) support
- Full Avro JSON wire-format encoder/decoder parity (only Value -> JSON
  dump provided)
- Streaming / incremental IO (buffer- and path-oriented only)
- Trevni, IDL (.avdl)
- Drop-in header compatibility with avrocpp (callers migrate to the new
  bridge API; naming is close but not identical)

## Test & verification plan

1. Rust: `#[cfg(test)]` unit tests per module, run via `cargo test`
   (toolchain-pinned). Round-trip property: for each type, build value,
   encode, decode, compare.
2. Golden-data check: decode a hard-coded OCF byte vector (generated once
   with avro tooling) to guard against silent wire-format drift.
3. C++: GoogleTest target in CMake exercising the bridge end to end,
   including error paths (malformed schema, truncated datum, wrong-type
   access, bad codec data).
4. QA pass: spawn a `qa-bug-finder` agent on the final diff (focus:
   boundary bugs - UTF-8 handling, span lifetimes, moved-from Rust objects,
   integer width mismatches, error-path coverage).
