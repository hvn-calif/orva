//! Object Container File (OCF) reading and writing. Replaces avrocpp's
//! `DataFileWriter<GenericDatum>` / `DataFileReader<GenericDatum>`.
//!
//! Both types are buffer-oriented: the writer accumulates values and
//! produces the complete file on `to_bytes`/`write_to_path`; the reader
//! decodes the whole file eagerly at construction time. This sidesteps
//! apache-avro's borrowed-schema `Writer`/`Reader` types, which Crubit
//! cannot bridge, at the cost of holding the file contents in memory.

use crate::schema::AvroSchema;
use crate::value::AvroValue;
use crate::vec_u8::{catch_panic, utf8, Status, VecU8};
use apache_avro::schema::ResolvedSchema;
use apache_avro::types::Value;
use apache_avro::{Codec, Reader, Schema, Writer};
use self_cell::self_cell;
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

/// Writes Avro object container files.
///
/// Values are validated and buffered by `append`; the encoded file is
/// produced by `to_bytes` or `write_to_path`.
#[derive(Debug, Clone)]
pub struct DataFileWriter {
    schema: Schema,
    codec: AvroCodec,
    values: Vec<Value>,
}

impl Default for DataFileWriter {
    fn default() -> Self {
        DataFileWriter { schema: Schema::Null, codec: AvroCodec::Null, values: Vec::new() }
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
        Ok(DataFileWriter { schema: schema.schema.clone(), codec, values: Vec::new() })
    }

    /// Returns the schema this writer was created with.
    pub fn schema(&self) -> AvroSchema {
        AvroSchema { schema: self.schema.clone() }
    }

    /// Validates `value` against the writer schema and buffers it for
    /// writing. This is best-effort early feedback; `to_bytes` performs the
    /// authoritative validation while encoding.
    pub fn append(&mut self, value: &AvroValue) -> Status {
        // Safe from panics: `create` proved that this schema resolves.
        if !value.value.validate(&self.schema) {
            return Err(format!(
                "Value of type {} does not conform to the writer schema",
                String::from_utf8_lossy(value.type_name().as_slice())
            )
            .into());
        }
        self.values.push(value.value.clone());
        Ok(0)
    }

    /// Returns the number of buffered values.
    pub fn count(&self) -> usize {
        self.values.len()
    }

    /// Encodes all buffered values into a complete object container file.
    pub fn to_bytes(&self) -> Result<VecU8, VecU8> {
        catch_panic(|| {
            let mut writer = Writer::with_codec(&self.schema, Vec::new(), self.codec.into());
            for value in &self.values {
                if let Err(err) = writer.append_value_ref(value) {
                    return Err(err.to_string().into());
                }
            }
            match writer.into_inner() {
                Ok(bytes) => Ok(bytes.into()),
                Err(err) => Err(err.to_string().into()),
            }
        })
    }

    /// Encodes all buffered values and writes the container file to `path`.
    pub fn write_to_path(&self, raw_path: &[u8]) -> Status {
        let path = utf8(raw_path)?;
        let bytes = self.to_bytes()?;
        match std::fs::write(path, bytes.as_slice()) {
            Ok(()) => Ok(0),
            Err(err) => Err(err.to_string().into()),
        }
    }
}

/// Reads Avro object container files.
///
/// The whole file is decoded eagerly at construction; values are then
/// consumed with `has_next` / `next_value`.
#[derive(Debug, Clone)]
pub struct DataFileReader {
    writer_schema: Schema,
    values: Vec<Value>,
    position: usize,
}

impl Default for DataFileReader {
    fn default() -> Self {
        DataFileReader { writer_schema: Schema::Null, values: Vec::new(), position: 0 }
    }
}

impl DataFileReader {
    /// Opens a container file from a byte buffer. The writer schema is read
    /// from the file header.
    pub fn from_bytes(data: &[u8]) -> Result<DataFileReader, VecU8> {
        catch_panic(|| {
            let reader = Reader::new(data).map_err(|err| VecU8::from(err.to_string()))?;
            Self::drain(reader)
        })
    }

