//! Object Container File (OCF) reading and writing. Replaces avrocpp's
//! `DataFileWriter<GenericDatum>` / `DataFileReader<GenericDatum>`.
//!
//! Both types stream and never require a whole file in memory:
//!
//! - `DataFileWriter` buffers at most one batch of values and emits encoded
//!   bytes incrementally through `take_bytes`/`finish`. Each batch is written
//!   by a short-lived `apache_avro::Writer` created inside `flush_block`, so
//!   the schema borrow in `Writer<'a, W>` never outlives a single method call
//!   and no self-referential type is needed.
//! - `DataFileReader` is a push parser: callers `feed` bytes as they arrive
//!   (from disk, sockets, any transport) and drain decoded values with
//!   `next_ready`/`next_value`. The binding parses only the container
//!   *framing* (magic, header metadata map, block headers, sync markers),
//!   which is a small frozen part of the Avro 1.x spec; schema parsing,
//!   block decompression and datum decoding are delegated to apache-avro's
//!   public API (`Schema::parse_str`, `Codec::decompress`,
//!   `from_avro_datum_schemata`).
//!
//! See doc/specs/AvroStreamingIO.md for the design discussion.

use crate::schema::AvroSchema;
use crate::value::AvroValue;
use crate::vec_u8::{catch_panic, utf8, Status, VecU8};
use apache_avro::decode::decode_resolved;
use apache_avro::schema::{build_names, Names, ResolvedSchema};
use apache_avro::types::Value;
use apache_avro::{Codec, Schema, Writer};
use std::io::Cursor;

/// Compression codec for object container files.
/// Mirrors `apache_avro::Codec` with default settings for each algorithm.
///
/// The codec set deliberately matches avrocpp: null, deflate, snappy, and
/// zstandard. bzip2 and xz are intentionally excluded -- avrocpp does not
/// support them, and they are the most aggressive decompression-bomb
/// formats. None of the compressing codecs bound their decompressed size
/// (apache-avro provides no such cap), so reading compressed container
/// files from untrusted sources still requires an external memory limit;
/// see the README's "Untrusted input" section.
#[repr(C)]
#[derive(Default, Debug, Clone, Copy, PartialEq, Eq)]
pub enum AvroCodec {
    #[default]
    Null,
    Deflate,
    Snappy,
    Zstandard,
}

impl AvroCodec {
    /// Converts an integer discriminant (matching the C++-side enum) to a
    /// codec. This is the constructor to use from C++.
    pub fn from_i32(value: i32) -> Result<AvroCodec, VecU8> {
        match value {
            0 => Ok(AvroCodec::Null),
            1 => Ok(AvroCodec::Deflate),
            2 => Ok(AvroCodec::Snappy),
            3 => Ok(AvroCodec::Zstandard),
            _ => Err(format!("Invalid codec discriminant: {}", value).into()),
        }
    }

    /// Maps the `avro.codec` name from a container file header to a codec.
    /// Names outside the supported set (including bzip2 and xz) are errors.
    fn from_name(name: &[u8]) -> Result<AvroCodec, VecU8> {
        match name {
            b"null" => Ok(AvroCodec::Null),
            b"deflate" => Ok(AvroCodec::Deflate),
            b"snappy" => Ok(AvroCodec::Snappy),
            b"zstandard" => Ok(AvroCodec::Zstandard),
            other => Err(format!(
                "Unsupported container codec: {}",
                String::from_utf8_lossy(other)
            )
            .into()),
        }
    }
}

impl From<AvroCodec> for Codec {
    fn from(codec: AvroCodec) -> Self {
        match codec {
            AvroCodec::Null => Codec::Null,
            AvroCodec::Deflate => Codec::Deflate(Default::default()),
            AvroCodec::Snappy => Codec::Snappy,
            AvroCodec::Zstandard => Codec::Zstandard(Default::default()),
        }
    }
}

/// How many values `DataFileWriter` buffers before encoding them as blocks.
/// Bounds the writer's working set; within one batch apache-avro still cuts
/// blocks by encoded size (~16 KiB), so on-disk block sizes stay normal.
const VALUES_PER_BLOCK: usize = 1024;

/// Streaming Avro object container file writer.
///
/// `append` validates and buffers values; every `VALUES_PER_BLOCK` values the
/// batch is encoded into container blocks by a short-lived
/// `apache_avro::Writer` (see `flush_block`). `take_bytes` drains the encoded
/// bytes produced so far (header plus completed blocks) without forcing a
/// partial block. `finish` encodes any remaining values, returns the last
/// bytes, and consumes the writer. The concatenation of every `take_bytes`
/// result followed by the `finish` result is a complete container file.
#[derive(Debug)]
pub struct DataFileWriter {
    schema: Schema,
    codec: AvroCodec,
    /// One random sync marker for the whole file. Every per-batch
    /// `apache_avro::Writer` is created with this same marker so the blocks
    /// they emit concatenate into one valid file.
    marker: [u8; 16],
    /// Values buffered since the last block flush (at most VALUES_PER_BLOCK).
    pending: Vec<Value>,
    /// Encoded bytes not yet drained by `take_bytes`/`finish`.
    out: Vec<u8>,
    header_written: bool,
    finished: bool,
}

impl Default for DataFileWriter {
    fn default() -> Self {
        // The default/moved-from state behaves like an already-finished
        // writer: every fallible method errors. (The infallible `schema`
        // accessor returns the placeholder null schema.)
        DataFileWriter {
            schema: Schema::Null,
            codec: AvroCodec::Null,
            marker: [0; 16],
            pending: Vec::new(),
            out: Vec::new(),
            header_written: false,
            finished: true,
        }
    }
}

impl DataFileWriter {
    /// Creates a writer for the given self-contained schema and codec.
    ///
    /// Returns an error for schemas that reference named types defined in
    /// other schemas (as returned by `AvroSchema::parse_list`). Container
    /// files embed only the writer schema in their header, so a file
    /// written with an unresolved reference could never be read back;
    /// apache-avro's validation path even panics on such schemas, and the
    /// check here keeps that path unreachable from `append`. To write
    /// container files, inline the referenced types in a single schema
    /// document; for raw datums, `datum::encode_datum_schemata` supports
    /// cross-referencing schemas.
    pub fn create(schema: &AvroSchema, codec: AvroCodec) -> Result<DataFileWriter, VecU8> {
        if let Err(err) = ResolvedSchema::try_from(&schema.schema) {
            return Err(format!(
                "Schema is not self-contained and cannot be used for a container file \
                 (inline the referenced types in a single schema document): {}",
                err
            )
            .into());
        }
        Ok(DataFileWriter {
            schema: schema.schema.clone(),
            codec,
            marker: rand::random::<[u8; 16]>(),
            pending: Vec::new(),
            out: Vec::new(),
            header_written: false,
            finished: false,
        })
    }

    /// Returns the schema this writer was created with.
    pub fn schema(&self) -> AvroSchema {
        AvroSchema { schema: self.schema.clone() }
    }

    /// Validates `value` against the writer schema and buffers it. A
    /// rejected value leaves the writer usable; an encoding failure while
    /// flushing a full batch tears the stream and consumes the writer.
    pub fn append(&mut self, value: &AvroValue) -> Status {
        self.ensure_active()?;
        // Safe from panics: `create` proved that this schema resolves.
        let valid = catch_panic(|| Ok(value.value.validate(&self.schema)))?;
        if !valid {
            return Err(format!(
                "Value of type {} does not conform to the writer schema",
                String::from_utf8_lossy(value.type_name().as_slice())
            )
            .into());
        }
        self.pending.push(value.value.clone());
        if self.pending.len() < VALUES_PER_BLOCK {
            return Ok(0);
        }
        self.flush_block_or_tear()
    }

