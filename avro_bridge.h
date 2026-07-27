#ifndef SECURITY_AVRO_AVRO_BRIDGE_H_
#define SECURITY_AVRO_AVRO_BRIDGE_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "crubit/rust.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"

// Memory-safe replacement for avrocpp: the C++ API below delegates all
// parsing, encoding and decoding to the Rust apache-avro crate through
// Crubit-generated bindings.
namespace security::avro {

class AvroValue;
class DataFileWriter;
class DataFileReader;

// Compression codec for object container files. The set deliberately
// matches avrocpp (null/deflate/snappy/zstd); bzip2 and xz are excluded.
// Values must stay in sync with rust::container::AvroCodec::from_i32.
// Note: no compressing codec bounds its decompressed size, so reading
// compressed files from untrusted sources needs an external memory limit.
enum class Codec : int32_t {
  kNull = 0,
  kDeflate = 1,
  kSnappy = 2,
  kZstandard = 3,
};

// An Avro schema. Replaces avrocpp's `ValidSchema`.
class AvroSchema final {
 public:
  // Parses a schema from its JSON representation.
  static absl::StatusOr<AvroSchema> Parse(absl::string_view json);

  // Parses a list of schemas that may reference each other by name. The
  // returned schemas are in the same order as the input.
  static absl::StatusOr<std::vector<AvroSchema>> ParseList(
      absl::Span<const absl::string_view> jsons);

  // Returns the Parsing Canonical Form defined by the Avro specification.
  std::string CanonicalForm() const;

  // Fingerprints of the canonical form. FingerprintRabin matches the
  // signed 64-bit CRC-64-AVRO value produced by the Java implementation.
  int64_t FingerprintRabin() const;
  std::string FingerprintRabinHex() const;
  std::string FingerprintMd5Hex() const;
  std::string FingerprintSha256Hex() const;

  // Name accessors; fail for unnamed (non record/enum/fixed) schemas.
  absl::StatusOr<std::string> Name() const;
  absl::StatusOr<std::string> Namespace() const;
  absl::StatusOr<std::string> FullName() const;

  // Returns the JSON representation of this schema.
  absl::StatusOr<std::string> ToJsonString() const;

  // Returns the schema type name, e.g. "record" or "timestamp-millis".
  std::string TypeName() const;

  bool IsNull() const;
  bool IsBoolean() const;
  bool IsInt() const;
  bool IsLong() const;
  bool IsFloat() const;
  bool IsDouble() const;
  bool IsBytes() const;
  bool IsString() const;
  bool IsRecord() const;
  bool IsEnum() const;
  bool IsArray() const;
  bool IsMap() const;
  bool IsUnion() const;
  bool IsFixed() const;

  // Schema evolution: checks that data written with `writer` can be read
  // using this schema. The error explains the incompatibility.
  absl::Status CanReadFrom(const AvroSchema& writer) const;

  // Checks that data written with either schema can be read with the other.
  absl::Status MutualRead(const AvroSchema& other) const;

  bool operator==(const AvroSchema& other) const;
  bool operator!=(const AvroSchema& other) const;

 private:
  friend class AvroValue;
  friend class DataFileWriter;
  friend class DataFileReader;
  friend absl::StatusOr<std::string> EncodeDatum(const AvroSchema& schema,
                                                 const AvroValue& value);
  friend absl::StatusOr<std::string> EncodeDatumSchemata(
      const AvroSchema& schema, absl::Span<const AvroSchema> schemata,
      const AvroValue& value);
  friend absl::StatusOr<AvroValue> DecodeDatum(const AvroSchema& writer_schema,
                                               absl::string_view data);
  friend absl::StatusOr<AvroValue> DecodeDatumResolved(
      const AvroSchema& writer_schema, const AvroSchema& reader_schema,
      absl::string_view data);
  friend absl::StatusOr<AvroValue> DecodeDatumSchemata(
      const AvroSchema& writer_schema,
      absl::Span<const AvroSchema> writer_schemata, absl::string_view data);

  explicit AvroSchema(rust::schema::AvroSchema schema);