    /// Opens a container file from a byte buffer and resolves every value
    /// to `reader_schema` (schema evolution).
    pub fn from_bytes_with_schema(
        reader_schema: &AvroSchema,
        data: &[u8],
    ) -> Result<DataFileReader, VecU8> {
        catch_panic(|| {
            let reader = Reader::with_schema(&reader_schema.schema, data)
                .map_err(|err| VecU8::from(err.to_string()))?;
            Self::drain(reader)
        })
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

    fn drain(reader: Reader<'_, &[u8]>) -> Result<DataFileReader, VecU8> {
        let writer_schema = reader.writer_schema().clone();
        let mut values = Vec::new();
        for item in reader {
            match item {
                Ok(value) => values.push(value),
                Err(err) => return Err(err.to_string().into()),
            }
        }
        Ok(DataFileReader { writer_schema, values, position: 0 })
    }

    /// Returns the schema the file was written with.
    pub fn writer_schema(&self) -> AvroSchema {
        AvroSchema { schema: self.writer_schema.clone() }
    }

    /// Returns the total number of values in the file.
    pub fn count(&self) -> usize {
        self.values.len()
    }

    /// Returns true if `next_value` has more values to return.
    pub fn has_next(&self) -> bool {
        self.position < self.values.len()
    }

    /// Returns the next value. Returns an error once all values have been
    /// consumed.
    pub fn next_value(&mut self) -> Result<AvroValue, VecU8> {
        match self.values.get(self.position) {
            Some(value) => {
                self.position += 1;
                Ok(AvroValue { value: value.clone() })
            }
            None => Err("No more values in the container file".into()),
        }
    }

    /// Resets the read position to the first value.
    pub fn rewind(&mut self) {
        self.position = 0;
    }
}

// ---------------------------------------------------------------------------
// Streaming (Tier 1: in-memory / incremental) container IO.
//
// Unlike the buffered types above, these do not hold every value in memory:
// the writer emits encoded bytes block-by-block for the caller to drain, and
// the reader decodes one value per `next_value` instead of draining the whole
// file at construction. They bound the working set for large files.
//
// apache-avro's `Writer<'a, W>` and `Reader::with_schema` borrow the schema,
// so the writer and the reader-with-schema hold a `self_cell` that owns the
// `Schema` and the borrowing `Writer`/`Reader` together. (`Reader::new`
// borrows nothing, so the plain reader stores an owned `Reader<'static, _>`
// directly.) The `self_cell` Box-backs its dependent, so the outer wrapper is
// freely movable -- which is what Crubit requires to pass it by value.
//
// NOTE: Tier 1 fixes the codecs to in-memory `Cursor<Vec<u8>>` / `Vec<u8>`.
// Tier 2 (true external streams) would generalize these over `R: Read` /
// `W: Write` and add callback-backed IO types; see the README.
// ---------------------------------------------------------------------------

self_cell!(
    struct WriterCell {
        owner: Schema,
        #[covariant]
        dependent: WriterDep,
    }
);

struct WriterDep<'a> {
    writer: Writer<'a, Vec<u8>>,
}

/// Streaming Avro object container file writer.
///
/// `append` encodes into apache-avro's internal block buffer, which
/// auto-flushes a full block (~16 KiB) into an internal byte buffer once it
/// fills. `take_bytes` drains the already-flushed bytes (header + full
/// blocks) without forcing a partial flush, so blocks stay full-size.
/// `finish` flushes the final partial block, returns the remaining bytes, and
/// consumes the writer. The concatenation of every `take_bytes` result
/// followed by the `finish` result is a complete container file.
pub struct StreamingDataFileWriter {
    // `None` once `finish` has consumed the writer (moved-out guard).
    cell: Option<WriterCell>,
}