    /// Drains the encoded bytes produced so far (header plus completed
    /// blocks) without forcing a partial block. May return empty.
    pub fn take_bytes(&mut self) -> Result<VecU8, VecU8> {
        self.ensure_active()?;
        Ok(std::mem::take(&mut self.out).into())
    }

    /// Encodes any remaining buffered values, returns all undrained bytes,
    /// and consumes the writer. Subsequent calls to any method error. With
    /// no appended values the result is a valid header-only file.
    pub fn finish(&mut self) -> Result<VecU8, VecU8> {
        self.ensure_active()?;
        let result = catch_panic(|| {
            self.flush_block()?;
            Ok(std::mem::take(&mut self.out).into())
        });
        self.finished = true;
        result
    }

    /// Returns true once `finish` has consumed the writer (or a flush
    /// failure tore the stream).
    pub fn is_finished(&self) -> bool {
        self.finished
    }

    fn ensure_active(&self) -> Status {
        if self.finished {
            return Err("Writer already finished".into());
        }
        Ok(0)
    }

    /// Encodes the pending values (and the header, the first time) into
    /// `out` as container blocks.
    ///
    /// The `apache_avro::Writer` created here lives only for this call:
    /// its `&Schema` borrow starts and ends inside the method, so the
    /// surrounding struct holds no self-reference and stays freely movable
    /// (which Crubit requires). Reusing the file's sync marker and skipping
    /// the header via `has_header` makes the batches concatenate into one
    /// valid container file; upstream still produces every wire byte.
    fn flush_block(&mut self) -> Result<(), VecU8> {
        let nothing_to_do = self.pending.is_empty() && self.header_written;
        if nothing_to_do {
            return Ok(());
        }
        let mut writer = Writer::builder()
            .schema(&self.schema)
            .writer(std::mem::take(&mut self.out))
            .codec(self.codec.into())
            .marker(self.marker)
            .has_header(self.header_written)
            .build();
        for value in self.pending.drain(..) {
            writer.append_value_ref(&value).map_err(|err| VecU8::from(err.to_string()))?;
        }
        self.out = writer.into_inner().map_err(|err| VecU8::from(err.to_string()))?;
        self.header_written = true;
        Ok(())
    }

