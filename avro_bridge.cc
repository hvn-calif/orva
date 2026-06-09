#include "avro_bridge.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "crubit/rust.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"

namespace security::avro {

namespace {

// Reinterprets a string_view as the byte span Crubit expects for `&[u8]`.
absl::Span<const uint8_t> ToByteSpan(absl::string_view data) {
  return absl::Span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

std::string FromVecU8(const rust::vec_u8::VecU8& vec) {
  return std::string(reinterpret_cast<const char*>(vec.as_ptr()), vec.len());
}

// Converts the Rust `Status` (Result<u8, VecU8>) into an absl::Status with
// the given code.
absl::Status ToStatus(rust::vec_u8::Status status,
                      absl::StatusCode code = absl::StatusCode::kInvalidArgument) {
  if (status.has_value()) {
    return absl::OkStatus();
  }
  return absl::Status(code, FromVecU8(std::move(status).err()));
}

std::vector<std::string> FromVecVecU8(const rust::value::VecVecU8& vec) {
  std::vector<std::string> result;
  result.reserve(vec.len());
  for (size_t i = 0; i < vec.len(); ++i) {
    result.push_back(FromVecU8(vec.as_ptr()[i]));
  }
  return result;
}

// Unwraps a Result<T, VecU8> into absl::StatusOr<T> for trivially
// convertible payload types.
template <typename T>
absl::StatusOr<T> Unwrap(rs_std::Result<T, rust::vec_u8::VecU8> result,
                         absl::StatusCode code) {
  if (!result.has_value()) {
    return absl::Status(code, FromVecU8(std::move(result).err()));
  }
  return std::move(result).value();
}

// Unwraps a Result<VecU8, VecU8> into absl::StatusOr<std::string>.
absl::StatusOr<std::string> UnwrapString(
    rs_std::Result<rust::vec_u8::VecU8, rust::vec_u8::VecU8> result,
    absl::StatusCode code) {
  if (!result.has_value()) {
    return absl::Status(code, FromVecU8(std::move(result).err()));
  }
  return FromVecU8(std::move(result).value());
}

constexpr absl::StatusCode kWrongType = absl::StatusCode::kFailedPrecondition;
constexpr absl::StatusCode kBadInput = absl::StatusCode::kInvalidArgument;
constexpr absl::StatusCode kNotFound = absl::StatusCode::kNotFound;

}  // namespace

// ---------------------------------------------------------------------------
// AvroSchema
// ---------------------------------------------------------------------------

AvroSchema::AvroSchema(rust::schema::AvroSchema schema)
    : schema_(std::move(schema)) {}

absl::StatusOr<AvroSchema> AvroSchema::Parse(absl::string_view json) {
  rs_std::Result<rust::schema::AvroSchema, rust::vec_u8::VecU8> result =
      rust::schema::AvroSchema::parse(ToByteSpan(json));
  if (!result.has_value()) {
    return absl::InvalidArgumentError(FromVecU8(std::move(result).err()));
  }
  return AvroSchema(std::move(result).value());
}

absl::StatusOr<std::vector<AvroSchema>> AvroSchema::ParseList(
    absl::Span<const absl::string_view> jsons) {
  std::vector<rust::vec_u8::VecU8> raw_jsons;
  raw_jsons.reserve(jsons.size());
  for (absl::string_view json : jsons) {
    raw_jsons.push_back(rust::vec_u8::VecU8::copy_from_slice(ToByteSpan(json)));
  }
  rs_std::Result<rust::schema::VecAvroSchema, rust::vec_u8::VecU8> result =
      rust::schema::AvroSchema::parse_list(raw_jsons);
  if (!result.has_value()) {
    return absl::InvalidArgumentError(FromVecU8(std::move(result).err()));
  }
  rust::schema::VecAvroSchema schemas = std::move(result).value();
  std::vector<AvroSchema> out;
  out.reserve(schemas.len());
  for (size_t i = 0; i < schemas.len(); ++i) {
    out.push_back(AvroSchema(schemas.as_ptr()[i]));
  }
  return out;
}

std::string AvroSchema::CanonicalForm() const {
  return FromVecU8(schema_.canonical_form());
}

int64_t AvroSchema::FingerprintRabin() const {
  return schema_.fingerprint_rabin();
}

std::string AvroSchema::FingerprintRabinHex() const {
  return FromVecU8(schema_.fingerprint_rabin_hex());
}

std::string AvroSchema::FingerprintMd5Hex() const {
  return FromVecU8(schema_.fingerprint_md5_hex());
}

std::string AvroSchema::FingerprintSha256Hex() const {
  return FromVecU8(schema_.fingerprint_sha256_hex());
}

absl::StatusOr<std::string> AvroSchema::Name() const {
  return UnwrapString(schema_.name(), kWrongType);
}

absl::StatusOr<std::string> AvroSchema::Namespace() const {
  return UnwrapString(schema_.namespace_name(), kWrongType);
}

absl::StatusOr<std::string> AvroSchema::FullName() const {
  return UnwrapString(schema_.full_name(), kWrongType);
}

absl::StatusOr<std::string> AvroSchema::ToJsonString() const {
  return UnwrapString(schema_.to_json_string(), absl::StatusCode::kInternal);
}

std::string AvroSchema::TypeName() const {
  return FromVecU8(schema_.type_name());
}

bool AvroSchema::IsNull() const { return schema_.is_null(); }
bool AvroSchema::IsBoolean() const { return schema_.is_boolean(); }
bool AvroSchema::IsInt() const { return schema_.is_int(); }
bool AvroSchema::IsLong() const { return schema_.is_long(); }
bool AvroSchema::IsFloat() const { return schema_.is_float(); }
bool AvroSchema::IsDouble() const { return schema_.is_double(); }
bool AvroSchema::IsBytes() const { return schema_.is_bytes(); }
bool AvroSchema::IsString() const { return schema_.is_string(); }
bool AvroSchema::IsRecord() const { return schema_.is_record(); }
bool AvroSchema::IsEnum() const { return schema_.is_enum(); }
bool AvroSchema::IsArray() const { return schema_.is_array(); }
bool AvroSchema::IsMap() const { return schema_.is_map(); }
bool AvroSchema::IsUnion() const { return schema_.is_union(); }
bool AvroSchema::IsFixed() const { return schema_.is_fixed(); }

absl::Status AvroSchema::CanReadFrom(const AvroSchema& writer) const {
  return ToStatus(schema_.can_read_from(writer.schema_),
                  absl::StatusCode::kFailedPrecondition);
}

absl::Status AvroSchema::MutualRead(const AvroSchema& other) const {
  return ToStatus(schema_.mutual_read(other.schema_),
                  absl::StatusCode::kFailedPrecondition);
}

bool AvroSchema::operator==(const AvroSchema& other) const {
  return schema_.equals(other.schema_);
}

bool AvroSchema::operator!=(const AvroSchema& other) const {
  return !schema_.equals(other.schema_);
}

// ---------------------------------------------------------------------------
// AvroValue
// ---------------------------------------------------------------------------

AvroValue::AvroValue(rust::value::AvroValue value) : value_(std::move(value)) {}

AvroValue AvroValue::CreateNull() {
  return AvroValue(rust::value::AvroValue::create_null());
}

AvroValue AvroValue::CreateBoolean(bool value) {
  return AvroValue(rust::value::AvroValue::create_boolean(value));
}

AvroValue AvroValue::CreateInt(int32_t value) {
  return AvroValue(rust::value::AvroValue::create_int(value));
}

AvroValue AvroValue::CreateLong(int64_t value) {
  return AvroValue(rust::value::AvroValue::create_long(value));
}

AvroValue AvroValue::CreateFloat(float value) {
  return AvroValue(rust::value::AvroValue::create_float(value));
}

AvroValue AvroValue::CreateDouble(double value) {
  return AvroValue(rust::value::AvroValue::create_double(value));
}

AvroValue AvroValue::CreateBytes(absl::string_view value) {
  return AvroValue(rust::value::AvroValue::create_bytes(ToByteSpan(value)));
}

absl::StatusOr<AvroValue> AvroValue::CreateString(absl::string_view value) {
  rs_std::Result<rust::value::AvroValue, rust::vec_u8::VecU8> result =
      rust::value::AvroValue::create_string(ToByteSpan(value));
  if (!result.has_value()) {
    return absl::InvalidArgumentError(FromVecU8(std::move(result).err()));
  }
  return AvroValue(std::move(result).value());
}

AvroValue AvroValue::CreateRecord() {
  return AvroValue(rust::value::AvroValue::create_record());
}

AvroValue AvroValue::CreateArray() {
  return AvroValue(rust::value::AvroValue::create_array());
}

AvroValue AvroValue::CreateMap() {
  return AvroValue(rust::value::AvroValue::create_map());
}

absl::StatusOr<AvroValue> AvroValue::CreateEnum(uint32_t position,
                                                absl::string_view symbol) {
  rs_std::Result<rust::value::AvroValue, rust::vec_u8::VecU8> result =
      rust::value::AvroValue::create_enum(position, ToByteSpan(symbol));
  if (!result.has_value()) {
    return absl::InvalidArgumentError(FromVecU8(std::move(result).err()));
  }
  return AvroValue(std::move(result).value());
}

AvroValue AvroValue::CreateFixed(absl::string_view value) {
  return AvroValue(rust::value::AvroValue::create_fixed(ToByteSpan(value)));
}

AvroValue AvroValue::CreateUnion(uint32_t branch_index,
                                 const AvroValue& value) {
  return AvroValue(
      rust::value::AvroValue::create_union(branch_index, value.value_));
}

AvroValue AvroValue::CreateDecimal(absl::string_view big_endian_bytes) {
  return AvroValue(
      rust::value::AvroValue::create_decimal(ToByteSpan(big_endian_bytes)));
}

absl::StatusOr<AvroValue> AvroValue::CreateUuid(absl::string_view value) {
  rs_std::Result<rust::value::AvroValue, rust::vec_u8::VecU8> result =
      rust::value::AvroValue::create_uuid(ToByteSpan(value));
  if (!result.has_value()) {
    return absl::InvalidArgumentError(FromVecU8(std::move(result).err()));
  }
  return AvroValue(std::move(result).value());
}

AvroValue AvroValue::CreateDate(int32_t days_since_epoch) {
  return AvroValue(rust::value::AvroValue::create_date(days_since_epoch));
}

AvroValue AvroValue::CreateTimeMillis(int32_t value) {
  return AvroValue(rust::value::AvroValue::create_time_millis(value));
}

AvroValue AvroValue::CreateTimeMicros(int64_t value) {
  return AvroValue(rust::value::AvroValue::create_time_micros(value));
}

AvroValue AvroValue::CreateTimestampMillis(int64_t value) {
  return AvroValue(rust::value::AvroValue::create_timestamp_millis(value));
}

AvroValue AvroValue::CreateTimestampMicros(int64_t value) {
  return AvroValue(rust::value::AvroValue::create_timestamp_micros(value));
}

AvroValue AvroValue::CreateTimestampNanos(int64_t value) {
  return AvroValue(rust::value::AvroValue::create_timestamp_nanos(value));
}

AvroValue AvroValue::CreateLocalTimestampMillis(int64_t value) {
  return AvroValue(
      rust::value::AvroValue::create_local_timestamp_millis(value));
}

AvroValue AvroValue::CreateLocalTimestampMicros(int64_t value) {
  return AvroValue(
      rust::value::AvroValue::create_local_timestamp_micros(value));
}

AvroValue AvroValue::CreateLocalTimestampNanos(int64_t value) {
  return AvroValue(
      rust::value::AvroValue::create_local_timestamp_nanos(value));
}

AvroValue AvroValue::CreateDuration(uint32_t months, uint32_t days,
                                    uint32_t millis) {
  return AvroValue(
      rust::value::AvroValue::create_duration(months, days, millis));
}

absl::StatusOr<bool> AvroValue::GetBoolean() const {
  return Unwrap(value_.get_boolean(), kWrongType);
}

absl::StatusOr<int32_t> AvroValue::GetInt() const {
  return Unwrap(value_.get_int(), kWrongType);
}

absl::StatusOr<int64_t> AvroValue::GetLong() const {
  return Unwrap(value_.get_long(), kWrongType);
}

absl::StatusOr<float> AvroValue::GetFloat() const {
  return Unwrap(value_.get_float(), kWrongType);
}

absl::StatusOr<double> AvroValue::GetDouble() const {
  return Unwrap(value_.get_double(), kWrongType);
}

absl::StatusOr<std::string> AvroValue::GetBytes() const {
  return UnwrapString(value_.get_bytes(), kWrongType);
}

absl::StatusOr<std::string> AvroValue::GetString() const {
  return UnwrapString(value_.get_string(), kWrongType);
}

absl::StatusOr<uint32_t> AvroValue::GetEnumPosition() const {
  return Unwrap(value_.get_enum_position(), kWrongType);
}

absl::StatusOr<std::string> AvroValue::GetEnumSymbol() const {
  return UnwrapString(value_.get_enum_symbol(), kWrongType);
}

absl::StatusOr<std::string> AvroValue::GetFixedBytes() const {
  return UnwrapString(value_.get_fixed_bytes(), kWrongType);
}

absl::StatusOr<uint32_t> AvroValue::GetUnionBranch() const {
  return Unwrap(value_.get_union_branch(), kWrongType);
}

absl::StatusOr<AvroValue> AvroValue::GetUnionValue() const {
  rs_std::Result<rust::value::AvroValue, rust::vec_u8::VecU8> result =
      value_.get_union_value();
  if (!result.has_value()) {
    return absl::Status(kWrongType, FromVecU8(std::move(result).err()));
  }
  return AvroValue(std::move(result).value());
}

absl::StatusOr<std::string> AvroValue::GetDecimalBytes() const {
  return UnwrapString(value_.get_decimal_bytes(), kWrongType);
}

absl::StatusOr<std::string> AvroValue::GetUuid() const {
  return UnwrapString(value_.get_uuid(), kWrongType);
}

absl::StatusOr<int32_t> AvroValue::GetDate() const {
  return Unwrap(value_.get_date(), kWrongType);
}

absl::StatusOr<int32_t> AvroValue::GetTimeMillis() const {
  return Unwrap(value_.get_time_millis(), kWrongType);
}

absl::StatusOr<int64_t> AvroValue::GetTimeMicros() const {
  return Unwrap(value_.get_time_micros(), kWrongType);
}

absl::StatusOr<int64_t> AvroValue::GetTimestampMillis() const {
  return Unwrap(value_.get_timestamp_millis(), kWrongType);
}

absl::StatusOr<int64_t> AvroValue::GetTimestampMicros() const {
  return Unwrap(value_.get_timestamp_micros(), kWrongType);
}

absl::StatusOr<int64_t> AvroValue::GetTimestampNanos() const {
  return Unwrap(value_.get_timestamp_nanos(), kWrongType);
}

absl::StatusOr<int64_t> AvroValue::GetLocalTimestampMillis() const {
  return Unwrap(value_.get_local_timestamp_millis(), kWrongType);
}

absl::StatusOr<int64_t> AvroValue::GetLocalTimestampMicros() const {
  return Unwrap(value_.get_local_timestamp_micros(), kWrongType);
}

absl::StatusOr<int64_t> AvroValue::GetLocalTimestampNanos() const {
  return Unwrap(value_.get_local_timestamp_nanos(), kWrongType);
}

absl::StatusOr<uint32_t> AvroValue::GetDurationMonths() const {
  return Unwrap(value_.get_duration_months(), kWrongType);
}

absl::StatusOr<uint32_t> AvroValue::GetDurationDays() const {
  return Unwrap(value_.get_duration_days(), kWrongType);
}

absl::StatusOr<uint32_t> AvroValue::GetDurationMillis() const {
  return Unwrap(value_.get_duration_millis(), kWrongType);
}

absl::StatusOr<size_t> AvroValue::GetArrayLen() const {
  return Unwrap(value_.get_array_len(), kWrongType);
}

absl::StatusOr<AvroValue> AvroValue::GetArrayItem(size_t index) const {
  rs_std::Result<rust::value::AvroValue, rust::vec_u8::VecU8> result =
      value_.get_array_item(index);
  if (!result.has_value()) {
    return absl::Status(kNotFound, FromVecU8(std::move(result).err()));
  }
  return AvroValue(std::move(result).value());
}

absl::StatusOr<size_t> AvroValue::GetMapLen() const {
  return Unwrap(value_.get_map_len(), kWrongType);
}

absl::StatusOr<std::vector<std::string>> AvroValue::GetMapKeys() const {
  rs_std::Result<rust::value::VecVecU8, rust::vec_u8::VecU8> result =
      value_.get_map_keys();
  if (!result.has_value()) {
    return absl::Status(kWrongType, FromVecU8(std::move(result).err()));
  }
  return FromVecVecU8(std::move(result).value());
}

absl::StatusOr<AvroValue> AvroValue::GetMapValue(absl::string_view key) const {
  rs_std::Result<rust::value::AvroValue, rust::vec_u8::VecU8> result =
      value_.get_map_value(ToByteSpan(key));
  if (!result.has_value()) {
    return absl::Status(kNotFound, FromVecU8(std::move(result).err()));
  }
  return AvroValue(std::move(result).value());
}

absl::StatusOr<bool> AvroValue::HasMapKey(absl::string_view key) const {
  return Unwrap(value_.has_map_key(ToByteSpan(key)), kWrongType);
}

absl::StatusOr<size_t> AvroValue::GetRecordLen() const {
  return Unwrap(value_.get_record_len(), kWrongType);
}

absl::StatusOr<std::vector<std::string>> AvroValue::GetRecordFieldNames()
    const {
  rs_std::Result<rust::value::VecVecU8, rust::vec_u8::VecU8> result =
      value_.get_record_field_names();
  if (!result.has_value()) {
    return absl::Status(kWrongType, FromVecU8(std::move(result).err()));
  }
  return FromVecVecU8(std::move(result).value());
}

absl::StatusOr<AvroValue> AvroValue::GetRecordField(
    absl::string_view name) const {
  rs_std::Result<rust::value::AvroValue, rust::vec_u8::VecU8> result =
      value_.get_record_field(ToByteSpan(name));
  if (!result.has_value()) {
    return absl::Status(kNotFound, FromVecU8(std::move(result).err()));
  }
  return AvroValue(std::move(result).value());
}

absl::StatusOr<bool> AvroValue::HasRecordField(absl::string_view name) const {
  return Unwrap(value_.has_record_field(ToByteSpan(name)), kWrongType);
}

absl::Status AvroValue::RecordPut(absl::string_view name,
                                  const AvroValue& value) {
  return ToStatus(value_.record_put(ToByteSpan(name), value.value_),
                  kWrongType);
}

absl::Status AvroValue::ArrayPush(const AvroValue& value) {
  return ToStatus(value_.array_push(value.value_), kWrongType);
}

absl::Status AvroValue::MapPut(absl::string_view key, const AvroValue& value) {
  return ToStatus(value_.map_put(ToByteSpan(key), value.value_), kWrongType);
}

bool AvroValue::IsNull() const { return value_.is_null(); }
bool AvroValue::IsBoolean() const { return value_.is_boolean(); }
bool AvroValue::IsInt() const { return value_.is_int(); }
bool AvroValue::IsLong() const { return value_.is_long(); }
bool AvroValue::IsFloat() const { return value_.is_float(); }
bool AvroValue::IsDouble() const { return value_.is_double(); }
bool AvroValue::IsBytes() const { return value_.is_bytes(); }
bool AvroValue::IsString() const { return value_.is_string(); }
bool AvroValue::IsRecord() const { return value_.is_record(); }
bool AvroValue::IsEnum() const { return value_.is_enum(); }
bool AvroValue::IsArray() const { return value_.is_array(); }
bool AvroValue::IsMap() const { return value_.is_map(); }
bool AvroValue::IsUnion() const { return value_.is_union(); }
bool AvroValue::IsFixed() const { return value_.is_fixed(); }
bool AvroValue::IsDecimal() const { return value_.is_decimal(); }
bool AvroValue::IsUuid() const { return value_.is_uuid(); }
bool AvroValue::IsDate() const { return value_.is_date(); }
bool AvroValue::IsTimeMillis() const { return value_.is_time_millis(); }
bool AvroValue::IsTimeMicros() const { return value_.is_time_micros(); }
bool AvroValue::IsTimestampMillis() const {
  return value_.is_timestamp_millis();
}
bool AvroValue::IsTimestampMicros() const {
  return value_.is_timestamp_micros();
}
bool AvroValue::IsTimestampNanos() const {
  return value_.is_timestamp_nanos();
}
bool AvroValue::IsLocalTimestampMillis() const {
  return value_.is_local_timestamp_millis();
}
bool AvroValue::IsLocalTimestampMicros() const {
  return value_.is_local_timestamp_micros();
}
bool AvroValue::IsLocalTimestampNanos() const {
  return value_.is_local_timestamp_nanos();
}
bool AvroValue::IsDuration() const { return value_.is_duration(); }

std::string AvroValue::TypeName() const {
  return FromVecU8(value_.type_name());
}

bool AvroValue::Validate(const AvroSchema& schema) const {
  return value_.validate(schema.schema_);
}

absl::StatusOr<AvroValue> AvroValue::Resolve(const AvroSchema& schema) const {
  rs_std::Result<rust::value::AvroValue, rust::vec_u8::VecU8> result =
      value_.resolve(schema.schema_);
  if (!result.has_value()) {
    return absl::Status(kWrongType, FromVecU8(std::move(result).err()));
  }
  return AvroValue(std::move(result).value());
}

absl::StatusOr<std::string> AvroValue::ToJsonString() const {
  return UnwrapString(value_.to_json_string(), absl::StatusCode::kInternal);
}

bool AvroValue::operator==(const AvroValue& other) const {
  return value_.equals(other.value_);
}

bool AvroValue::operator!=(const AvroValue& other) const {
  return !value_.equals(other.value_);
}

// ---------------------------------------------------------------------------
// Single-datum encode/decode
// ---------------------------------------------------------------------------

absl::StatusOr<std::string> EncodeDatum(const AvroSchema& schema,
                                        const AvroValue& value) {
  return UnwrapString(rust::datum::encode_datum(schema.schema_, value.value_),
                      kBadInput);
}

absl::StatusOr<std::string> EncodeDatumSchemata(
    const AvroSchema& schema, absl::Span<const AvroSchema> schemata,
    const AvroValue& value) {
  std::vector<rust::schema::AvroSchema> raw_schemata;
  raw_schemata.reserve(schemata.size());
  for (const AvroSchema& s : schemata) {
    raw_schemata.push_back(s.schema_);
  }
  return UnwrapString(rust::datum::encode_datum_schemata(
                          schema.schema_, raw_schemata, value.value_),
                      kBadInput);
}

absl::StatusOr<AvroValue> DecodeDatum(const AvroSchema& writer_schema,
                                      absl::string_view data) {
  rs_std::Result<rust::value::AvroValue, rust::vec_u8::VecU8> result =
      rust::datum::decode_datum(writer_schema.schema_, ToByteSpan(data));
  if (!result.has_value()) {
    return absl::InvalidArgumentError(FromVecU8(std::move(result).err()));
  }
  return AvroValue(std::move(result).value());
}

absl::StatusOr<AvroValue> DecodeDatumResolved(const AvroSchema& writer_schema,
                                              const AvroSchema& reader_schema,
                                              absl::string_view data) {
  rs_std::Result<rust::value::AvroValue, rust::vec_u8::VecU8> result =
      rust::datum::decode_datum_resolved(writer_schema.schema_,
                                         reader_schema.schema_,
                                         ToByteSpan(data));
  if (!result.has_value()) {
    return absl::InvalidArgumentError(FromVecU8(std::move(result).err()));
  }
  return AvroValue(std::move(result).value());
}

absl::StatusOr<AvroValue> DecodeDatumSchemata(
    const AvroSchema& writer_schema,
    absl::Span<const AvroSchema> writer_schemata, absl::string_view data) {
  std::vector<rust::schema::AvroSchema> raw_schemata;
  raw_schemata.reserve(writer_schemata.size());
  for (const AvroSchema& s : writer_schemata) {
    raw_schemata.push_back(s.schema_);
  }
  rs_std::Result<rust::value::AvroValue, rust::vec_u8::VecU8> result =
      rust::datum::decode_datum_schemata(writer_schema.schema_, raw_schemata,
                                         ToByteSpan(data));
  if (!result.has_value()) {
    return absl::InvalidArgumentError(FromVecU8(std::move(result).err()));
  }
  return AvroValue(std::move(result).value());
}

size_t SetMaxAllocationBytes(size_t num_bytes) {
  return rust::datum::set_max_allocation_bytes(num_bytes);
}

// ---------------------------------------------------------------------------
// DataFileWriter
// ---------------------------------------------------------------------------

DataFileWriter::DataFileWriter(rust::container::DataFileWriter writer)
    : writer_(std::move(writer)) {}

absl::StatusOr<DataFileWriter> DataFileWriter::Create(const AvroSchema& schema,
                                                      Codec codec) {
  rs_std::Result<rust::container::AvroCodec, rust::vec_u8::VecU8> rust_codec =
      rust::container::AvroCodec::from_i32(static_cast<int32_t>(codec));
  if (!rust_codec.has_value()) {
    return absl::InvalidArgumentError(FromVecU8(std::move(rust_codec).err()));
  }
  rs_std::Result<rust::container::DataFileWriter, rust::vec_u8::VecU8>
      result = rust::container::DataFileWriter::create(
          schema.schema_, std::move(rust_codec).value());
  if (!result.has_value()) {
    return absl::InvalidArgumentError(FromVecU8(std::move(result).err()));
  }
  return DataFileWriter(std::move(result).value());
}

AvroSchema DataFileWriter::Schema() const {
  return AvroSchema(writer_.schema());
}

absl::Status DataFileWriter::Append(const AvroValue& value) {
  return ToStatus(writer_.append(value.value_), kBadInput);
}

size_t DataFileWriter::Count() const { return writer_.count(); }

absl::StatusOr<std::string> DataFileWriter::ToBytes() const {
  return UnwrapString(writer_.to_bytes(), absl::StatusCode::kInternal);
}

absl::Status DataFileWriter::WriteToPath(absl::string_view path) const {
  return ToStatus(writer_.write_to_path(ToByteSpan(path)),
                  absl::StatusCode::kInternal);
}

// ---------------------------------------------------------------------------
// DataFileReader
// ---------------------------------------------------------------------------

DataFileReader::DataFileReader(rust::container::DataFileReader reader)
    : reader_(std::move(reader)) {}

absl::StatusOr<DataFileReader> DataFileReader::FromBytes(
    absl::string_view data) {
  rs_std::Result<rust::container::DataFileReader, rust::vec_u8::VecU8> result =
      rust::container::DataFileReader::from_bytes(ToByteSpan(data));
  if (!result.has_value()) {
    return absl::InvalidArgumentError(FromVecU8(std::move(result).err()));
  }
  return DataFileReader(std::move(result).value());
}

absl::StatusOr<DataFileReader> DataFileReader::FromBytesWithSchema(
    const AvroSchema& reader_schema, absl::string_view data) {
  rs_std::Result<rust::container::DataFileReader, rust::vec_u8::VecU8> result =
      rust::container::DataFileReader::from_bytes_with_schema(
          reader_schema.schema_, ToByteSpan(data));
  if (!result.has_value()) {
    return absl::InvalidArgumentError(FromVecU8(std::move(result).err()));
  }
  return DataFileReader(std::move(result).value());
}

absl::StatusOr<DataFileReader> DataFileReader::FromPath(
    absl::string_view path) {
  rs_std::Result<rust::container::DataFileReader, rust::vec_u8::VecU8> result =
      rust::container::DataFileReader::from_path(ToByteSpan(path));
  if (!result.has_value()) {
    return absl::InvalidArgumentError(FromVecU8(std::move(result).err()));
  }
  return DataFileReader(std::move(result).value());
}

absl::StatusOr<DataFileReader> DataFileReader::FromPathWithSchema(
    const AvroSchema& reader_schema, absl::string_view path) {
  rs_std::Result<rust::container::DataFileReader, rust::vec_u8::VecU8> result =
      rust::container::DataFileReader::from_path_with_schema(
          reader_schema.schema_, ToByteSpan(path));
  if (!result.has_value()) {
    return absl::InvalidArgumentError(FromVecU8(std::move(result).err()));
  }
  return DataFileReader(std::move(result).value());
}

AvroSchema DataFileReader::WriterSchema() const {
  return AvroSchema(reader_.writer_schema());
}

size_t DataFileReader::Count() const { return reader_.count(); }

bool DataFileReader::HasNext() const { return reader_.has_next(); }

absl::StatusOr<AvroValue> DataFileReader::NextValue() {
  rs_std::Result<rust::value::AvroValue, rust::vec_u8::VecU8> result =
      reader_.next_value();
  if (!result.has_value()) {
    return absl::OutOfRangeError(FromVecU8(std::move(result).err()));
  }
  return AvroValue(std::move(result).value());
}

void DataFileReader::Rewind() { reader_.rewind(); }

// ---------------------------------------------------------------------------
// StreamingDataFileWriter
// ---------------------------------------------------------------------------

StreamingDataFileWriter::StreamingDataFileWriter(
    rust::container::StreamingDataFileWriter writer)
    : writer_(std::move(writer)) {}

absl::StatusOr<StreamingDataFileWriter> StreamingDataFileWriter::Create(
    const AvroSchema& schema, Codec codec) {
  rs_std::Result<rust::container::AvroCodec, rust::vec_u8::VecU8> rust_codec =
      rust::container::AvroCodec::from_i32(static_cast<int32_t>(codec));
  if (!rust_codec.has_value()) {
    return absl::InvalidArgumentError(FromVecU8(std::move(rust_codec).err()));
  }
  rs_std::Result<rust::container::StreamingDataFileWriter, rust::vec_u8::VecU8>
      result = rust::container::StreamingDataFileWriter::create(
          schema.schema_, std::move(rust_codec).value());
  if (!result.has_value()) {
    return absl::InvalidArgumentError(FromVecU8(std::move(result).err()));
  }
  return StreamingDataFileWriter(std::move(result).value());
}

absl::StatusOr<AvroSchema> StreamingDataFileWriter::Schema() const {
  rs_std::Result<rust::schema::AvroSchema, rust::vec_u8::VecU8> result =
      writer_.schema();
  if (!result.has_value()) {
    return absl::FailedPreconditionError(FromVecU8(std::move(result).err()));
  }
  return AvroSchema(std::move(result).value());
}

absl::Status StreamingDataFileWriter::Append(const AvroValue& value) {
  return ToStatus(writer_.append(value.value_), kBadInput);
}

absl::StatusOr<std::string> StreamingDataFileWriter::TakeBytes() {
  return UnwrapString(writer_.take_bytes(), absl::StatusCode::kInternal);
}

absl::StatusOr<std::string> StreamingDataFileWriter::Finish() {
  return UnwrapString(writer_.finish(), absl::StatusCode::kInternal);
}

bool StreamingDataFileWriter::IsFinished() const {
  return writer_.is_finished();
}

// ---------------------------------------------------------------------------
// StreamingDataFileReader
// ---------------------------------------------------------------------------

StreamingDataFileReader::StreamingDataFileReader(
    rust::container::StreamingDataFileReader reader)
    : reader_(std::move(reader)) {}

absl::StatusOr<StreamingDataFileReader> StreamingDataFileReader::FromBytes(
    absl::string_view data) {
  rs_std::Result<rust::container::StreamingDataFileReader, rust::vec_u8::VecU8>
      result = rust::container::StreamingDataFileReader::from_bytes(
          ToByteSpan(data));
  if (!result.has_value()) {
    return absl::InvalidArgumentError(FromVecU8(std::move(result).err()));
  }
  return StreamingDataFileReader(std::move(result).value());
}

absl::StatusOr<StreamingDataFileReader>
StreamingDataFileReader::FromBytesWithSchema(const AvroSchema& reader_schema,
                                             absl::string_view data) {
  rs_std::Result<rust::container::StreamingDataFileReader, rust::vec_u8::VecU8>
      result = rust::container::StreamingDataFileReader::from_bytes_with_schema(
          reader_schema.schema_, ToByteSpan(data));
  if (!result.has_value()) {
    return absl::InvalidArgumentError(FromVecU8(std::move(result).err()));
  }
  return StreamingDataFileReader(std::move(result).value());
}

absl::StatusOr<StreamingDataFileReader> StreamingDataFileReader::FromPath(
    absl::string_view path) {
  rs_std::Result<rust::container::StreamingDataFileReader, rust::vec_u8::VecU8>
      result = rust::container::StreamingDataFileReader::from_path(
          ToByteSpan(path));
  if (!result.has_value()) {
    return absl::InvalidArgumentError(FromVecU8(std::move(result).err()));
  }
  return StreamingDataFileReader(std::move(result).value());
}

absl::StatusOr<StreamingDataFileReader>
StreamingDataFileReader::FromPathWithSchema(const AvroSchema& reader_schema,
                                            absl::string_view path) {
  rs_std::Result<rust::container::StreamingDataFileReader, rust::vec_u8::VecU8>
      result = rust::container::StreamingDataFileReader::from_path_with_schema(
          reader_schema.schema_, ToByteSpan(path));
  if (!result.has_value()) {
    return absl::InvalidArgumentError(FromVecU8(std::move(result).err()));
  }
  return StreamingDataFileReader(std::move(result).value());
}

AvroSchema StreamingDataFileReader::WriterSchema() const {
  return AvroSchema(reader_.writer_schema());
}

bool StreamingDataFileReader::HasNext() { return reader_.has_next(); }

absl::StatusOr<AvroValue> StreamingDataFileReader::NextValue() {
  rs_std::Result<rust::value::AvroValue, rust::vec_u8::VecU8> result =
      reader_.next_value();
  if (!result.has_value()) {
    return absl::OutOfRangeError(FromVecU8(std::move(result).err()));
  }
  return AvroValue(std::move(result).value());
}

}  // namespace security::avro