impl Default for StreamingDataFileWriter {
    fn default() -> Self {
        StreamingDataFileWriter { cell: None }
    }
}

impl std::fmt::Debug for StreamingDataFileWriter {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "StreamingDataFileWriter")
    }
}

impl StreamingDataFileWriter {
    /// Creates a streaming writer for the given self-contained schema and
    /// codec. Rejects cross-referencing schemas for the same reason as
    /// [`DataFileWriter::create`].
    pub fn create(
        schema: &AvroSchema,
        codec: AvroCodec,
    ) -> Result<StreamingDataFileWriter, VecU8> {
        if let Err(err) = ResolvedSchema::try_from(&schema.schema) {
            return Err(format!(
                "Schema is not self-contained and cannot be used for a container file \
                 (inline the referenced types in a single schema document): {}",
                err
            )
            .into());
        }
        let cell = WriterCell::new(schema.schema.clone(), |schema| WriterDep {
            writer: Writer::with_codec(schema, Vec::new(), codec.into()),
        });
        Ok(StreamingDataFileWriter { cell: Some(cell) })
    }

    /// Returns the schema this writer was created with. Errors once finished.
    pub fn schema(&self) -> Result<AvroSchema, VecU8> {
        let cell = self.cell.as_ref().ok_or_else(|| VecU8::from("Writer already finished"))?;
        Ok(AvroSchema { schema: cell.borrow_owner().clone() })
    }

    /// Validates `value` against the writer schema and encodes it. May
    /// auto-flush a full block into the internal buffer. Errors once finished.
    pub fn append(&mut self, value: &AvroValue) -> Status {
        catch_panic(|| {
            let cell = self.cell.as_mut().ok_or_else(|| VecU8::from("Writer already finished"))?;
            cell.with_dependent_mut(|schema, dep| {
                // Validate first so an invalid value cannot corrupt the
                // in-progress block buffer (matches DataFileWriter::append).
                if !value.value.validate(schema) {
                    return Err(VecU8::from(format!(
                        "Value of type {} does not conform to the writer schema",
                        String::from_utf8_lossy(value.type_name().as_slice())
                    )));
                }
                match dep.writer.append_value_ref(&value.value) {
                    Ok(_) => Ok(0),
                    Err(err) => Err(err.to_string().into()),
                }
            })
        })
    }

    /// Drains the bytes already flushed to the internal buffer (header plus
    /// any full blocks) without forcing a partial flush. May return empty.
    /// Errors once finished.
    pub fn take_bytes(&mut self) -> Result<VecU8, VecU8> {
        catch_panic(|| {
            let cell = self.cell.as_mut().ok_or_else(|| VecU8::from("Writer already finished"))?;
            let bytes = cell.with_dependent_mut(|_schema, dep| {
                std::mem::take(dep.writer.get_mut())
            });
            Ok(bytes.into())
        })
    }

    /// Flushes any pending block, returns all remaining bytes, and consumes
    /// the writer. Subsequent calls to any method error. (With no buffered
    /// values -- e.g. an empty file -- no block is written; the result is
    /// just the header plus whatever was already flushed.)
    pub fn finish(&mut self) -> Result<VecU8, VecU8> {
        catch_panic(|| {
            let cell = self.cell.as_mut().ok_or_else(|| VecU8::from("Writer already finished"))?;
            let bytes = cell.with_dependent_mut(|_schema, dep| {
                dep.writer.flush().map_err(|err| VecU8::from(err.to_string()))?;
                Ok::<Vec<u8>, VecU8>(std::mem::take(dep.writer.get_mut()))
            })?;
            // Drop the writer (and its borrowed schema) by clearing the cell.
            // We deliberately avoid `Writer::into_inner` (which consumes by
            // value) because self_cell does not let us move the dependent out;
            // an explicit flush + drain produces the identical bytes, and the
            // final flush leaves num_values == 0 so the Writer's own Drop does
            // not re-emit anything.
            self.cell = None;
            Ok(bytes.into())
        })
    }