    /// `flush_block`, but a failure consumes the writer: bytes already
    /// handed to the temporary `Writer` are lost on error, so the stream is
    /// torn and continuing would silently produce a corrupt file.
    fn flush_block_or_tear(&mut self) -> Status {
        match catch_panic(|| {
            self.flush_block()?;
            Ok(0)
        }) {
            Ok(ok) => Ok(ok),
            Err(err) => {
                self.finished = true;
                Err(err)
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Push-parser reader
// ---------------------------------------------------------------------------

/// Cap on the container header (magic + metadata map + sync marker). A
/// hostile stream cannot make us buffer more than this while hunting for
/// the end of the header. 16 MiB comfortably fits any real schema.
const MAX_HEADER_BYTES: usize = 16 * 1024 * 1024;

/// Default cap on a block's declared compressed size; adjustable per reader
/// with `set_max_block_size`. Enforced before any buffering or allocation
/// for the block's payload is attempted.
const DEFAULT_MAX_BLOCK_BYTES: u64 = 128 * 1024 * 1024;

/// Parsed container file header.
#[derive(Debug)]
struct Header {
    schema: Schema,
    codec: AvroCodec,
    marker: [u8; 16],
    /// `schema`'s named types, resolved once when the header is parsed.
    /// apache-avro's public per-datum decode entry points rebuild this map
    /// on every call, which for small records costs more than decoding the
    /// record; `decode_one` passes this instead. Owned (not a borrow of
    /// `schema`) so the reader stays movable, which Crubit requires.
    names: Names,
}

/// Streaming Avro object container file reader (push parser).
///
/// Feed input bytes incrementally with `feed` (chunk boundaries are
/// arbitrary; mid-varint splits are fine), declare end of input with
/// `close_input`, and drain decoded values with `next_ready`/`next_value`.
/// The `from_bytes`/`from_path` constructors are conveniences that feed
/// everything at once and close.
///
/// Peak memory is one compressed block plus its decompressed form plus one
/// decoded value; the whole file is never required in memory, and total
/// parse time is linear in the input (progress is committed between calls,
/// so hostile chunking cannot force re-parsing). Any framing or decode
/// error is fatal and fuses the reader (every later call returns the same
/// error), so a torn stream can never silently truncate.
#[derive(Debug)]
pub struct DataFileReader {
    /// Bytes fed but not yet compacted away. The parsed prefix is tracked by
    /// `consumed` and removed with amortized O(1) cost per byte (`compact`),
    /// never per block.
    input: Vec<u8>,
    /// How many bytes of `input` the parser has consumed.
    consumed: usize,
    input_closed: bool,
    /// Incremental header parser; replaced by `header` once complete.
    header_parser: HeaderParser,
    header: Option<Header>,
    /// When set, every value is resolved to this schema (schema evolution).
    reader_schema: Option<Schema>,
    max_block_bytes: u64,
    /// Decompressed payload of the block currently being drained, with the
    /// decode position and the number of values left in it.
    block: Vec<u8>,
    block_pos: usize,
    block_remaining: u64,
    /// Fatal error; sticky. Every subsequent call returns a copy.
    error: Option<VecU8>,
}

impl Default for DataFileReader {
    fn default() -> Self {
        // The default state exists for Crubit's move semantics: C++ moves
        // replace the moved-from object with Default::default(). Fuse it so
        // a C++ use-after-move errors loudly instead of behaving like a
        // fresh, working reader.
        let mut reader = Self::create();
        reader.error = Some("Reader is in an invalid, moved-out state".into());
        reader
    }
}

impl DataFileReader {
    /// Creates an empty reader; the writer schema is taken from the stream
    /// header once enough bytes have been fed. (Named `create` rather than
    /// `new` because `new` is a reserved word in the generated C++.)
    pub fn create() -> DataFileReader {
        DataFileReader {
            input: Vec::new(),
            consumed: 0,
            input_closed: false,
            header_parser: HeaderParser::start(),
            header: None,
            reader_schema: None,
            max_block_bytes: DEFAULT_MAX_BLOCK_BYTES,
            block: Vec::new(),
            block_pos: 0,
            block_remaining: 0,
            error: None,
        }
    }

    /// Creates an empty reader that additionally resolves every value to
    /// `reader_schema` (schema evolution).
    pub fn with_reader_schema(reader_schema: &AvroSchema) -> DataFileReader {
        let mut reader = Self::create();
        reader.reader_schema = Some(reader_schema.schema.clone());
        reader
    }

    /// Opens a container file from a complete byte buffer. The writer schema
    /// is read from the header. Errors on garbage or a truncated first block.
    pub fn from_bytes(data: &[u8]) -> Result<DataFileReader, VecU8> {
        Self::build_from_bytes(None, data)
    }

    /// Opens a container file from a complete byte buffer and resolves every
    /// value to `reader_schema`.
    pub fn from_bytes_with_schema(
        reader_schema: &AvroSchema,
        data: &[u8],
    ) -> Result<DataFileReader, VecU8> {
        Self::build_from_bytes(Some(reader_schema), data)
    }

    /// Opens a container file from the filesystem.
    pub fn from_path(raw_path: &[u8]) -> Result<DataFileReader, VecU8> {
        let path = utf8(raw_path)?;
        let data = std::fs::read(path).map_err(|err| VecU8::from(err.to_string()))?;
        Self::from_bytes(&data)
    }

    /// Opens a container file from the filesystem, resolving every value to
    /// `reader_schema`.
    pub fn from_path_with_schema(
        reader_schema: &AvroSchema,
        raw_path: &[u8],
    ) -> Result<DataFileReader, VecU8> {
        let path = utf8(raw_path)?;
        let data = std::fs::read(path).map_err(|err| VecU8::from(err.to_string()))?;
        Self::from_bytes_with_schema(reader_schema, &data)
    }

    fn build_from_bytes(
        reader_schema: Option<&AvroSchema>,
        data: &[u8],
    ) -> Result<DataFileReader, VecU8> {
        let mut reader = match reader_schema {
            Some(schema) => Self::with_reader_schema(schema),
            None => Self::create(),
        };
        reader.feed(data)?;
        reader.close_input()?;
        // Parse eagerly up to the first value so garbage and files truncated
        // in the header or first block fail at construction.
        reader.next_ready()?;
        if reader.header.is_none() {
            return Err("Container file ended before a complete header".into());
        }
        Ok(reader)
    }

    /// Appends input bytes. Chunk boundaries are arbitrary. Errors once the
    /// input was closed or after a fatal error.
    pub fn feed(&mut self, data: &[u8]) -> Status {
        self.ensure_not_failed()?;
        if self.input_closed {
            return Err("Input already closed".into());
        }
        self.input.extend_from_slice(data);
        Ok(0)
    }

    /// Declares end of input. After this, data ending mid-header or
    /// mid-block surfaces as an error from `next_ready`/`next_value`
    /// instead of waiting for more bytes forever. Idempotent.
    pub fn close_input(&mut self) -> Status {
        self.ensure_not_failed()?;
        self.input_closed = true;
        Ok(0)
    }

    /// Drives the parser as far as the fed bytes allow. Returns true iff
    /// `next_value` would return a value right now; false means more input
    /// is needed or the file ended cleanly (disambiguate with `at_end`).
    pub fn next_ready(&mut self) -> Result<bool, VecU8> {
        self.ensure_not_failed()?;
        match catch_panic(|| {
            self.pump()?;
            Ok(0u8)
        }) {
            Ok(_) => Ok(self.block_remaining > 0),
            Err(err) => {
                self.error = Some(err.clone());
                Err(err)
            }
        }
    }

    /// Returns the next decoded value. Errors (benignly, without fusing) if
    /// no value is ready; decode failures are fatal.
    pub fn next_value(&mut self) -> Result<AvroValue, VecU8> {
        if !self.next_ready()? {
            if self.at_end() {
                return Err("No more values in the container file".into());
            }
            return Err("No value ready: feed more input or close_input".into());
        }
        match catch_panic(|| self.decode_one()) {
            Ok(value) => Ok(value),
            Err(err) => {
                self.error = Some(err.clone());
                Err(err)
            }
        }
    }

    /// True once the file ended cleanly: header parsed, input closed and
    /// fully consumed, all values drained, no error. Reflects parse progress
    /// as of the last `next_ready`/`next_value` call.
    pub fn at_end(&self) -> bool {
        self.error.is_none()
            && self.header.is_some()
            && self.input_closed
            && self.consumed == self.input.len()
            && self.block_remaining == 0
    }

    /// True once a fatal error has fused the reader (every call returns the
    /// same error). Lets the C++ bridge distinguish a fatally corrupt stream
    /// from the benign "no value ready yet" state.
    pub fn has_failed(&self) -> bool {
        self.error.is_some()
    }

    /// True once the header has been parsed and `writer_schema` is available.
    pub fn header_ready(&self) -> bool {
        self.header.is_some()
    }

    /// Returns the schema the file was written with. Errors until enough
    /// input has been fed and parsed (drive with `next_ready`).
    pub fn writer_schema(&self) -> Result<AvroSchema, VecU8> {
        match &self.header {
            Some(header) => Ok(AvroSchema { schema: header.schema.clone() }),
            None => Err("Header not parsed yet: feed more input and call next_ready".into()),
        }
    }

    /// Adjusts the cap on a block's declared compressed size (default
    /// 128 MiB). Applies to blocks parsed after the call.
    pub fn set_max_block_size(&mut self, bytes: u64) -> Status {
        self.ensure_not_failed()?;
        if bytes == 0 {
            return Err("Max block size must be positive".into());
        }
        // Clamp to addressable memory so the u64 -> usize conversion of a
        // declared block size below the cap can never truncate (relevant on
        // 32-bit targets).
        self.max_block_bytes = bytes.min(usize::MAX as u64);
        Ok(0)
    }

    fn ensure_not_failed(&self) -> Status {
        match &self.error {
            Some(err) => Err(err.clone()),
            None => Ok(0),
        }
    }

    /// Advances the parser as far as the buffered input allows: parses the
    /// header if still pending, then complete blocks until one with values
    /// is loaded. Returns without error when it simply needs more input.
    fn pump(&mut self) -> Result<(), VecU8> {
        // Phase 1: header. The parser commits progress between calls, so a
        // header arriving in many small chunks is parsed in linear total
        // time (nothing is re-walked on later calls).
        if self.header.is_none() {
            let (consumed, header) =
                self.header_parser.advance(&self.input[self.consumed..])?;
            self.consumed += consumed;
            self.compact();
            match header {
                Some(header) => self.header = Some(header),
                None => return self.need_more_input("header"),
            }
        }
        let (codec, expected_marker) = match &self.header {
            Some(header) => (header.codec, header.marker),
            None => return Err("Internal error: header missing after parse".into()),
        };

        loop {
            // Phase 2: drain the current block before parsing the next one.
            if self.block_remaining > 0 {
                return Ok(());
            }
            let block_has_trailing_junk = self.block_pos < self.block.len();
            if block_has_trailing_junk {
                return Err("Corrupt block: trailing bytes after the last value".into());
            }
            let unparsed = &self.input[self.consumed..];
            if unparsed.is_empty() {
                // Clean end of file (if closed) or waiting for more input.
                return Ok(());
            }

            // Phase 3: parse the next block: count, size, payload, marker.
            let mut pos = 0;
            let Some(count) = parse_long(unparsed, &mut pos)? else {
                return self.need_more_input("block header");
            };
            if count < 0 {
                return Err("Corrupt block: negative object count".into());
            }
            let Some(declared_size) = parse_long(unparsed, &mut pos)? else {
                return self.need_more_input("block header");
            };
            if declared_size < 0 {
                return Err("Corrupt block: negative block size".into());
            }
            if declared_size as u64 > self.max_block_bytes {
                return Err(format!(
                    "Block size {} exceeds the maximum of {} bytes",
                    declared_size, self.max_block_bytes
                )
                .into());
            }
            // Cannot truncate: set_max_block_size clamps the cap to
            // usize::MAX, and declared_size passed the cap check above.
            let Ok(size) = usize::try_from(declared_size) else {
                return Err("Block size exceeds addressable memory".into());
            };
            let block_end = pos + size;
            let after_marker = block_end + 16;
            if unparsed.len() < after_marker {
                return self.need_more_input("block payload");
            }
            if unparsed[block_end..after_marker] != expected_marker {
                return Err("Corrupt block: sync marker mismatch".into());
            }
            let mut payload = unparsed[pos..block_end].to_vec();
            self.consumed += after_marker;
            self.compact();
            Codec::from(codec)
                .decompress(&mut payload)
                .map_err(|err| VecU8::from(err.to_string()))?;
            if count == 0 {
                // An empty block is legal only if it is truly empty.
                if !payload.is_empty() {
                    return Err("Corrupt block: zero-count block with payload".into());
                }
                continue;
            }
            self.block = payload;
            self.block_pos = 0;
            self.block_remaining = count as u64;
            return Ok(());
        }
    }

    /// Removes the consumed prefix of the input buffer once it makes up at
    /// least half of it. Amortized O(1) per byte, so a file with many small
    /// blocks parses in linear time (a per-block drain would be quadratic).
    fn compact(&mut self) {
        if self.consumed > 0 && self.consumed * 2 >= self.input.len() {
            self.input.drain(..self.consumed);
            self.consumed = 0;
        }
    }

    /// Whether running out of buffered bytes is fine (more may be fed) or a
    /// truncation error (the input was closed).
    fn need_more_input(&self, while_parsing: &str) -> Result<(), VecU8> {
        if self.input_closed {
            return Err(format!(
                "Truncated container file: input ended inside a {}",
                while_parsing
            )
            .into());
        }
        Ok(())
    }

    /// Decodes one value from the current block. Only called with
    /// `block_remaining > 0`.
    fn decode_one(&mut self) -> Result<AvroValue, VecU8> {
        let Some(header) = &self.header else {
            return Err("Internal error: no header while decoding".into());
        };
        let mut cursor = Cursor::new(&self.block[self.block_pos..]);
        // `header.names` carries the writer schema's named types, resolved
        // once at header parse time, so named-type references inside a
        // self-contained (e.g. recursive) schema still resolve -- without
        // rebuilding the map for every value the way the upstream per-datum
        // entry points do.
        let value = decode_resolved(&header.schema, &header.names, &mut cursor)
            .map_err(|err| VecU8::from(err.to_string()))?;
        // Schema evolution, matching what from_avro_datum_schemata did with
        // a reader schema and no reader schemata.
        let value = match &self.reader_schema {
            Some(reader_schema) => {
                value.resolve(reader_schema).map_err(|err| VecU8::from(err.to_string()))?
            }
            None => value,
        };
        self.block_pos += cursor.position() as usize;
        self.block_remaining -= 1;
        Ok(AvroValue { value })
    }
}

// ---------------------------------------------------------------------------
// Incremental framing parsers. Each returns Ok(None) when the buffered input
// does not yet contain a complete unit ("need more bytes", detected purely by
// length checks, never by sniffing EOF errors), Ok(Some(..)) on success, and
// Err on definitely-corrupt input.
// ---------------------------------------------------------------------------

/// Parses one zigzag-encoded Avro long starting at `*pos`, advancing `*pos`
/// past it on success. Strict: at most 10 bytes, no overflow.
fn parse_long(buf: &[u8], pos: &mut usize) -> Result<Option<i64>, VecU8> {
    let mut value: u64 = 0;
    let mut shift: u32 = 0;
    let mut i = *pos;
    loop {
        let Some(&byte) = buf.get(i) else {
            return Ok(None);
        };
        i += 1;
        // The 10th byte (shift 63) may only carry the final bit.
        if shift == 63 && (byte & 0x7f) > 1 {
            return Err("Corrupt varint: overflows a 64-bit long".into());
        }
        value |= u64::from(byte & 0x7f) << shift;
        if byte & 0x80 == 0 {
            *pos = i;
            let decoded = ((value >> 1) as i64) ^ -((value & 1) as i64);
            return Ok(Some(decoded));
        }
        shift += 7;
        if shift > 63 {
            return Err("Corrupt varint: longer than 10 bytes".into());
        }
    }
}

/// Parses a length prefix for a header map key or value, enforcing the
/// header cap so absurd declared lengths fail fast.
fn parse_header_len(buf: &[u8], pos: &mut usize, what: &str) -> Result<Option<usize>, VecU8> {
    let Some(len) = parse_long(buf, pos)? else {
        return Ok(None);
    };
    if len < 0 {
        return Err(format!("Corrupt header: negative length for {}", what).into());
    }
    if len as u64 > MAX_HEADER_BYTES as u64 {
        return Err(
            format!("Corrupt header: {} length {} exceeds the header cap", what, len).into(),
        );
    }
    Ok(Some(len as usize))
}

/// The header metadata keys the binding cares about; everything else (user
/// metadata, avro.codec.compression_level) is skipped, matching
/// apache-avro's reader.
#[derive(Debug, Clone, Copy)]
enum MetaKey {
    Schema,
    Codec,
    Other,
}

/// Position inside the container header. The metadata is an Avro
/// map<bytes>: repeated groups of a pair count then that many key/value
/// pairs, terminated by a zero count; a negative count means |count| pairs
/// preceded by the group's byte size.
#[derive(Debug, Clone, Copy)]
enum HeaderState {
    Magic,
    GroupCount,
    GroupSize { pairs_left: u64 },
    KeyLen { pairs_left: u64 },
    KeyBytes { pairs_left: u64, len: usize },
    ValueLen { pairs_left: u64, key: MetaKey },
    ValueBytes { pairs_left: u64, key: MetaKey, len: usize },
    Marker,
}

/// Incremental parser for the container header (magic, metadata map, sync
/// marker). Progress is committed after every completed token, so a header
/// fed in many small chunks parses in linear total time: each `advance`
/// call re-examines at most the one incomplete token at the buffer front
/// (a varint of <= 10 bytes, or an O(1) length check for byte strings).
#[derive(Debug)]
struct HeaderParser {
    state: HeaderState,
    schema_json: Option<Vec<u8>>,
    codec_name: Vec<u8>,
    /// Header bytes consumed so far, enforced against MAX_HEADER_BYTES.
    bytes_consumed: usize,
}

impl HeaderParser {
    fn start() -> HeaderParser {
        HeaderParser {
            state: HeaderState::Magic,
            schema_json: None,
            codec_name: b"null".to_vec(),
            bytes_consumed: 0,
        }
    }

    /// Consumes as many complete header tokens from `input` as possible.
    /// Returns the number of bytes consumed (the caller must not re-feed
    /// them) and the finished header once the sync marker has been read.
    fn advance(&mut self, input: &[u8]) -> Result<(usize, Option<Header>), VecU8> {
        let mut pos = 0;
        loop {
            if self.bytes_consumed.saturating_add(pos) > MAX_HEADER_BYTES {
                return Err(format!(
                    "Container header exceeds the maximum of {} bytes",
                    MAX_HEADER_BYTES
                )
                .into());
            }
            match self.state {
                HeaderState::Magic => {
                    if input.len() - pos < 4 {
                        break;
                    }
                    if &input[pos..pos + 4] != b"Obj\x01" {
                        return Err("Not an Avro object container file (bad magic)".into());
                    }
                    pos += 4;
                    self.state = HeaderState::GroupCount;
                }
                HeaderState::GroupCount => {
                    let Some(count) = parse_long(input, &mut pos)? else {
                        break;
                    };
                    self.state = if count == 0 {
                        HeaderState::Marker
                    } else if count < 0 {
                        HeaderState::GroupSize { pairs_left: count.unsigned_abs() }
                    } else {
                        HeaderState::KeyLen { pairs_left: count as u64 }
                    };
                }
                HeaderState::GroupSize { pairs_left } => {
                    let Some(_group_byte_size) = parse_long(input, &mut pos)? else {
                        break;
                    };
                    self.state = HeaderState::KeyLen { pairs_left };
                }
                HeaderState::KeyLen { pairs_left } => {
                    let Some(len) = parse_header_len(input, &mut pos, "metadata key")? else {
                        break;
                    };
                    self.state = HeaderState::KeyBytes { pairs_left, len };
                }
                HeaderState::KeyBytes { pairs_left, len } => {
                    if input.len() - pos < len {
                        break;
                    }
                    let key = match &input[pos..pos + len] {
                        b"avro.schema" => MetaKey::Schema,
                        b"avro.codec" => MetaKey::Codec,
                        _ => MetaKey::Other,
                    };
                    pos += len;
                    self.state = HeaderState::ValueLen { pairs_left, key };
                }
                HeaderState::ValueLen { pairs_left, key } => {
                    let Some(len) = parse_header_len(input, &mut pos, "metadata value")? else {
                        break;
                    };
                    self.state = HeaderState::ValueBytes { pairs_left, key, len };
                }
                HeaderState::ValueBytes { pairs_left, key, len } => {
                    if input.len() - pos < len {
                        break;
                    }
                    let value = &input[pos..pos + len];
                    match key {
                        MetaKey::Schema => self.schema_json = Some(value.to_vec()),
                        MetaKey::Codec => self.codec_name = value.to_vec(),
                        MetaKey::Other => {}
                    }
                    pos += len;
                    let pairs_left = pairs_left - 1;
                    self.state = if pairs_left == 0 {
                        HeaderState::GroupCount
                    } else {
                        HeaderState::KeyLen { pairs_left }
                    };
                }
                HeaderState::Marker => {
                    if input.len() - pos < 16 {
                        break;
                    }
                    let mut marker = [0u8; 16];
                    marker.copy_from_slice(&input[pos..pos + 16]);
                    pos += 16;
                    self.bytes_consumed = self.bytes_consumed.saturating_add(pos);
                    return Ok((pos, Some(self.build(marker)?)));
                }
            }
        }
        self.bytes_consumed = self.bytes_consumed.saturating_add(pos);
        Ok((pos, None))
    }

    fn build(&self, marker: [u8; 16]) -> Result<Header, VecU8> {
        let Some(schema_json) = &self.schema_json else {
            return Err("Corrupt header: missing avro.schema metadata".into());
        };
        let schema =
            Schema::parse_str(utf8(schema_json)?).map_err(|err| VecU8::from(err.to_string()))?;
        let codec = AvroCodec::from_name(&self.codec_name)?;
        let names = build_names(&schema).map_err(|err| VecU8::from(err.to_string()))?;
        Ok(Header { schema, codec, marker, names })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use apache_avro::{to_avro_datum, Reader as UpstreamReader, Writer as UpstreamWriter};

    const RECORD_SCHEMA: &str = r#"{
        "type": "record",
        "name": "Measurement",
        "fields": [
            {"name": "sensor", "type": "string"},
            {"name": "value", "type": "double"}
        ]
    }"#;

    fn measurement(sensor: &str, value: f64) -> AvroValue {
        let mut record = AvroValue::create_record();
        record
            .record_put(b"sensor", &AvroValue::create_string(sensor.as_bytes()).unwrap())
            .unwrap();
        record.record_put(b"value", &AvroValue::create_double(value)).unwrap();
        record
    }

    /// Writes `values` through the writer, draining after every append, and
    /// returns the complete container file bytes.
    fn write_file(schema: &AvroSchema, codec: AvroCodec, values: &[AvroValue]) -> Vec<u8> {
        let mut writer = DataFileWriter::create(schema, codec).unwrap();
        let mut out = Vec::new();
        for value in values {
            writer.append(value).unwrap();
            out.extend_from_slice(writer.take_bytes().unwrap().as_slice());
        }
        out.extend_from_slice(writer.finish().unwrap().as_slice());
        out
    }

    /// Reads every value from a complete file through the push reader.
    fn read_all(bytes: &[u8]) -> Vec<AvroValue> {
        let mut reader = DataFileReader::from_bytes(bytes).unwrap();
        let mut values = Vec::new();
        while reader.next_ready().unwrap() {
            values.push(reader.next_value().unwrap());
        }
        assert!(reader.at_end());
        values
    }

    fn roundtrip_with_codec(codec: AvroCodec) {
        let schema = AvroSchema::parse(RECORD_SCHEMA.as_bytes()).unwrap();
        let first = measurement("a", 1.5);
        let second = measurement("b", -2.25);
        let bytes = write_file(&schema, codec, &[first.clone(), second.clone()]);

        let mut reader = DataFileReader::from_bytes(&bytes).unwrap();
        assert!(reader.header_ready());
        assert!(reader.writer_schema().unwrap().equals(&schema));
        assert!(reader.next_ready().unwrap());
        // next_ready is idempotent: it does not consume.
        assert!(reader.next_ready().unwrap());
        assert!(reader.next_value().unwrap().equals(&first));
        assert!(reader.next_value().unwrap().equals(&second));
        assert!(!reader.next_ready().unwrap());
        assert!(reader.at_end());
        let message = String::from_utf8(reader.next_value().unwrap_err().into_vec()).unwrap();
        assert!(message.contains("No more values"), "unexpected error: {message}");
    }

    #[test]
    fn roundtrip_null_codec() {
        roundtrip_with_codec(AvroCodec::Null);
    }

    #[test]
    fn roundtrip_deflate_codec() {
        roundtrip_with_codec(AvroCodec::Deflate);
    }

    #[test]
    fn roundtrip_snappy_codec() {
        roundtrip_with_codec(AvroCodec::Snappy);
    }

    #[test]
    fn roundtrip_zstandard_codec() {
        roundtrip_with_codec(AvroCodec::Zstandard);
    }

    #[test]
    fn byte_at_a_time_feeding_hits_every_split_point() {
        // Feeding one byte at a time exercises every possible chunk split:
        // mid-magic, mid-varint, mid-key, mid-payload, mid-sync-marker.
        // Enough values to span several flush batches and many blocks.
        let schema = AvroSchema::parse(RECORD_SCHEMA.as_bytes()).unwrap();
        let n = 3000usize;
        let values: Vec<AvroValue> =
            (0..n).map(|i| measurement("sensor", i as f64)).collect();
        let bytes = write_file(&schema, AvroCodec::Deflate, &values);

        let mut reader = DataFileReader::create();
        let mut seen = 0usize;
        for byte in &bytes {
            reader.feed(std::slice::from_ref(byte)).unwrap();
            while reader.next_ready().unwrap() {
                let value = reader.next_value().unwrap();
                let got =
                    value.get_record_field(b"value").unwrap().get_double().unwrap();
                assert_eq!(got, seen as f64);
                seen += 1;
            }
        }
        reader.close_input().unwrap();
        assert!(!reader.next_ready().unwrap());
        assert_eq!(seen, n);
        assert!(reader.at_end());
    }

    #[test]
    fn interop_upstream_writer_to_push_reader() {
        // A file written by apache-avro's own Writer decodes with our push
        // reader (wire-format compatibility in the read direction).
        let schema = Schema::parse_str(RECORD_SCHEMA).unwrap();
        let mut upstream =
            UpstreamWriter::with_codec(&schema, Vec::new(), Codec::Deflate(Default::default()));
        for i in 0..100 {
            let value = measurement("s", i as f64);
            upstream.append_value_ref(&value.value).unwrap();
        }
        let bytes = upstream.into_inner().unwrap();

        let values = read_all(&bytes);
        assert_eq!(values.len(), 100);
        assert_eq!(
            values[99].get_record_field(b"value").unwrap().get_double().unwrap(),
            99.0
        );
    }

    #[test]
    fn interop_push_writer_to_upstream_reader() {
        // A file written by our writer decodes with apache-avro's own Reader
        // (wire-format compatibility in the write direction).
        let schema = AvroSchema::parse(RECORD_SCHEMA.as_bytes()).unwrap();
        let values: Vec<AvroValue> =
            (0..100).map(|i| measurement("s", i as f64)).collect();
        let bytes = write_file(&schema, AvroCodec::Snappy, &values);

        let upstream = UpstreamReader::new(bytes.as_slice()).unwrap();
        let decoded: Vec<Value> = upstream.map(|item| item.unwrap()).collect();
        assert_eq!(decoded.len(), 100);
        assert!(values[7].equals(&AvroValue { value: decoded[7].clone() }));
    }

    fn cross_referencing_schemas() -> Vec<AvroSchema> {
        let address = VecU8::from(
            r#"{"type": "record", "name": "Address", "fields": [
                {"name": "city", "type": "string"}]}"#,
        );
        let person = VecU8::from(
            r#"{"type": "record", "name": "Person", "fields": [
                {"name": "address", "type": "Address"}]}"#,
        );
        AvroSchema::parse_list(&[address, person]).unwrap().into_vec()
    }

