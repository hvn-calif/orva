# Spec: Avro Streaming Container IO (push-parser reader)

Status: APPROVED (GO 2026-07-22), implemented

## Purpose & user problem

Streaming is a hard requirement for the Avro binding, and the current
`container.rs` does not deliver it:

- The buffered `DataFileWriter` does not wrap `apache_avro::Writer` at all.
  It buffers `Value`s in a `Vec` and re-encodes the whole file in
  `to_bytes()`; peak memory is the entire dataset.
- The buffered `DataFileReader` decodes the entire file eagerly at
  construction.
- `StreamingDataFileWriter` (commit 0367189) is correct: it wraps
  `apache_avro::Writer` via `self_cell` and streams encoded bytes out through
  `take_bytes()`/`finish()`.
- `StreamingDataFileReader` is only half-streaming: it decodes values lazily
  but still requires the complete file bytes up front
  (`Cursor<Vec<u8>>`), so input cannot arrive incrementally and peak memory
  is still the whole file.

Goal: a reader that accepts input incrementally (C++ pushes chunks as they
arrive from disk, sockets, or any transport) and decodes values as soon as a
container block is complete, with bounded memory. Plus a consolidation of the
writer/reader API so the canonical `DataFileWriter`/`DataFileReader` are the
streaming implementations.

## Design decision (agreed 2026-07-22)

Push-parser reader. `apache_avro::Reader` is strictly pull-based over
`R: Read` and its block machinery (`Block`) is private, so it cannot be
wrapped in a push API without threads (rejected: thread-per-reader lifecycle
and deadlock/TSAN surface) or full input buffering (the current defect).
Instead, the binding owns only the Object Container File *framing*, which is
a small, frozen part of the Avro 1.x spec, and delegates every decode step to
apache-avro's public API:

- `Schema::parse_str` - writer schema from the header
- `Codec::decompress` - block decompression (public since 0.x)
- `from_avro_datum_schemata` - datum decode with named-ref support and
  optional reader-schema resolution (no borrowed-schema wrapper needed; the
  schema is passed per call, so `self_cell` disappears from the read side)

Rejected alternatives, for the record: thread-wrapped `apache_avro::Reader`
(safe but adds a decode thread per reader; this repo already carries a TSAN
deadlock PoC from pixel_bridge and we will not add thread surface to an FFI
crate); C++ callback-based `Read` (Crubit function-pointer bridging is
unproven in this repo, and Rust-calls-C++ re-entrancy breaks the
`catch_panic` model); file-owned pull reader (only fixes the on-disk case).

## Final API surface

The four current container types collapse to two. The buffered
`DataFileWriter`/`DataFileReader` and the `Streaming*` names are removed
(pre-release binding; no compatibility shim).

### DataFileWriter (per-block temporary Writer; replaces both current writers)

Same public surface as the current streaming writer, but without `self_cell`:
the struct holds only owned data (`Schema`, codec, one random 16-byte sync
marker, `pending: Vec<Value>`, `out: Vec<u8>`, `header_written: bool`). A
short-lived `apache_avro::Writer` is constructed inside `flush_block()` via
`Writer::builder()` with `.marker(self.marker)` (same sync marker for every
block) and `.has_header(self.header_written)` (header emitted only by the
first flush; the builder documents this exact use). The schema borrow starts
and ends inside that one method, so no self-referential type exists and the
`self_cell` dependency is removed from the crate. Upstream still produces
every wire byte (header, metadata map, block framing, compression, sync
markers).

- `create(schema, codec)` - rejects non-self-contained schemas; generates
  the file's sync marker (via the existing `uuid` dependency)
- `schema()`
- `append(value)` - validate early, buffer; auto-flushes a block every
  N values (default 1024)
- `take_bytes()` - drain `out` (header + completed blocks); never forces a
  partial block
- `finish()` - flush pending values as the final block, return remaining
  bytes, consume writer (an empty file still gets its header)
- `is_finished()`

Known trade-offs (accepted): blocks are sized by value count rather than
encoded bytes; one block of `Value`s is buffered between flushes; the
temporary `Writer` rebuilds its `ResolvedSchema` once per block.

C++ bridge keeps `WriteToPath`-style convenience by draining `take_bytes`
into a `std::ofstream` chunk by chunk (true streaming to disk, file IO owned
by C++).

### DataFileReader (new push parser, Rust + C++)

Incremental state machine: `ParsingHeader -> ReadingBlocks -> Done | Failed`.

Constructors:

- `new()` - writer schema comes from the stream header
- `with_reader_schema(schema)` - additionally resolve every value to the
  reader schema (schema evolution)
- Conveniences that feed everything at once and close:
  `from_bytes(data)`, `from_bytes_with_schema(schema, data)`,
  `from_path(path)`, `from_path_with_schema(schema, path)`

Streaming methods:

- `feed(data) -> Status` - append bytes to the internal buffer; errors after
  `close_input` or after a fatal error
- `close_input() -> Status` - declare end of input; a partially-received
  block becomes a decode error instead of waiting forever