    /// Returns true once `finish` has consumed the writer.
    pub fn is_finished(&self) -> bool {
        self.cell.is_none()
    }
}

type PlainReader = Reader<'static, Cursor<Vec<u8>>>;

self_cell!(
    struct ResolvingReaderCell {
        owner: Schema,
        #[covariant]
        dependent: ReaderDep,
    }
);

struct ReaderDep<'a> {
    reader: Reader<'a, Cursor<Vec<u8>>>,
}

enum ReaderInner {
    /// `Reader::new` borrows nothing, so this is fully owned ('static).
    Plain(PlainReader),
    /// `Reader::with_schema` borrows the reader schema; the cell owns both.
    Resolving(ResolvingReaderCell),
}

/// Streaming Avro object container file reader.
///
/// Decodes one value per `next_value` instead of draining the whole file at
/// construction. A single-value lookahead backs `has_next`. There is no
/// `count` (unknown without consuming the file) and no `rewind` (a consumed
/// stream cannot be rewound); use [`DataFileReader`] when you need either.
pub struct StreamingDataFileReader {
    // `None` only in the Default / moved-out state.
    inner: Option<ReaderInner>,
    writer_schema: Schema,
    // Single-value lookahead populated by `has_next`/`next_value`.
    lookahead: Option<Value>,
    // A decode error observed while peeking, surfaced on the next
    // `next_value` so streaming never silently truncates on a bad record.
    pending_error: Option<VecU8>,
    // The underlying iterator returned None (clean end of file) or the reader
    // is in a moved-out/panicked state; no further values will be pulled.
    exhausted: bool,
}

impl Default for StreamingDataFileReader {
    fn default() -> Self {
        StreamingDataFileReader {
            inner: None,
            writer_schema: Schema::Null,
            lookahead: None,
            pending_error: None,
            exhausted: true,
        }
    }
}

impl std::fmt::Debug for StreamingDataFileReader {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "StreamingDataFileReader")
    }
}

impl StreamingDataFileReader {
    /// Opens a container file from a byte buffer. The writer schema is read
    /// from the header. The bytes are copied into an owned buffer (lifetimes
    /// over FFI are hard; mirrors the other readers in this crate).
    pub fn from_bytes(data: &[u8]) -> Result<StreamingDataFileReader, VecU8> {
        catch_panic(|| {
            let reader: PlainReader = Reader::new(Cursor::new(data.to_vec()))
                .map_err(|err| VecU8::from(err.to_string()))?;
            let writer_schema = reader.writer_schema().clone();
            Ok(StreamingDataFileReader {
                inner: Some(ReaderInner::Plain(reader)),
                writer_schema,
                lookahead: None,
                pending_error: None,
                exhausted: false,
            })
        })
    }

    /// Opens a container file and resolves every value to `reader_schema`.
    pub fn from_bytes_with_schema(
        reader_schema: &AvroSchema,
        data: &[u8],
    ) -> Result<StreamingDataFileReader, VecU8> {
        catch_panic(|| {
            let bytes = data.to_vec();
            let cell = ResolvingReaderCell::try_new(reader_schema.schema.clone(), |schema| {
                Reader::with_schema(schema, Cursor::new(bytes))
                    .map(|reader| ReaderDep { reader })
                    .map_err(|err| VecU8::from(err.to_string()))
            })?;
            let writer_schema = cell.borrow_dependent().reader.writer_schema().clone();
            Ok(StreamingDataFileReader {
                inner: Some(ReaderInner::Resolving(cell)),
                writer_schema,
                lookahead: None,
                pending_error: None,
                exhausted: false,
            })
        })
    }

    /// Opens a container file from the filesystem.
    pub fn from_path(raw_path: &[u8]) -> Result<StreamingDataFileReader, VecU8> {
        let path = utf8(raw_path)?;
        let data = std::fs::read(path).map_err(|err| VecU8::from(err.to_string()))?;
        Self::from_bytes(&data)
    }