  rust::schema::AvroSchema schema_;
};

// A generic Avro value tree. Replaces avrocpp's `GenericDatum`.
class AvroValue final {
 public:
  // Constructors for primitive types.
  static AvroValue CreateNull();
  static AvroValue CreateBoolean(bool value);
  static AvroValue CreateInt(int32_t value);
  static AvroValue CreateLong(int64_t value);
  static AvroValue CreateFloat(float value);
  static AvroValue CreateDouble(double value);
  static AvroValue CreateBytes(absl::string_view value);
  static absl::StatusOr<AvroValue> CreateString(absl::string_view value);

  // Constructors for complex types. Records, arrays and maps start empty
  // and are filled with RecordPut / ArrayPush / MapPut.
  static AvroValue CreateRecord();
  static AvroValue CreateArray();
  static AvroValue CreateMap();
  static absl::StatusOr<AvroValue> CreateEnum(uint32_t position,
                                              absl::string_view symbol);
  static AvroValue CreateFixed(absl::string_view value);
  static AvroValue CreateUnion(uint32_t branch_index, const AvroValue& value);

  // Constructors for logical types.
  static AvroValue CreateDecimal(absl::string_view big_endian_bytes);
  static absl::StatusOr<AvroValue> CreateUuid(absl::string_view value);
  static AvroValue CreateDate(int32_t days_since_epoch);
  static AvroValue CreateTimeMillis(int32_t value);
  static AvroValue CreateTimeMicros(int64_t value);
  static AvroValue CreateTimestampMillis(int64_t value);
  static AvroValue CreateTimestampMicros(int64_t value);
  static AvroValue CreateTimestampNanos(int64_t value);
  static AvroValue CreateLocalTimestampMillis(int64_t value);
  static AvroValue CreateLocalTimestampMicros(int64_t value);
  static AvroValue CreateLocalTimestampNanos(int64_t value);
  static AvroValue CreateDuration(uint32_t months, uint32_t days,
                                  uint32_t millis);

  // Accessors. Each fails with kFailedPrecondition if the value holds a
  // different type.
  absl::StatusOr<bool> GetBoolean() const;
  absl::StatusOr<int32_t> GetInt() const;
  absl::StatusOr<int64_t> GetLong() const;
  absl::StatusOr<float> GetFloat() const;
  absl::StatusOr<double> GetDouble() const;
  absl::StatusOr<std::string> GetBytes() const;
  absl::StatusOr<std::string> GetString() const;
  absl::StatusOr<uint32_t> GetEnumPosition() const;
  absl::StatusOr<std::string> GetEnumSymbol() const;
  absl::StatusOr<std::string> GetFixedBytes() const;
  absl::StatusOr<uint32_t> GetUnionBranch() const;
  absl::StatusOr<AvroValue> GetUnionValue() const;

  // Accessors for logical types.
  absl::StatusOr<std::string> GetDecimalBytes() const;
  absl::StatusOr<std::string> GetUuid() const;
  absl::StatusOr<int32_t> GetDate() const;
  absl::StatusOr<int32_t> GetTimeMillis() const;
  absl::StatusOr<int64_t> GetTimeMicros() const;
  absl::StatusOr<int64_t> GetTimestampMillis() const;
  absl::StatusOr<int64_t> GetTimestampMicros() const;
  absl::StatusOr<int64_t> GetTimestampNanos() const;
  absl::StatusOr<int64_t> GetLocalTimestampMillis() const;
  absl::StatusOr<int64_t> GetLocalTimestampMicros() const;
  absl::StatusOr<int64_t> GetLocalTimestampNanos() const;
  absl::StatusOr<uint32_t> GetDurationMonths() const;
  absl::StatusOr<uint32_t> GetDurationDays() const;
  absl::StatusOr<uint32_t> GetDurationMillis() const;

  // Array access.
  absl::StatusOr<size_t> GetArrayLen() const;
  absl::StatusOr<AvroValue> GetArrayItem(size_t index) const;

  // Map access. Keys are returned sorted lexicographically.
  absl::StatusOr<size_t> GetMapLen() const;
  absl::StatusOr<std::vector<std::string>> GetMapKeys() const;
  absl::StatusOr<AvroValue> GetMapValue(absl::string_view key) const;
  absl::StatusOr<bool> HasMapKey(absl::string_view key) const;

  // Record access. Field names are returned in field order.
  absl::StatusOr<size_t> GetRecordLen() const;
  absl::StatusOr<std::vector<std::string>> GetRecordFieldNames() const;
  absl::StatusOr<AvroValue> GetRecordField(absl::string_view name) const;
  absl::StatusOr<bool> HasRecordField(absl::string_view name) const;