    #[test]
    fn create_with_unresolved_refs_fails_instead_of_panicking() {
        // A schema from parse_list contains Schema::Ref nodes. It must be
        // rejected at creation time: appending to it would panic inside
        // apache-avro's validate path, and the container file header could
        // not embed the referenced types anyway.
        let schemas = cross_referencing_schemas();
        let person_schema = &schemas[1];
        let result = DataFileWriter::create(person_schema, AvroCodec::Null);
        let message = String::from_utf8(result.unwrap_err().into_vec()).unwrap();
        assert!(message.contains("not self-contained"));
    }

    #[test]
    fn inlined_nested_record_roundtrip() {
        // The documented alternative to cross-referencing schemas for
        // container files: define the nested type inline in one document.
        let schema = AvroSchema::parse(
            br#"{"type": "record", "name": "Person", "fields": [
                {"name": "address", "type": {
                    "type": "record", "name": "Address", "fields": [
                        {"name": "city", "type": "string"}]}}]}"#,
        )
        .unwrap();

        let mut address = AvroValue::create_record();
        address.record_put(b"city", &AvroValue::create_string(b"Zurich").unwrap()).unwrap();
        let mut person = AvroValue::create_record();
        person.record_put(b"address", &address).unwrap();

        let bytes = write_file(&schema, AvroCodec::Null, &[person.clone()]);
        let values = read_all(&bytes);
        assert!(values[0].equals(&person));
    }

    #[test]
    fn recursive_schema_roundtrip() {
        // A self-referential schema exercises named-type resolution in the
        // push reader's from_avro_datum_schemata path.
        let schema = AvroSchema::parse(
            br#"{"type": "record", "name": "Node", "fields": [
                {"name": "next", "type": ["null", "Node"]}]}"#,
        )
        .unwrap();

        let mut inner = AvroValue::create_record();
        inner
            .record_put(b"next", &AvroValue::create_union(0, &AvroValue::create_null()))
            .unwrap();
        let mut outer = AvroValue::create_record();
        outer.record_put(b"next", &AvroValue::create_union(1, &inner)).unwrap();

        let bytes = write_file(&schema, AvroCodec::Null, &[outer.clone()]);
        let values = read_all(&bytes);
        assert!(values[0].equals(&outer));
    }

    #[test]
    fn codec_from_i32() {
        assert_eq!(AvroCodec::from_i32(0).unwrap(), AvroCodec::Null);
        assert_eq!(AvroCodec::from_i32(3).unwrap(), AvroCodec::Zstandard);
        // bzip2 (4) and xz (5) are deliberately unsupported (avrocpp parity).
        assert!(AvroCodec::from_i32(4).is_err());
        assert!(AvroCodec::from_i32(5).is_err());
        assert!(AvroCodec::from_i32(-1).is_err());
    }

    #[test]
    fn codec_from_name_rejects_unsupported() {
        assert_eq!(AvroCodec::from_name(b"null").unwrap(), AvroCodec::Null);
        assert_eq!(AvroCodec::from_name(b"zstandard").unwrap(), AvroCodec::Zstandard);
        assert!(AvroCodec::from_name(b"bzip2").is_err());
        assert!(AvroCodec::from_name(b"xz").is_err());
        assert!(AvroCodec::from_name(b"lz4").is_err());
    }

    #[test]
    fn append_rejects_invalid_value_and_writer_stays_usable() {
        let schema = AvroSchema::parse(RECORD_SCHEMA.as_bytes()).unwrap();
        let mut writer = DataFileWriter::create(&schema, AvroCodec::Null).unwrap();
        assert!(writer.append(&AvroValue::create_long(1)).is_err());
        assert!(!writer.is_finished());
        // The writer still works after a rejected value.
        let value = measurement("ok", 1.0);
        writer.append(&value).unwrap();
        let bytes = writer.finish().unwrap();
        let values = read_all(bytes.as_slice());
        assert_eq!(values.len(), 1);
        assert!(values[0].equals(&value));
    }

    #[test]
    fn writer_finish_consumes_writer() {
        let schema = AvroSchema::parse(RECORD_SCHEMA.as_bytes()).unwrap();
        let mut writer = DataFileWriter::create(&schema, AvroCodec::Null).unwrap();
        writer.append(&measurement("x", 1.0)).unwrap();
        assert!(!writer.is_finished());
        let _ = writer.finish().unwrap();
        assert!(writer.is_finished());
        assert!(writer.append(&measurement("y", 2.0)).is_err());
        assert!(writer.take_bytes().is_err());
        assert!(writer.finish().is_err());
        // The schema stays queryable after finish.
        assert!(writer.schema().equals(&schema));
    }

    #[test]
    fn writer_take_bytes_drains_full_batches_before_finish() {
        let schema = AvroSchema::parse(RECORD_SCHEMA.as_bytes()).unwrap();
        let mut writer = DataFileWriter::create(&schema, AvroCodec::Null).unwrap();
        let n = 4000;
        let mut drained = Vec::new();
        let mut max_chunk = 0usize;
        for i in 0..n {
            writer.append(&measurement("sensor", i as f64)).unwrap();
            let chunk = writer.take_bytes().unwrap();
            max_chunk = max_chunk.max(chunk.len());
            drained.extend_from_slice(chunk.as_slice());
        }
        // Batches were flushed and drained before finish: the writer is not
        // accumulating the whole file, and no single drain approaches it.
        assert!(!drained.is_empty(), "expected batches drained before finish");
        assert!(max_chunk < 128 * 1024, "drain chunk grew to {max_chunk} bytes");
        drained.extend_from_slice(writer.finish().unwrap().as_slice());

        let values = read_all(&drained);
        assert_eq!(values.len(), n);
        assert_eq!(
            values[n - 1].get_record_field(b"value").unwrap().get_double().unwrap(),
            (n - 1) as f64
        );
    }

    #[test]
    fn reader_rejects_garbage() {
        assert!(DataFileReader::from_bytes(b"not an avro file").is_err());
        assert!(DataFileReader::from_bytes(b"").is_err());
        // A stream fed garbage fails fast, before 4 full magic bytes worth
        // of parsing state could matter.
        let mut reader = DataFileReader::create();
        reader.feed(b"XXXX").unwrap();
        assert!(reader.next_ready().is_err());
    }

    #[test]
    fn reader_resolves_to_reader_schema() {
        let writer_schema = AvroSchema::parse(
            br#"{"type": "record", "name": "R", "fields": [
                {"name": "a", "type": "int"}]}"#,
        )
        .unwrap();
        let reader_schema = AvroSchema::parse(
            br#"{"type": "record", "name": "R", "fields": [
                {"name": "a", "type": "long"},
                {"name": "b", "type": "string", "default": "d"}]}"#,
        )
        .unwrap();

        let mut record = AvroValue::create_record();
        record.record_put(b"a", &AvroValue::create_int(12)).unwrap();
        let bytes = write_file(&writer_schema, AvroCodec::Null, &[record]);

        let mut reader =
            DataFileReader::from_bytes_with_schema(&reader_schema, &bytes).unwrap();
        let value = reader.next_value().unwrap();
        assert_eq!(value.get_record_field(b"a").unwrap().get_long().unwrap(), 12);
        assert_eq!(
            value.get_record_field(b"b").unwrap().get_string().unwrap().as_slice(),
            b"d"
        );
        // The writer schema reported by the file stays the original one.
        assert!(reader.writer_schema().unwrap().equals(&writer_schema));
    }

    #[test]
    fn path_roundtrip() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("data.avro");
        let path_bytes = path.to_str().unwrap().as_bytes();

        let schema = AvroSchema::parse(RECORD_SCHEMA.as_bytes()).unwrap();
        let value = measurement("temp", 21.5);
        let bytes = write_file(&schema, AvroCodec::Deflate, &[value.clone()]);
        std::fs::write(&path, &bytes).unwrap();

        let mut reader = DataFileReader::from_path(path_bytes).unwrap();
        assert!(reader.next_value().unwrap().equals(&value));

        let mut resolved_reader =
            DataFileReader::from_path_with_schema(&schema, path_bytes).unwrap();
        assert!(resolved_reader.next_value().unwrap().equals(&value));
    }

    #[test]
    fn from_path_missing_file_fails() {
        assert!(DataFileReader::from_path(b"/nonexistent/file.avro").is_err());
    }

    #[test]
    fn golden_container_file_layout() {
        // A minimal OCF pinned against silent wire-format drift: header
        // magic, one record {v: 42} under a null codec.
        let schema = AvroSchema::parse(
            br#"{"type": "record", "name": "G", "fields": [
                {"name": "v", "type": "long"}]}"#,
        )
        .unwrap();
        let mut record = AvroValue::create_record();
        record.record_put(b"v", &AvroValue::create_long(42)).unwrap();
        let bytes = write_file(&schema, AvroCodec::Null, &[record.clone()]);

        // Magic per the Avro 1.x specification.
        assert_eq!(&bytes[..4], b"Obj\x01");
        // The single record (long 42, zigzag encoded as 0x54) is the last
        // data byte before the trailing 16-byte sync marker.
        let payload_end = bytes.len() - 16;
        assert_eq!(bytes[payload_end - 1], 0x54);
        assert!(read_all(&bytes)[0].equals(&record));
    }

    #[test]
    fn empty_file_roundtrips() {
        // create then finish with zero appends yields a valid header-only
        // file that both our reader and upstream's accept.
        let schema = AvroSchema::parse(RECORD_SCHEMA.as_bytes()).unwrap();
        let bytes = write_file(&schema, AvroCodec::Null, &[]);

        let mut reader = DataFileReader::from_bytes(&bytes).unwrap();
        assert!(reader.writer_schema().unwrap().equals(&schema));
        assert!(!reader.next_ready().unwrap());
        assert!(reader.at_end());
        assert!(reader.next_value().is_err());

        let upstream = UpstreamReader::new(bytes.as_slice()).unwrap();
        assert_eq!(upstream.count(), 0);
    }

    #[test]
    fn next_value_before_data_is_benign() {
        // "No value ready" is not fatal: feeding the rest of the file after
        // the error must still work.
        let schema = AvroSchema::parse(RECORD_SCHEMA.as_bytes()).unwrap();
        let value = measurement("a", 1.0);
        let bytes = write_file(&schema, AvroCodec::Null, &[value.clone()]);

        let mut reader = DataFileReader::create();
        reader.feed(&bytes[..10]).unwrap();
        let message = String::from_utf8(reader.next_value().unwrap_err().into_vec()).unwrap();
        assert!(message.contains("No value ready"), "unexpected error: {message}");

        reader.feed(&bytes[10..]).unwrap();
        reader.close_input().unwrap();
        assert!(reader.next_value().unwrap().equals(&value));
        assert!(!reader.next_ready().unwrap());
        assert!(reader.at_end());
    }

    #[test]
    fn truncated_stream_errors_and_fuses() {
        let schema = AvroSchema::parse(RECORD_SCHEMA.as_bytes()).unwrap();
        let bytes =
            write_file(&schema, AvroCodec::Null, &[measurement("a", 1.0), measurement("b", 2.0)]);
        let truncated = &bytes[..bytes.len() - 8];

        let mut reader = DataFileReader::create();
        reader.feed(truncated).unwrap();
        reader.close_input().unwrap();
        let mut errored = false;
        for _ in 0..100 {
            match reader.next_ready() {
                Ok(true) => {
                    if reader.next_value().is_err() {
                        errored = true;
                        break;
                    }
                }
                Ok(false) => break,
                Err(_) => {
                    errored = true;
                    break;
                }
            }
        }
        assert!(errored, "expected a truncation error");
        // Fatal errors are sticky: every subsequent call fails, including
        // feeding more data, and at_end never reports a clean end.
        assert!(reader.next_ready().is_err());
        assert!(reader.next_value().is_err());
        assert!(reader.feed(b"more").is_err());
        assert!(!reader.at_end());
    }

    #[test]
    fn feed_after_close_fails() {
        let mut reader = DataFileReader::create();
        reader.close_input().unwrap();
        // close_input is idempotent, feeding afterwards is not allowed.
        reader.close_input().unwrap();
        assert!(reader.feed(b"x").is_err());
    }

    #[test]
    fn sync_marker_mismatch_is_fatal() {
        let schema = AvroSchema::parse(RECORD_SCHEMA.as_bytes()).unwrap();
        let mut bytes = write_file(&schema, AvroCodec::Null, &[measurement("a", 1.0)]);
        // The file ends with the block's 16-byte sync marker; corrupt it.
        let last = bytes.len() - 1;
        bytes[last] ^= 0xff;

        let result = DataFileReader::from_bytes(&bytes);
        let message = String::from_utf8(result.unwrap_err().into_vec()).unwrap();
        assert!(message.contains("sync marker"), "unexpected error: {message}");
    }

    #[test]
    fn unknown_codec_in_header_is_rejected() {
        // Rewrite the header's codec name to an unsupported one of the same
        // length, so the framing stays intact but the codec lookup fails.
        let schema = AvroSchema::parse(RECORD_SCHEMA.as_bytes()).unwrap();
        let bytes = write_file(&schema, AvroCodec::Deflate, &[measurement("a", 1.0)]);
        let needle = b"deflate";
        let at = bytes
            .windows(needle.len())
            .position(|window| window == needle)
            .expect("codec name not found in header");
        let mut corrupted = bytes.clone();
        corrupted[at..at + needle.len()].copy_from_slice(b"unknown");

        let result = DataFileReader::from_bytes(&corrupted);
        let message = String::from_utf8(result.unwrap_err().into_vec()).unwrap();
        assert!(message.contains("Unsupported container codec"), "unexpected: {message}");
    }

    /// Returns the length of the file's header (everything up to and
    /// including the header sync marker). The marker equals the file's
    /// trailing 16 bytes.
    fn header_len(bytes: &[u8]) -> usize {
        let marker = &bytes[bytes.len() - 16..];
        let at = bytes
            .windows(16)
            .position(|window| window == marker)
            .expect("sync marker not found");
        at + 16
    }

    fn zigzag(n: i64) -> Vec<u8> {
        let mut z = ((n << 1) ^ (n >> 63)) as u64;
        let mut out = Vec::new();
        loop {
            let byte = (z & 0x7f) as u8;
            z >>= 7;
            if z == 0 {
                out.push(byte);
                break;
            }
            out.push(byte | 0x80);
        }
        out
    }

    #[test]
    fn negative_block_count_is_fatal() {
        let schema = AvroSchema::parse(RECORD_SCHEMA.as_bytes()).unwrap();
        let bytes = write_file(&schema, AvroCodec::Null, &[measurement("a", 1.0)]);
        let mut corrupted = bytes[..header_len(&bytes)].to_vec();
        corrupted.extend_from_slice(&zigzag(-1));

        let mut reader = DataFileReader::create();
        reader.feed(&corrupted).unwrap();
        let message = String::from_utf8(reader.next_ready().unwrap_err().into_vec()).unwrap();
        assert!(message.contains("negative object count"), "unexpected: {message}");
    }

    #[test]
    fn oversized_block_fails_fast_without_buffering() {
        let schema = AvroSchema::parse(RECORD_SCHEMA.as_bytes()).unwrap();
        let bytes = write_file(&schema, AvroCodec::Null, &[measurement("a", 1.0)]);
        // A block claiming to be 1 GiB must be rejected from its declared
        // size alone; no payload bytes are ever fed.
        let mut corrupted = bytes[..header_len(&bytes)].to_vec();
        corrupted.extend_from_slice(&zigzag(1));
        corrupted.extend_from_slice(&zigzag(1024 * 1024 * 1024));

        let mut reader = DataFileReader::create();
        reader.set_max_block_size(64 * 1024 * 1024).unwrap();
        reader.feed(&corrupted).unwrap();
        let message = String::from_utf8(reader.next_ready().unwrap_err().into_vec()).unwrap();
        assert!(message.contains("exceeds the maximum"), "unexpected: {message}");
    }

    #[test]
    fn oversized_header_length_fails_fast() {
        // A metadata key claiming to be enormous is rejected from its
        // declared length alone.
        let mut crafted = b"Obj\x01".to_vec();
        crafted.extend_from_slice(&zigzag(1)); // one metadata pair
        crafted.extend_from_slice(&zigzag((MAX_HEADER_BYTES as i64) + 1)); // key length

        let mut reader = DataFileReader::create();
        reader.feed(&crafted).unwrap();
        let message = String::from_utf8(reader.next_ready().unwrap_err().into_vec()).unwrap();
        assert!(message.contains("header cap"), "unexpected: {message}");
    }

    #[test]
    fn handmade_header_with_negative_map_count_parses() {
        // The Avro map encoding allows a negative pair count followed by the
        // group's byte size; upstream writers rarely emit it, so craft one.
        let schema_json = br#""long""#;
        let mut file = b"Obj\x01".to_vec();
        let mut entries = Vec::new();
        for (key, value) in
            [(&b"avro.schema"[..], &schema_json[..]), (&b"avro.codec"[..], &b"null"[..])]
        {
            entries.extend_from_slice(&zigzag(key.len() as i64));
            entries.extend_from_slice(key);
            entries.extend_from_slice(&zigzag(value.len() as i64));
            entries.extend_from_slice(value);
        }
        file.extend_from_slice(&zigzag(-2)); // two pairs, negative form
        file.extend_from_slice(&zigzag(entries.len() as i64)); // group byte size
        file.extend_from_slice(&entries);
        file.extend_from_slice(&zigzag(0)); // end of map
        let marker = [7u8; 16];
        file.extend_from_slice(&marker);
        // One block: one long value 42.
        let datum = to_avro_datum(&Schema::Long, Value::Long(42)).unwrap();
        file.extend_from_slice(&zigzag(1));
        file.extend_from_slice(&zigzag(datum.len() as i64));
        file.extend_from_slice(&datum);
        file.extend_from_slice(&marker);

        let mut reader = DataFileReader::from_bytes(&file).unwrap();
        assert_eq!(reader.next_value().unwrap().get_long().unwrap(), 42);
        assert!(!reader.next_ready().unwrap());
        assert!(reader.at_end());
    }

    #[test]
    fn run_of_zero_count_blocks_is_skipped() {
        // A run of degenerate zero-count blocks (all parsed within a single
        // next_ready call, exercising buffer compaction) must be skipped and
        // the real values behind them still decode.
        let schema = AvroSchema::parse(RECORD_SCHEMA.as_bytes()).unwrap();
        let value = measurement("after-zeros", 3.5);
        let bytes = write_file(&schema, AvroCodec::Null, &[value.clone()]);
        let split = header_len(&bytes);
        let mut crafted = bytes[..split].to_vec();
        let marker = &bytes[bytes.len() - 16..];
        for _ in 0..1000 {
            crafted.extend_from_slice(&zigzag(0)); // object count
            crafted.extend_from_slice(&zigzag(0)); // block size
            crafted.extend_from_slice(marker);
        }
        crafted.extend_from_slice(&bytes[split..]);

        let values = read_all(&crafted);
        assert_eq!(values.len(), 1);
        assert!(values[0].equals(&value));
    }

    #[test]
    fn chunked_header_with_many_metadata_pairs_parses_incrementally() {
        // A header with many metadata pairs fed byte-by-byte exercises the
        // resumable header parser: progress must be committed between feeds
        // (a from-scratch reparse per feed would be quadratic) and the
        // extracted schema/codec must still be correct.
        let mut file = b"Obj\x01".to_vec();
        let pairs: i64 = 500;
        file.extend_from_slice(&zigzag(pairs + 2));
        for i in 0..pairs {
            let key = format!("user.meta.{i}");
            file.extend_from_slice(&zigzag(key.len() as i64));
            file.extend_from_slice(key.as_bytes());
            file.extend_from_slice(&zigzag(4));
            file.extend_from_slice(b"junk");
        }
        for (key, value) in
            [(&b"avro.schema"[..], &br#""long""#[..]), (&b"avro.codec"[..], &b"null"[..])]
        {
            file.extend_from_slice(&zigzag(key.len() as i64));
            file.extend_from_slice(key);
            file.extend_from_slice(&zigzag(value.len() as i64));
            file.extend_from_slice(value);
        }
        file.extend_from_slice(&zigzag(0));
        let marker = [9u8; 16];
        file.extend_from_slice(&marker);
        let datum = to_avro_datum(&Schema::Long, Value::Long(7)).unwrap();
        file.extend_from_slice(&zigzag(1));
        file.extend_from_slice(&zigzag(datum.len() as i64));
        file.extend_from_slice(&datum);
        file.extend_from_slice(&marker);

        let mut reader = DataFileReader::create();
        let mut values = Vec::new();
        for byte in &file {
            reader.feed(std::slice::from_ref(byte)).unwrap();
            while reader.next_ready().unwrap() {
                values.push(reader.next_value().unwrap());
            }
        }
        reader.close_input().unwrap();
        assert!(!reader.next_ready().unwrap());
        assert!(reader.at_end());
        assert_eq!(values.len(), 1);
        assert_eq!(values[0].get_long().unwrap(), 7);
    }

    #[test]
    fn moved_out_default_states_error() {
        // Crubit replaces C++ moved-from objects with Default::default();
        // both husks must error loudly instead of acting alive.
        let mut reader = DataFileReader::default();
        assert!(reader.feed(b"Obj\x01").is_err());
        assert!(reader.next_ready().is_err());
        assert!(reader.next_value().is_err());
        assert!(reader.has_failed());
        assert!(!reader.at_end());

        let mut writer = DataFileWriter::default();
        assert!(writer.is_finished());
        assert!(writer.append(&AvroValue::create_long(1)).is_err());
        assert!(writer.take_bytes().is_err());
        assert!(writer.finish().is_err());
    }

    #[test]
    fn varint_hardening() {
        // Longer than 10 bytes.
        let mut pos = 0;
        let eleven = [0x80u8; 11];
        assert!(parse_long(&eleven, &mut pos).is_err());
        // 10th byte carrying more than the final bit.
        let mut pos = 0;
        let overflow = [0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x7f];
        assert!(parse_long(&overflow, &mut pos).is_err());
        // A valid maximal varint decodes.
        let mut pos = 0;
        let max = [0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01];
        assert_eq!(parse_long(&max, &mut pos).unwrap(), Some(i64::MIN));
        // Incomplete input reports NeedMore, not corruption.
        let mut pos = 0;
        assert_eq!(parse_long(&[0x80], &mut pos).unwrap(), None);
    }
}
