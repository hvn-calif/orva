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
class StreamingDataFileWriter;
class StreamingDataFileReader;

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
  friend class StreamingDataFileWriter;
  friend class StreamingDataFileReader;
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

  // Converts this value to a JSON string. Fails for a non-finite float or
  // double, which JSON cannot represent.
  absl::StatusOr<std::string> ToJsonString() const;

  // Equality is IEEE-754 equality for float and double, not bit equality, so
  // it is the wrong check for a round-trip: two NaNs compare unequal though
  // their bits match, and -0.0 compares equal to 0.0 though theirs do not.
  // Avro carries raw IEEE bits, so compare the output of EncodeDatum when what
  // you mean is "the same value came back".
  bool operator==(const AvroValue& other) const;
  bool operator!=(const AvroValue& other) const;

 private:
  friend class DataFileWriter;
  friend class DataFileReader;
  friend class StreamingDataFileWriter;
  friend class StreamingDataFileReader;
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

// When on, a `string` whose wire bytes are not valid UTF-8 decodes as bytes
// instead of failing, matching what avrocpp does: it copies the bytes into a
// byte-oriented std::string without validating them, so files carrying such
// bytes round-trip through it and rejecting them here would make that data
// unreadable. The bytes come back in a distinct variant, so re-encoding writes
// back exactly what was read.
//
// On is the default. Pass false to get apache-avro's stricter reading, where
// such a `string` is an error. Process-global and first-call-wins like
// SetMaxAllocationBytes, and read on the first string decode, so call it before
// decoding anything. Returns the setting actually in effect.
bool SetNonUtf8StringAsBytes(bool as_bytes);

// When on, a `uuid` decodes as an ordinary string rather than being parsed, so
// the bytes as written survive. Avro defines `uuid` as an annotation on
// `string` and a reader may leave it uninterpreted, which is what avrocpp does:
// it never parses or validates one. Parsing has three effects reading it as a
// string does not -- the bytes are rewritten into canonical form, any 16-byte
// string is reinterpreted as a raw uuid, and text that is not a uuid is
// rejected, making data other implementations wrote unreadable.
//
// On is the default. Pass false to get the parsing behaviour back.
// Process-global and first-call-wins; call before decoding anything.
bool SetUuidAsString(bool as_string);

// When on, DecodeDatum and its variants require the buffer to hold exactly one
// datum. Off by default, which is what avrocpp does: it stops at the end of the
// first datum and ignores what follows, so a caller migrating off avrocpp that
// passes a padded or over-allocated buffer keeps working.
//
// Turn it on to be stricter than avrocpp. Leftover bytes usually mean framing
// has gone wrong, and a caller decoding concatenated datums in a loop would
// otherwise stop after the first and report success.
//
// This governs bytes left over after a complete datum, not bytes missing from
// one: a truncated datum is an error either way. Process-global and
// first-call-wins like SetMaxAllocationBytes, read on the first decode, so call
// it before decoding anything. Returns the setting actually in effect.
//
// Both values are covered by DatumTest.TrailingBytesFollowTheSetting, which is
// built twice; see the CMake comment above avro_bridge_strict_test.
bool SetRejectTrailingBytes(bool reject);

// When on, AvroSchema::Parse and AvroSchema::ParseList require the input to be
// one JSON document and nothing else. Off by default, which is what avrocpp
// does: its JSON reader stops once it has one complete value and never looks at
// what follows, so `"int"` followed by anything is still the schema `"int"`.
//
// Off matters more here than the size of the difference suggests. Two of the
// three ways such an input used to be refused happened before any JSON parsing,
// because the whole buffer is validated as UTF-8 first: bytes after a perfectly
// good schema were enough to lose it. And a schema is a cache key and a
// fingerprint input, so a producer on avrocpp can write a document with a NUL
// pad, a stray byte or a second document behind it that the bridge would not
// load at all. Trailing *whitespace* was never at issue: serde_json accepts it
// under either setting.
//
// Turn it on to be stricter than avrocpp: text after a complete document
// usually means a truncated write or two documents concatenated by mistake.
//
// This governs text after a complete document, not a document cut short: a
// truncated schema is an error either way. Process-global and first-call-wins
// like SetMaxAllocationBytes, read on the first parse, so call it before parsing
// anything. Returns the setting actually in effect.
//
// Both values are covered by
// AvroSchemaTest.TextAfterSchemaJsonFollowsTheSetting, which is built twice;
// see the CMake comment above avro_bridge_strict_test.
bool SetRejectTextAfterSchemaJson(bool reject);

// Writes Avro object container files. Replaces avrocpp's
// DataFileWriter<GenericDatum>. Values are validated and buffered by
// Append; the encoded file is produced by ToBytes or WriteToPath.
class DataFileWriter final {
 public:
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

  // Validates `value` against the writer schema and buffers it.
  absl::Status Append(const AvroValue& value);

  // Returns the number of buffered values.
  size_t Count() const;

  // Encodes all buffered values into a complete container file.
  absl::StatusOr<std::string> ToBytes() const;

  // Encodes all buffered values and writes the container file to `path`.
  absl::Status WriteToPath(absl::string_view path) const;