  // Mutators. RecordPut and MapPut replace existing entries with the same
  // name/key.
  absl::Status RecordPut(absl::string_view name, const AvroValue& value);
  absl::Status ArrayPush(const AvroValue& value);
  absl::Status MapPut(absl::string_view key, const AvroValue& value);

  // Type predicates.
  bool IsNull() const;
  bool IsBoolean() const;
  bool IsInt() const;
  bool IsLong() const;
  bool IsFloat() const;
  bool IsDouble() const;
  bool IsBytes() const;
  bool IsString() const;
  bool IsRecord() const;
  bool IsEnum() const;
  bool IsArray() const;
  bool IsMap() const;
  bool IsUnion() const;
  bool IsFixed() const;
  bool IsDecimal() const;
  bool IsUuid() const;
  bool IsDate() const;
  bool IsTimeMillis() const;
  bool IsTimeMicros() const;
  bool IsTimestampMillis() const;
  bool IsTimestampMicros() const;
  bool IsTimestampNanos() const;
  bool IsLocalTimestampMillis() const;
  bool IsLocalTimestampMicros() const;
  bool IsLocalTimestampNanos() const;
  bool IsDuration() const;

  // Returns the type name of the value, e.g. "record" or "int".
  std::string TypeName() const;

  // Returns true if this value conforms to the given schema.
  bool Validate(const AvroSchema& schema) const;

  // Schema resolution: adapts this value to the given schema (promotions,
  // union branch selection, defaults).
  absl::StatusOr<AvroValue> Resolve(const AvroSchema& schema) const;

  // Converts this value to a JSON string.
  absl::StatusOr<std::string> ToJsonString() const;

  bool operator==(const AvroValue& other) const;
  bool operator!=(const AvroValue& other) const;

 private:
  friend class DataFileWriter;
  friend class DataFileReader;
  friend absl::StatusOr<std::string> EncodeDatum(const AvroSchema& schema,
                                                 const AvroValue& value);
  friend absl::StatusOr<std::string> EncodeDatumSchemata(
      const AvroSchema& schema, absl::Span<const AvroSchema> schemata,
      const AvroValue& value);
  friend absl::StatusOr<AvroValue> DecodeDatum(const AvroSchema& writer_schema,
                                               absl::string_view data);
  friend absl::StatusOr<AvroValue> DecodeDatumResolved(
      const AvroSchema& writer_schema, const AvroSchema& reader_schema,
      absl::string_view data);
  friend absl::StatusOr<AvroValue> DecodeDatumSchemata(
      const AvroSchema& writer_schema,
      absl::Span<const AvroSchema> writer_schemata, absl::string_view data);

  explicit AvroValue(rust::value::AvroValue value);

  rust::value::AvroValue value_;
};

// Single-datum binary encoding without container-file framing. Replaces
// avrocpp's raw binary Encoder/Decoder usage.
absl::StatusOr<std::string> EncodeDatum(const AvroSchema& schema,
                                        const AvroValue& value);
absl::StatusOr<std::string> EncodeDatumSchemata(
    const AvroSchema& schema, absl::Span<const AvroSchema> schemata,
    const AvroValue& value);
absl::StatusOr<AvroValue> DecodeDatum(const AvroSchema& writer_schema,
                                      absl::string_view data);
absl::StatusOr<AvroValue> DecodeDatumResolved(const AvroSchema& writer_schema,
                                              const AvroSchema& reader_schema,
                                              absl::string_view data);
absl::StatusOr<AvroValue> DecodeDatumSchemata(
    const AvroSchema& writer_schema,
    absl::Span<const AvroSchema> writer_schemata, absl::string_view data);

// Bounds decoder value/length-prefix allocations on untrusted input. This
// is a process-global that can only be set once, and must be set before the
// first decode OR container-file read (the reader also consults it). Later
// calls have no effect and return the value already in effect; call at
// process startup or the 512 MiB default is locked in. Does NOT bound
// decompression of compressed container codecs (see DataFileReader).
size_t SetMaxAllocationBytes(size_t num_bytes);

// Streaming Avro object container file writer. Replaces avrocpp's
// DataFileWriter<GenericDatum>.
//
// Append validates and buffers values; every 1024 values the batch is
// encoded into ~16 KiB container blocks. Drain the encoded bytes
// incrementally with TakeBytes and complete the file with Finish (which
// encodes any remaining values and consumes the writer). The concatenation
// of every TakeBytes result followed by the Finish result is a complete,
// valid container file. Peak memory is one batch, never the whole file.
//
// Small-file usage: Append everything, then Finish once for all the bytes.
class DataFileWriter final {
 public:
  DataFileWriter(DataFileWriter&&) noexcept = default;
  DataFileWriter& operator=(DataFileWriter&&) noexcept = default;
  DataFileWriter(const DataFileWriter&) = delete;
  DataFileWriter& operator=(const DataFileWriter&) = delete;

