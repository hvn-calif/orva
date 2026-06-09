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

}  // namespace security::avro

#endif  // SECURITY_AVRO_AVRO_BRIDGE_H_