 private:
  explicit DataFileWriter(rust::container::DataFileWriter writer);

  rust::container::DataFileWriter writer_;
};

// Reads Avro object container files. Replaces avrocpp's
// DataFileReader<GenericDatum>. The whole file is decoded eagerly at
// construction; values are consumed with HasNext / NextValue.
class DataFileReader final {
 public:
  // Opens a container file from a byte buffer; the writer schema is read
  // from the file header.
  static absl::StatusOr<DataFileReader> FromBytes(absl::string_view data);

  // Opens a container file and resolves every value to `reader_schema`.
  static absl::StatusOr<DataFileReader> FromBytesWithSchema(
      const AvroSchema& reader_schema, absl::string_view data);

  // Filesystem variants of the two constructors above.
  static absl::StatusOr<DataFileReader> FromPath(absl::string_view path);
  static absl::StatusOr<DataFileReader> FromPathWithSchema(
      const AvroSchema& reader_schema, absl::string_view path);

  // Returns the schema the file was written with.
  AvroSchema WriterSchema() const;

  // Returns the total number of values in the file.
  size_t Count() const;

  // Iteration. NextValue fails with kOutOfRange after the last value.
  bool HasNext() const;
  absl::StatusOr<AvroValue> NextValue();
  void Rewind();

 private:
  explicit DataFileReader(rust::container::DataFileReader reader);

  rust::container::DataFileReader reader_;
};

// Streaming object container file writer. Unlike DataFileWriter, values are
// not all buffered: Append encodes into a ~16 KB block buffer that
// auto-flushes full blocks into an internal buffer. Drain the encoded bytes
// incrementally with TakeBytes (full blocks only) and finish with Finish
// (flushes the final partial block). The concatenation of every TakeBytes
// result followed by the Finish result is a complete, valid container file.
// Use this to bound peak memory when writing large files; use DataFileWriter
// when you want the whole file from one ToBytes call.
class StreamingDataFileWriter final {
 public:
  StreamingDataFileWriter(StreamingDataFileWriter&&) noexcept = default;
  StreamingDataFileWriter& operator=(StreamingDataFileWriter&&) noexcept =
      default;
  StreamingDataFileWriter(const StreamingDataFileWriter&) = delete;
  StreamingDataFileWriter& operator=(const StreamingDataFileWriter&) = delete;

  static absl::StatusOr<StreamingDataFileWriter> Create(const AvroSchema& schema,
                                                        Codec codec);

  // The schema the writer was created with. Fails after Finish.
  absl::StatusOr<AvroSchema> Schema() const;

  // Validates and encodes `value`; may auto-flush a full block internally.
  // Fails after Finish.
  absl::Status Append(const AvroValue& value);

  // Returns the bytes already flushed to the internal buffer (header plus any
  // full blocks), draining them. Does not force a partial flush, so may
  // return "". Fails after Finish.
  absl::StatusOr<std::string> TakeBytes();

  // Flushes any pending block, returns the remaining bytes, and consumes the
  // writer. Further calls fail. (With no buffered values the result is just
  // the header plus whatever was already flushed.)
  absl::StatusOr<std::string> Finish();

  // True once Finish has consumed the writer.
  bool IsFinished() const;

 private:
  explicit StreamingDataFileWriter(rust::container::StreamingDataFileWriter writer);

  rust::container::StreamingDataFileWriter writer_;
};

// Streaming object container file reader. Unlike DataFileReader, this decodes
// one value per NextValue instead of decoding the whole file up front. Use it
// to bound peak memory when reading large files. There is no Count (unknown
// without consuming the file) and no Rewind (a consumed stream cannot be
// rewound); use DataFileReader if you need either.
class StreamingDataFileReader final {
 public:
  StreamingDataFileReader(StreamingDataFileReader&&) noexcept = default;
  StreamingDataFileReader& operator=(StreamingDataFileReader&&) noexcept =
      default;
  StreamingDataFileReader(const StreamingDataFileReader&) = delete;
  StreamingDataFileReader& operator=(const StreamingDataFileReader&) = delete;

  static absl::StatusOr<StreamingDataFileReader> FromBytes(
      absl::string_view data);
  static absl::StatusOr<StreamingDataFileReader> FromBytesWithSchema(
      const AvroSchema& reader_schema, absl::string_view data);
  static absl::StatusOr<StreamingDataFileReader> FromPath(
      absl::string_view path);
  static absl::StatusOr<StreamingDataFileReader> FromPathWithSchema(
      const AvroSchema& reader_schema, absl::string_view path);

  // The schema the file was written with.
  AvroSchema WriterSchema() const;

  // HasNext is non-const: it fills a single-value lookahead buffer.
  bool HasNext();
  // NextValue fails with kOutOfRange after the last value.
  absl::StatusOr<AvroValue> NextValue();

 private:
  explicit StreamingDataFileReader(rust::container::StreamingDataFileReader reader);

  rust::container::StreamingDataFileReader reader_;
};

}  // namespace security::avro

#endif  // SECURITY_AVRO_AVRO_BRIDGE_H_