  // Creates a writer for a self-contained schema. Fails for schemas that
  // reference named types defined in other schemas (as returned by
  // ParseList): container file headers embed only the writer schema, so
  // such a file could never be read back. Inline the referenced types in a
  // single schema document instead; for raw datums, EncodeDatumSchemata
  // supports cross-referencing schemas.
  static absl::StatusOr<DataFileWriter> Create(const AvroSchema& schema,
                                               Codec codec);

  // Returns the schema this writer was created with.
  AvroSchema Schema() const;

  // Validates `value` against the writer schema and buffers a copy of it; a
  // full batch is encoded into container blocks. A rejected value leaves the
  // writer usable; an encoding failure tears the stream and consumes the
  // writer. Fails after Finish.
  //
  // Buffering copies the whole value tree, which is a meaningful share of
  // the cost of appending (roughly a third for a record holding an
  // eight-item array and a four-key map). Callers that build a value and
  // hand it straight to the writer should move it instead; see below.
  absl::Status Append(const AvroValue& value);

  // Append, but moves the value into the writer rather than copying it.
  // `value` is left null. A rejected value is not consumed, so it can still
  // be inspected after an error.
  absl::Status Append(AvroValue&& value);

  // Drains the encoded bytes produced so far (header plus completed blocks)
  // without forcing a partial block, so may return "". Fails after Finish.
  absl::StatusOr<std::string> TakeBytes();

  // Encodes any remaining buffered values, returns all undrained bytes, and
  // consumes the writer. Further calls fail. With no appended values the
  // result is a valid header-only file.
  absl::StatusOr<std::string> Finish();

  // Convenience for whole files: Finish() and stream the result to `path`.
  // Fails with kFailedPrecondition if TakeBytes ever returned bytes (the
  // file on disk would silently be missing them; concatenate the taken
  // bytes with Finish() yourself instead). Consumes the writer on success.
  absl::Status FinishToPath(absl::string_view path);

  // True once Finish has consumed the writer (or a failed flush tore the
  // stream).
  bool IsFinished() const;

 private:
  explicit DataFileWriter(rust::container::DataFileWriter writer);

  rust::container::DataFileWriter writer_;
  // True once TakeBytes has drained bytes the file cannot be complete
  // without; guards FinishToPath against writing a truncated file.
  bool bytes_taken_ = false;
};

// Streaming Avro object container file reader (push parser). Replaces
// avrocpp's DataFileReader<GenericDatum>.
//
// Feed input bytes as they arrive with Feed (chunk boundaries are
// arbitrary; mid-token splits are fine), declare end of input with
// CloseInput, and drain decoded values with NextReady/NextValue. The
// FromBytes/FromPath constructors are conveniences that feed a complete
// file at once. Peak memory is one container block plus one decoded value;
// the whole file is never required in memory.
//
// Any framing or decode error is fatal and fuses the reader (every later
// call returns the same error), so a torn stream can never silently
// truncate. There is no Count or Rewind: a stream's length is unknown until
// consumed, and a consumed stream cannot be rewound.
class DataFileReader final {
 public:
  DataFileReader(DataFileReader&&) noexcept = default;
  DataFileReader& operator=(DataFileReader&&) noexcept = default;
  DataFileReader(const DataFileReader&) = delete;
  DataFileReader& operator=(const DataFileReader&) = delete;

  // Creates an empty reader; the writer schema is read from the stream
  // header once enough bytes have been fed.
  static DataFileReader Create();

  // As Create, but additionally resolves every value to `reader_schema`
  // (schema evolution).
  static DataFileReader CreateWithReaderSchema(const AvroSchema& reader_schema);