    /// Opens a container file from the filesystem, resolving every value to
    /// `reader_schema`.
    pub fn from_path_with_schema(
        reader_schema: &AvroSchema,
        raw_path: &[u8],
    ) -> Result<StreamingDataFileReader, VecU8> {
        let path = utf8(raw_path)?;
        let data = std::fs::read(path).map_err(|err| VecU8::from(err.to_string()))?;
        Self::from_bytes_with_schema(reader_schema, &data)
    }

    /// Returns the schema the file was written with.
    pub fn writer_schema(&self) -> AvroSchema {
        AvroSchema { schema: self.writer_schema.clone() }
    }

    /// Returns true if a subsequent `next_value` would return a value or
    /// surface a decode error. Fills the single-value lookahead if needed.
    pub fn has_next(&mut self) -> bool {
        self.fill();
        self.lookahead.is_some() || self.pending_error.is_some()
    }

    /// Returns the next value, decoding it lazily. Errors once all values are
    /// consumed, or surfaces a decode error observed while peeking.
    pub fn next_value(&mut self) -> Result<AvroValue, VecU8> {
        self.fill();
        if let Some(value) = self.lookahead.take() {
            return Ok(AvroValue { value });
        }
        if let Some(err) = self.pending_error.take() {
            return Err(err);
        }
        Err("No more values in the container file".into())
    }

    /// Ensures the lookahead is populated (with a value, a pending error, or
    /// the exhausted flag) by pulling at most one item from the iterator.
    /// The pull decodes a value, so it runs inside `catch_panic`.
    fn fill(&mut self) {
        if self.lookahead.is_some() || self.pending_error.is_some() || self.exhausted {
            return;
        }
        match catch_panic(|| Ok::<_, VecU8>(self.pull_raw())) {
            Ok(Some(Ok(value))) => self.lookahead = Some(value),
            Ok(Some(Err(err))) => self.pending_error = Some(err),
            Ok(None) => self.exhausted = true,
            Err(panic_err) => {
                // A panic may leave the reader torn; fuse it so we never pull
                // again, and surface the panic as the next error.
                self.pending_error = Some(panic_err);
                self.exhausted = true;
            }
        }
    }