- `next_ready() -> Result<bool, VecU8>` - drive the state machine as far as
  the buffered bytes allow; `Ok(true)` iff `next_value` would return a value
  now; `Ok(false)` means more input is needed or clean EOF (disambiguate with
  `at_end`)
- `next_value() -> Result<AvroValue, VecU8>` - next decoded value; errors if
  none is ready
- `next_value_into(&mut value) -> Result<bool, VecU8>` - overwrite one
  caller-owned value, reusing compatible allocations; true means one value
  was decoded and false is clean EOF. The C++ `NextValueInto(AvroValue*)`
  mirrors avro-cpp's `read(datum)`.
- `at_end() -> bool` - clean end: header seen, input closed, all values
  drained, no error
- `header_ready() -> bool`, `writer_schema() -> Result<AvroSchema, VecU8>`
  (error until the header has been parsed)

Error semantics: any framing/decode error is fatal and fuses the reader
(mirrors the current `pending_error`/`exhausted` behavior; a torn stream
never silently truncates).

### Framing owned by the binding (and only this)

1. Magic `Obj\x01` (4 bytes).
2. Header metadata map (avro-encoded `map<bytes>`): parsed by a hand-written
   incremental parser (zigzag varints, length-prefixed keys/values, negative
   map-block counts per the spec) so "need more bytes" is detected
   deterministically by length checks, never by sniffing `UnexpectedEof`
   error strings. Extracts `avro.schema` (via `Schema::parse_str`) and
   `avro.codec` (matched against the binding's codec set: null, deflate,
   snappy, zstandard; anything else, including bzip2/xz, is a fatal error,
   preserving avrocpp parity).
3. 16-byte sync marker.
4. Per block: object count (long varint), compressed size (long varint),
   payload, 16-byte sync marker which must equal the header's. Payload is
   decompressed with `Codec::decompress`, then `count` datums are decoded
   lazily (one per call). `next_value` uses `from_avro_datum_schemata` with
   the writer schema (passed in `schemata` so recursive/named-type references
   inside a self-contained schema resolve) and the optional reader schema.
   `next_value_into` uses a compiled identity plan from `crate::decode` to
   overwrite compatible record, array, union, string, bytes, fixed and enum
   storage; reader-schema resolution retains the ordinary allocating path.

## Security / untrusted input

- Caps enforced at parse time, before buffering or allocation:
  - header size cap (default 16 MiB) covering magic + metadata map + marker
  - per-block compressed-size cap (default 128 MiB), settable via
    `set_max_block_size(u64)` on the reader before the first `feed`
  - negative or absurd varints (>10 bytes, or values above the caps) are
    fatal errors
- The internal buffer is drained as blocks complete; peak memory is one
  compressed block + its decompressed form + one decoded value.
- Decompression output size remains unbounded by apache-avro (pre-existing
  caveat for deflate/snappy/zstd; unchanged, still documented in the README
  "Untrusted input" section). `set_max_allocation_bytes` still bounds
  apache-avro's internal decode allocations.
- All entry points that touch untrusted bytes stay behind `catch_panic`.
- `#![forbid(unsafe_code)]` is preserved; the `self_cell` dependency is
  removed entirely (neither writer nor reader holds a borrowing upstream
  object across calls).

## Success criteria

1. `cargo test` passes; new tests cover (at minimum):
   - byte-at-a-time feeding of a multi-block file (every split point:
     mid-magic, mid-varint, mid-key, mid-sync, mid-block)
   - round-trips for all four codecs, streaming writer -> push reader and
     apache-avro `Writer` -> push reader and push-written bytes ->
     apache-avro `Reader` (wire-format interop both directions)
   - schema resolution (`with_reader_schema`), recursive/named-ref schema
   - garbage magic, wrong sync marker, unknown codec (bzip2), truncated
     stream + `close_input` -> error then fused, feed-after-close,
     `next_value` with nothing ready, empty file (header only)
   - caps: oversized declared block size fails fast without buffering
   - golden pinned-bytes check retained
2. `rust/tests/streaming_memory.rs` updated: the large-file test feeds the
   reader in chunks and asserts the reader never holds the whole file.
3. C++ bridge + `avro_bridge_test.cc` updated to the two-type surface,
   including a chunked-feed C++ test; `main.cc` example uses the streaming
   API. `cmake --build` succeeds.
4. `qa-bug-finder` review of the final diff (focus: varint edge cases,
   integer overflow in caps/lengths, state-machine stuck/livelock cases,
   boundary UTF-8/span issues) with findings addressed.

## Out of scope

- Async IO, threads, C++ callbacks into Rust
- Seeking/`rewind`/`count` on the reader (gone with the buffered reader)
- Exposing user metadata from the header (can be added later; the parser
  skips unknown keys like apache-avro does)
- Bounding decompressed block size (upstream apache-avro limitation; tracked
  in README)
- Writer-side hand framing (encoding datums and emitting header/block bytes
  ourselves): the per-block temporary `Writer` keeps all wire bytes upstream;
  revisit only if per-block `Writer` construction ever shows up in profiles