  // Conveniences over a complete in-memory file: feed everything, close the
  // input, and fail eagerly on garbage or a file truncated in the header or
  // first block.
  static absl::StatusOr<DataFileReader> FromBytes(absl::string_view data);
  static absl::StatusOr<DataFileReader> FromBytesWithSchema(
      const AvroSchema& reader_schema, absl::string_view data);

  // Filesystem variants of the two constructors above.
  static absl::StatusOr<DataFileReader> FromPath(absl::string_view path);
  static absl::StatusOr<DataFileReader> FromPathWithSchema(
      const AvroSchema& reader_schema, absl::string_view path);

  // Appends input bytes. Fails after CloseInput or a fatal error.
  absl::Status Feed(absl::string_view data);

  // Declares end of input: data ending mid-header or mid-block then
  // surfaces as an error from NextReady/NextValue instead of waiting for
  // more bytes forever. Idempotent.
  absl::Status CloseInput();

  // Drives the parser as far as the fed bytes allow. Returns true iff
  // NextValue would return a value right now; false means more input is
  // needed or the file ended cleanly (disambiguate with AtEnd).
  absl::StatusOr<bool> NextReady();

  // Returns the next decoded value. Fails with kOutOfRange when no value is
  // ready (benign: more input may arrive, or the file ended cleanly) and
  // with kInvalidArgument for fatal framing/decode errors, which fuse the
  // reader (see HasFailed).
  absl::StatusOr<AvroValue> NextValue();

  // True once the file ended cleanly: header parsed, input closed and fully
  // consumed, all values drained, no error. Reflects parse progress as of
  // the last NextReady/NextValue call.
  bool AtEnd() const;

  // True once a fatal error has fused the reader: every subsequent call
  // fails with the same error, and AtEnd never reports a clean end.
  bool HasFailed() const;

  // True once the header has been parsed and WriterSchema is available.
  bool HeaderReady() const;

  // The schema the file was written with. Fails until enough input has been
  // fed and parsed (drive with NextReady).
  absl::StatusOr<AvroSchema> WriterSchema() const;

  // Adjusts the cap on a block's declared compressed size (default 128 MiB).
  // Applies to blocks parsed after the call. Note: this bounds the
  // compressed size only; see the codec decompression caveat on Codec.
  absl::Status SetMaxBlockSize(uint64_t bytes);

 private:
  explicit DataFileReader(rust::container::DataFileReader reader);

  rust::container::DataFileReader reader_;
};

// Injectable chunked input source for DataFileStreamReader. Mirrors the
// shape of google::protobuf::io::ZeroCopyInputStream so existing
// implementations adapt via StreamAdapter, without this binding depending
// on protobuf. Chunks may be discontiguous memory of any size (gRPC buffer
// chains, absl::Cord fragments); nothing is flattened at the boundary.
//
// Example implementation over an absl::Cord (until a dedicated adapter
// ships in this binding):
//
//   class CordStream final : public security::avro::ZeroCopyInputStream {
//    public:
//     explicit CordStream(const absl::Cord& cord)
//         : chunk_(cord.chunk_begin()), end_(cord.chunk_end()) {}
//     bool Next(const void** data, int* size) override {
//       if (chunk_ == end_) return false;
//       *data = chunk_->data();
//       *size = static_cast<int>(chunk_->size());
//       byte_count_ += chunk_->size();
//       ++chunk_;
//       return true;
//     }
//     void BackUp(int count) override {}  // never called by the reader
//     int64_t ByteCount() const override { return byte_count_; }
//    private:
//     absl::Cord::ChunkIterator chunk_, end_;
//     int64_t byte_count_ = 0;
//   };
class ZeroCopyInputStream {
 public:
  virtual ~ZeroCopyInputStream() = default;

  // Yields the next chunk, or returns false at end of stream. The chunk
  // must stay valid until the next call on this stream. Returning true
  // with a zero-size chunk is allowed and skipped, but an unbounded run of
  // them is treated as a stream protocol violation and fails the read
  // (this bounds the reader against livelocking on a stuck stream).
  virtual bool Next(const void** data, int* size) = 0;

  // Returns the trailing `count` bytes of the last Next() chunk to the
  // stream. Present for protobuf shape parity only: an Avro container file
  // has no in-band end marker, so it always extends to the end of the
  // stream and DataFileStreamReader consumes every chunk whole, never
  // calling BackUp.
  virtual void BackUp(int count) = 0;