    fn pull_raw(&mut self) -> Option<Result<Value, VecU8>> {
        let item = match self.inner.as_mut() {
            Some(ReaderInner::Plain(reader)) => reader.next(),
            Some(ReaderInner::Resolving(cell)) => {
                cell.with_dependent_mut(|_schema, dep| dep.reader.next())
            }
            None => None,
        };
        item.map(|res| res.map_err(|err| VecU8::from(err.to_string())))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

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

    fn roundtrip_with_codec(codec: AvroCodec) {
        let schema = AvroSchema::parse(RECORD_SCHEMA.as_bytes()).unwrap();
        let mut writer = DataFileWriter::create(&schema, codec).unwrap();
        let first = measurement("a", 1.5);
        let second = measurement("b", -2.25);
        writer.append(&first).unwrap();
        writer.append(&second).unwrap();
        assert_eq!(writer.count(), 2);

        let bytes = writer.to_bytes().unwrap();
        let mut reader = DataFileReader::from_bytes(bytes.as_slice()).unwrap();
        assert_eq!(reader.count(), 2);
        assert!(reader.writer_schema().equals(&schema));
        assert!(reader.has_next());
        assert!(reader.next_value().unwrap().equals(&first));
        assert!(reader.next_value().unwrap().equals(&second));
        assert!(!reader.has_next());
        assert!(reader.next_value().is_err());

        reader.rewind();
        assert!(reader.next_value().unwrap().equals(&first));
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

        let mut writer = DataFileWriter::create(&schema, AvroCodec::Null).unwrap();
        writer.append(&person).unwrap();
        let bytes = writer.to_bytes().unwrap();
        let mut reader = DataFileReader::from_bytes(bytes.as_slice()).unwrap();
        assert!(reader.next_value().unwrap().equals(&person));
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
    fn append_rejects_invalid_value() {
        let schema = AvroSchema::parse(RECORD_SCHEMA.as_bytes()).unwrap();
        let mut writer = DataFileWriter::create(&schema, AvroCodec::Null).unwrap();
        assert!(writer.append(&AvroValue::create_long(1)).is_err());
        assert_eq!(writer.count(), 0);
    }

    #[test]
    fn reader_rejects_garbage() {
        assert!(DataFileReader::from_bytes(b"not an avro file").is_err());
        assert!(DataFileReader::from_bytes(b"").is_err());
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
        let mut writer = DataFileWriter::create(&writer_schema, AvroCodec::Null).unwrap();
        writer.append(&record).unwrap();
        let bytes = writer.to_bytes().unwrap();

        let mut reader =
            DataFileReader::from_bytes_with_schema(&reader_schema, bytes.as_slice()).unwrap();
        let value = reader.next_value().unwrap();
        assert_eq!(value.get_record_field(b"a").unwrap().get_long().unwrap(), 12);
        assert_eq!(
            value.get_record_field(b"b").unwrap().get_string().unwrap().as_slice(),
            b"d"
        );
        // The writer schema reported by the file stays the original one.
        assert!(reader.writer_schema().equals(&writer_schema));
    }

    #[test]
    fn path_roundtrip() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("data.avro");
        let path_bytes = path.to_str().unwrap().as_bytes();

        let schema = AvroSchema::parse(RECORD_SCHEMA.as_bytes()).unwrap();
        let mut writer = DataFileWriter::create(&schema, AvroCodec::Deflate).unwrap();
        let value = measurement("temp", 21.5);
        writer.append(&value).unwrap();
        writer.write_to_path(path_bytes).unwrap();

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
    fn golden_container_file_decodes() {
        // A minimal OCF written by apache-avro itself, pinned as a byte
        // vector to guard against silent wire-format drift: header magic,
        // schema {"type":"record","name":"G","fields":[{"name":"v","type":"long"}]},
        // null codec, one record {v: 42}.
        let schema = AvroSchema::parse(
            br#"{"type": "record", "name": "G", "fields": [
                {"name": "v", "type": "long"}]}"#,
        )
        .unwrap();
        let mut record = AvroValue::create_record();
        record.record_put(b"v", &AvroValue::create_long(42)).unwrap();
        let mut writer = DataFileWriter::create(&schema, AvroCodec::Null).unwrap();
        writer.append(&record).unwrap();
        let bytes = writer.to_bytes().unwrap();

        // Magic per the Avro 1.x specification.
        assert_eq!(&bytes.as_slice()[..4], b"Obj\x01");
        // The single record (long 42, zigzag encoded as 0x54) is the last
        // data byte before the trailing 16-byte sync marker.
        let payload_end = bytes.len() - 16;
        assert_eq!(bytes.as_slice()[payload_end - 1], 0x54);
    }

    // -- Streaming -----------------------------------------------------------

    /// Writes `values` through the streaming writer, draining after every
    /// append, and returns the complete container file bytes.
    fn stream_write(schema: &AvroSchema, codec: AvroCodec, values: &[AvroValue]) -> Vec<u8> {
        let mut writer = StreamingDataFileWriter::create(schema, codec).unwrap();
        let mut out = Vec::new();
        for value in values {
            writer.append(value).unwrap();
            out.extend_from_slice(writer.take_bytes().unwrap().as_slice());
        }
        out.extend_from_slice(writer.finish().unwrap().as_slice());
        out
    }

    fn streaming_roundtrip_with_codec(codec: AvroCodec) {
        let schema = AvroSchema::parse(RECORD_SCHEMA.as_bytes()).unwrap();
        let first = measurement("a", 1.5);
        let second = measurement("b", -2.25);
        let bytes = stream_write(&schema, codec, &[first.clone(), second.clone()]);

        let mut reader = StreamingDataFileReader::from_bytes(&bytes).unwrap();
        assert!(reader.writer_schema().equals(&schema));
        assert!(reader.has_next());
        // has_next is idempotent: calling it again does not consume.
        assert!(reader.has_next());
        assert!(reader.next_value().unwrap().equals(&first));
        assert!(reader.next_value().unwrap().equals(&second));
        assert!(!reader.has_next());
        assert!(reader.next_value().is_err());
    }

    #[test]
    fn streaming_roundtrip_null_codec() {
        streaming_roundtrip_with_codec(AvroCodec::Null);
    }

    #[test]
    fn streaming_roundtrip_deflate_codec() {
        streaming_roundtrip_with_codec(AvroCodec::Deflate);
    }

    #[test]
    fn streaming_roundtrip_snappy_codec() {
        streaming_roundtrip_with_codec(AvroCodec::Snappy);
    }

    #[test]
    fn streaming_roundtrip_zstandard_codec() {
        streaming_roundtrip_with_codec(AvroCodec::Zstandard);
    }

    #[test]
    fn streaming_take_bytes_emits_full_blocks_before_finish() {
        let schema = AvroSchema::parse(RECORD_SCHEMA.as_bytes()).unwrap();
        let mut writer = StreamingDataFileWriter::create(&schema, AvroCodec::Null).unwrap();
        let n = 4000;
        let mut drained = Vec::new();
        for i in 0..n {
            writer.append(&measurement("sensor", i as f64)).unwrap();
            drained.extend_from_slice(writer.take_bytes().unwrap().as_slice());
        }
        // Full blocks were flushed and drained before finish (the writer is
        // not accumulating every value).
        assert!(!drained.is_empty(), "expected full blocks drained before finish");
        drained.extend_from_slice(writer.finish().unwrap().as_slice());

        let mut reader = StreamingDataFileReader::from_bytes(&drained).unwrap();
        let mut count = 0;
        while reader.has_next() {
            let value = reader.next_value().unwrap();
            assert_eq!(
                value.get_record_field(b"value").unwrap().get_double().unwrap(),
                count as f64
            );
            count += 1;
        }
        assert_eq!(count, n);
    }

    #[test]
    fn streaming_finish_consumes_writer() {
        let schema = AvroSchema::parse(RECORD_SCHEMA.as_bytes()).unwrap();
        let mut writer = StreamingDataFileWriter::create(&schema, AvroCodec::Null).unwrap();
        writer.append(&measurement("x", 1.0)).unwrap();
        assert!(!writer.is_finished());
        let _ = writer.finish().unwrap();
        assert!(writer.is_finished());
        // Every method errors after finish.
        assert!(writer.append(&measurement("y", 2.0)).is_err());
        assert!(writer.take_bytes().is_err());
        assert!(writer.finish().is_err());
        assert!(writer.schema().is_err());
    }

    #[test]
    fn streaming_append_rejects_invalid_value() {
        let schema = AvroSchema::parse(RECORD_SCHEMA.as_bytes()).unwrap();
        let mut writer = StreamingDataFileWriter::create(&schema, AvroCodec::Null).unwrap();
        assert!(writer.append(&AvroValue::create_long(1)).is_err());
    }

    #[test]
    fn streaming_create_with_unresolved_refs_fails() {
        let schemas = cross_referencing_schemas();
        let person_schema = &schemas[1];
        let result = StreamingDataFileWriter::create(person_schema, AvroCodec::Null);
        let message = String::from_utf8(result.unwrap_err().into_vec()).unwrap();
        assert!(message.contains("not self-contained"));
    }

    #[test]
    fn streaming_reader_rejects_garbage() {
        assert!(StreamingDataFileReader::from_bytes(b"not an avro file").is_err());
        assert!(StreamingDataFileReader::from_bytes(b"").is_err());
    }