  // Total bytes yielded so far.
  virtual int64_t ByteCount() const = 0;
};

// Header-only adapter for any type with the same Next/BackUp/ByteCount
// shape, e.g. google::protobuf::io::ZeroCopyInputStream itself. Instantiate
// it in user code where the wrapped type is available; the binding takes no
// dependency on that type. The wrapped stream is borrowed, not owned.
template <typename S>
class StreamAdapter final : public ZeroCopyInputStream {
 public:
  explicit StreamAdapter(S* stream) : stream_(stream) {}
  bool Next(const void** data, int* size) override {
    return stream_->Next(data, size);
  }
  void BackUp(int count) override { stream_->BackUp(count); }
  int64_t ByteCount() const override { return stream_->ByteCount(); }

 private:
  S* stream_;
};

// Pull-style reader that decodes one Avro object container file arriving
// through a ZeroCopyInputStream (e.g. gRPC streaming responses). A thin
// driver over the push DataFileReader: HasNext/NextValue pull chunks from
// the stream exactly as needed, so peak memory stays one block plus one
// value regardless of file size, and call sites never touch
// Feed/NextReady/CloseInput.
//
// Blocking: HasNext/NextValue block whenever the stream's Next() blocks
// (e.g. a synchronous gRPC read). Callers that must not block a thread
// should drive the push DataFileReader directly instead.
//
// The stream is borrowed and must outlive the reader. End of stream defines
// end of file (the container format has no in-band end marker), so the
// stream must contain exactly one container file; bound it externally if
// the transport carries trailing data.
class DataFileStreamReader final {
 public:
  DataFileStreamReader(DataFileStreamReader&&) noexcept = default;
  DataFileStreamReader& operator=(DataFileStreamReader&&) noexcept = default;
  DataFileStreamReader(const DataFileStreamReader&) = delete;
  DataFileStreamReader& operator=(const DataFileStreamReader&) = delete;

  // Creates a reader over `stream`, pulling chunks until the header is
  // parsed, so garbage and schema errors surface here rather than on the
  // first value. Data truncated after the header surfaces on the first
  // HasNext/NextValue.
  static absl::StatusOr<DataFileStreamReader> Create(
      ZeroCopyInputStream* stream);

  // As Create, but additionally resolves every value to `reader_schema`
  // (schema evolution).
  static absl::StatusOr<DataFileStreamReader> CreateWithReaderSchema(
      const AvroSchema& reader_schema, ZeroCopyInputStream* stream);

  // True if another value is available, pulling stream chunks as needed;
  // false at the clean end of the file. Framing/decode errors are fatal and
  // fuse the reader, like the push DataFileReader's.
  absl::StatusOr<bool> HasNext();

  // Returns the next decoded value, pulling stream chunks as needed. Fails
  // with kOutOfRange at the clean end of the file and kInvalidArgument for
  // fatal framing/decode errors.
  absl::StatusOr<AvroValue> NextValue();

  // The schema the file was written with (parsed during Create).
  absl::StatusOr<AvroSchema> WriterSchema() const;

  // See DataFileReader::SetMaxBlockSize. Call right after Create to apply
  // from the first block onward; Create stops pulling once the header is
  // parsed, but any block framing that arrived in the same chunks as the
  // header has already been parsed under the default cap. Callers needing
  // a strict cap on every block should drive the push DataFileReader
  // (whose Create parses nothing) directly.
  absl::Status SetMaxBlockSize(uint64_t bytes);

 private:
  DataFileStreamReader(DataFileReader reader, ZeroCopyInputStream* stream);

  static absl::StatusOr<DataFileStreamReader> Build(
      DataFileReader reader, ZeroCopyInputStream* stream);

  // How far PumpUntil drives the parser: to a parsed header (Create) or to
  // a decodable value / clean end (everything else).
  enum PumpGoal { kHeaderReady, kValueReady };

  // Pulls chunks and drives the parser until the goal is reached, the file
  // ends cleanly, or an error occurs.
  absl::Status PumpUntil(PumpGoal goal);
  absl::Status Pump();

  DataFileReader reader_;
  ZeroCopyInputStream* stream_;
  bool stream_done_ = false;
};

}  // namespace security::avro

#endif  // SECURITY_AVRO_AVRO_BRIDGE_H_