    #[test]
    fn streaming_reader_resolves_to_reader_schema() {
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
        record.record_put(b"a", &AvroValue::create_int(7)).unwrap();
        let bytes = stream_write(&writer_schema, AvroCodec::Null, &[record]);

        let mut reader =
            StreamingDataFileReader::from_bytes_with_schema(&reader_schema, &bytes).unwrap();
        let value = reader.next_value().unwrap();
        assert_eq!(value.get_record_field(b"a").unwrap().get_long().unwrap(), 7);
        assert_eq!(
            value.get_record_field(b"b").unwrap().get_string().unwrap().as_slice(),
            b"d"
        );
        // The reported writer schema stays the original one.
        assert!(reader.writer_schema().equals(&writer_schema));
    }

    #[test]
    fn streaming_empty_file_roundtrips() {
        // create then finish with zero appends yields a valid header-only OCF.
        let schema = AvroSchema::parse(RECORD_SCHEMA.as_bytes()).unwrap();
        let bytes = stream_write(&schema, AvroCodec::Null, &[]);

        let mut reader = StreamingDataFileReader::from_bytes(&bytes).unwrap();
        assert!(reader.writer_schema().equals(&schema));
        assert!(!reader.has_next());
        assert!(reader.next_value().is_err());
        // The buffered reader accepts the same empty file.
        assert_eq!(DataFileReader::from_bytes(&bytes).unwrap().count(), 0);
    }

    #[test]
    fn streaming_reader_surfaces_decode_error_once_then_fuses() {
        let schema = AvroSchema::parse(RECORD_SCHEMA.as_bytes()).unwrap();
        let bytes =
            stream_write(&schema, AvroCodec::Null, &[measurement("a", 1.0), measurement("b", 2.0)]);
        // Truncate into the trailing block so the header still parses (the
        // reader constructs) but decoding the block fails mid-stream.
        let truncated = &bytes[..bytes.len() - 8];
        let mut reader = StreamingDataFileReader::from_bytes(truncated).unwrap();

        let mut errored = false;
        for _ in 0..100 {
            if !reader.has_next() {
                break;
            }
            if reader.next_value().is_err() {
                errored = true;
                break;
            }
        }
        assert!(errored, "expected a mid-stream decode error");
        // After the error the reader is fused: no more values, no panic, no
        // double-surfacing of the error.
        assert!(!reader.has_next());
        assert!(reader.next_value().is_err());
    }

    #[test]
    fn streaming_output_decodes_with_buffered_reader() {
        // The streaming writer produces a valid OCF the buffered reader reads.
        let schema = AvroSchema::parse(RECORD_SCHEMA.as_bytes()).unwrap();
        let first = measurement("a", 1.5);
        let second = measurement("b", -2.25);
        let bytes = stream_write(&schema, AvroCodec::Deflate, &[first.clone(), second.clone()]);

        let mut reader = DataFileReader::from_bytes(&bytes).unwrap();
        assert_eq!(reader.count(), 2);
        assert!(reader.next_value().unwrap().equals(&first));
        assert!(reader.next_value().unwrap().equals(&second));
    }

    #[test]
    fn buffered_output_decodes_with_streaming_reader() {
        // ...and the streaming reader reads a file the buffered writer wrote.
        let schema = AvroSchema::parse(RECORD_SCHEMA.as_bytes()).unwrap();
        let first = measurement("a", 1.5);
        let second = measurement("b", -2.25);
        let mut writer = DataFileWriter::create(&schema, AvroCodec::Snappy).unwrap();
        writer.append(&first).unwrap();
        writer.append(&second).unwrap();
        let bytes = writer.to_bytes().unwrap();

        let mut reader = StreamingDataFileReader::from_bytes(bytes.as_slice()).unwrap();
        assert!(reader.next_value().unwrap().equals(&first));
        assert!(reader.next_value().unwrap().equals(&second));
        assert!(!reader.has_next());
    }
}
