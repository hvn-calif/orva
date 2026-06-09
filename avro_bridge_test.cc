#include "avro_bridge.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/status/status.h"

namespace security::avro {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

constexpr char kRecordSchema[] = R"({
  "type": "record",
  "name": "User",
  "namespace": "com.example",
  "fields": [
    {"name": "id", "type": "long"},
    {"name": "name", "type": "string"}
  ]
})";

AvroValue MakeUser(int64_t id, const std::string& name) {
  AvroValue record = AvroValue::CreateRecord();
  EXPECT_TRUE(record.RecordPut("id", AvroValue::CreateLong(id)).ok());
  auto name_value = AvroValue::CreateString(name);
  EXPECT_TRUE(name_value.ok());
  EXPECT_TRUE(record.RecordPut("name", *name_value).ok());
  return record;
}

// -- Schema ---------------------------------------------------------------

TEST(AvroSchemaTest, ParsePrimitive) {
  auto schema = AvroSchema::Parse("\"int\"");
  ASSERT_TRUE(schema.ok());
  EXPECT_TRUE(schema->IsInt());
  EXPECT_FALSE(schema->IsLong());
  EXPECT_EQ(schema->TypeName(), "int");
}

TEST(AvroSchemaTest, ParseMalformedFails) {
  EXPECT_EQ(AvroSchema::Parse("{oops").status().code(),
            absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(AvroSchema::Parse("\"not_a_type\"").status().code(),
            absl::StatusCode::kInvalidArgument);
  // Invalid UTF-8 in the schema JSON.
  EXPECT_EQ(AvroSchema::Parse("\xff\xfe").status().code(),
            absl::StatusCode::kInvalidArgument);
}

TEST(AvroSchemaTest, RecordNames) {
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  EXPECT_TRUE(schema->IsRecord());
  EXPECT_EQ(schema->Name().value_or(""), "User");
  EXPECT_EQ(schema->Namespace().value_or(""), "com.example");
  EXPECT_EQ(schema->FullName().value_or(""), "com.example.User");
}

TEST(AvroSchemaTest, UnnamedSchemaNameFails) {
  auto schema = AvroSchema::Parse("\"int\"");
  ASSERT_TRUE(schema.ok());
  EXPECT_EQ(schema->Name().status().code(),
            absl::StatusCode::kFailedPrecondition);
}

TEST(AvroSchemaTest, RabinFingerprintMatchesApacheTestData) {
  // Expected values from apache/avro share/test/data/schema-tests.txt.
  auto int_schema = AvroSchema::Parse("\"int\"");
  ASSERT_TRUE(int_schema.ok());
  EXPECT_EQ(int_schema->FingerprintRabin(), 8247732601305521295LL);

  auto record = AvroSchema::Parse(R"({"fields":[], "type":"record", "name":"foo"})");
  ASSERT_TRUE(record.ok());
  EXPECT_EQ(record->FingerprintRabin(), -4824392279771201922LL);
  EXPECT_EQ(record->CanonicalForm(),
            R"({"name":"foo","type":"record","fields":[]})");
}

TEST(AvroSchemaTest, HexFingerprintShapes) {
  auto schema = AvroSchema::Parse("\"int\"");
  ASSERT_TRUE(schema.ok());
  EXPECT_EQ(schema->FingerprintRabinHex().size(), 16);
  EXPECT_EQ(schema->FingerprintMd5Hex().size(), 32);
  EXPECT_EQ(schema->FingerprintSha256Hex().size(), 64);
}

TEST(AvroSchemaTest, ParseListCrossReference) {
  std::vector<absl::string_view> jsons = {
      R"({"type": "record", "name": "Address", "fields": [
          {"name": "city", "type": "string"}]})",
      R"({"type": "record", "name": "Person", "fields": [
          {"name": "address", "type": "Address"}]})",
  };
  auto schemas = AvroSchema::ParseList(jsons);
  ASSERT_TRUE(schemas.ok());
  ASSERT_EQ(schemas->size(), 2);
  EXPECT_EQ((*schemas)[0].Name().value_or(""), "Address");
  EXPECT_EQ((*schemas)[1].Name().value_or(""), "Person");
}

TEST(AvroSchemaTest, SchemaEvolutionCompatibility) {
  auto int_schema = AvroSchema::Parse("\"int\"");
  auto long_schema = AvroSchema::Parse("\"long\"");
  ASSERT_TRUE(int_schema.ok());
  ASSERT_TRUE(long_schema.ok());
  EXPECT_TRUE(long_schema->CanReadFrom(*int_schema).ok());
  EXPECT_FALSE(int_schema->CanReadFrom(*long_schema).ok());
  EXPECT_FALSE(long_schema->MutualRead(*int_schema).ok());
}

TEST(AvroSchemaTest, Equality) {
  auto a = AvroSchema::Parse("\"int\"");
  auto b = AvroSchema::Parse("\"int\"");
  auto c = AvroSchema::Parse("\"long\"");
  ASSERT_TRUE(a.ok() && b.ok() && c.ok());
  EXPECT_TRUE(*a == *b);
  EXPECT_TRUE(*a != *c);
}

// -- Value ----------------------------------------------------------------

TEST(AvroValueTest, PrimitiveRoundtrips) {
  EXPECT_TRUE(AvroValue::CreateNull().IsNull());
  EXPECT_EQ(AvroValue::CreateBoolean(true).GetBoolean().value_or(false), true);
  EXPECT_EQ(AvroValue::CreateInt(-42).GetInt().value_or(0), -42);
  EXPECT_EQ(AvroValue::CreateLong(1LL << 40).GetLong().value_or(0), 1LL << 40);
  EXPECT_EQ(AvroValue::CreateFloat(1.5f).GetFloat().value_or(0), 1.5f);
  EXPECT_EQ(AvroValue::CreateDouble(2.5).GetDouble().value_or(0), 2.5);
  EXPECT_EQ(AvroValue::CreateBytes("\x01\x02").GetBytes().value_or(""),
            std::string("\x01\x02"));
  auto str = AvroValue::CreateString("hello");
  ASSERT_TRUE(str.ok());
  EXPECT_EQ(str->GetString().value_or(""), "hello");
}

TEST(AvroValueTest, WrongTypeAccessFails) {
  AvroValue value = AvroValue::CreateInt(1);
  auto result = value.GetString();
  EXPECT_EQ(result.status().code(), absl::StatusCode::kFailedPrecondition);
  EXPECT_THAT(result.status().message(), HasSubstr("int"));
}

TEST(AvroValueTest, InvalidUtf8StringFails) {
  EXPECT_EQ(AvroValue::CreateString("\xff\xfe").status().code(),
            absl::StatusCode::kInvalidArgument);
}

TEST(AvroValueTest, RecordPutReplacesInPlace) {
  // Exercises the Crubit `&mut self` binding: the second put must mutate
  // this object, not a copy, and must replace rather than append.
  AvroValue record = AvroValue::CreateRecord();
  ASSERT_TRUE(record.RecordPut("id", AvroValue::CreateLong(1)).ok());
  ASSERT_TRUE(record.RecordPut("id", AvroValue::CreateLong(2)).ok());
  EXPECT_EQ(record.GetRecordLen().value_or(0), 1);
  EXPECT_EQ(record.GetRecordField("id")->GetLong().value_or(0), 2);
}

TEST(AvroValueTest, RecordOperations) {
  AvroValue record = MakeUser(7, "alice");
  EXPECT_TRUE(record.IsRecord());
  EXPECT_EQ(record.GetRecordLen().value_or(0), 2);
  EXPECT_TRUE(record.HasRecordField("id").value_or(false));
  EXPECT_FALSE(record.HasRecordField("zip").value_or(true));
  EXPECT_EQ(record.GetRecordField("id")->GetLong().value_or(0), 7);
  EXPECT_EQ(record.GetRecordField("zip").status().code(),
            absl::StatusCode::kNotFound);
  EXPECT_THAT(record.GetRecordFieldNames().value_or(
                  std::vector<std::string>{}),
              ElementsAre("id", "name"));
}

TEST(AvroValueTest, ArrayAndMapOperations) {
  AvroValue array = AvroValue::CreateArray();
  EXPECT_TRUE(array.ArrayPush(AvroValue::CreateInt(1)).ok());
  EXPECT_TRUE(array.ArrayPush(AvroValue::CreateInt(2)).ok());
  EXPECT_EQ(array.GetArrayLen().value_or(0), 2);
  EXPECT_EQ(array.GetArrayItem(1)->GetInt().value_or(0), 2);
  EXPECT_EQ(array.GetArrayItem(5).status().code(),
            absl::StatusCode::kNotFound);

  AvroValue map = AvroValue::CreateMap();
  EXPECT_TRUE(map.MapPut("zebra", AvroValue::CreateInt(1)).ok());
  EXPECT_TRUE(map.MapPut("apple", AvroValue::CreateInt(2)).ok());
  EXPECT_THAT(map.GetMapKeys().value_or(std::vector<std::string>{}),
              ElementsAre("apple", "zebra"));
  EXPECT_EQ(map.GetMapValue("zebra")->GetInt().value_or(0), 1);
  EXPECT_EQ(map.GetMapValue("pear").status().code(),
            absl::StatusCode::kNotFound);

  // Mutating the wrong type fails.
  EXPECT_EQ(map.ArrayPush(AvroValue::CreateNull()).code(),
            absl::StatusCode::kFailedPrecondition);
}

TEST(AvroValueTest, UnionEnumFixed) {
  auto inner = AvroValue::CreateLong(9);
  AvroValue u = AvroValue::CreateUnion(1, inner);
  EXPECT_TRUE(u.IsUnion());
  EXPECT_EQ(u.GetUnionBranch().value_or(0), 1);
  EXPECT_TRUE(*u.GetUnionValue() == inner);

  auto e = AvroValue::CreateEnum(2, "GREEN");
  ASSERT_TRUE(e.ok());
  EXPECT_EQ(e->GetEnumPosition().value_or(0), 2);
  EXPECT_EQ(e->GetEnumSymbol().value_or(""), "GREEN");

  AvroValue f = AvroValue::CreateFixed("abcd");
  EXPECT_EQ(f.GetFixedBytes().value_or(""), "abcd");
}

TEST(AvroValueTest, LogicalTypes) {
  AvroValue decimal = AvroValue::CreateDecimal("\x01\x18");
  EXPECT_TRUE(decimal.IsDecimal());
  EXPECT_EQ(decimal.GetDecimalBytes().value_or(""), std::string("\x01\x18"));

  auto uuid = AvroValue::CreateUuid("6f2b0e76-4d3d-4f8e-9d3a-2e1b8a7c6d5e");
  ASSERT_TRUE(uuid.ok());
  EXPECT_EQ(uuid->GetUuid().value_or(""),
            "6f2b0e76-4d3d-4f8e-9d3a-2e1b8a7c6d5e");
  EXPECT_EQ(AvroValue::CreateUuid("nope").status().code(),
            absl::StatusCode::kInvalidArgument);

  EXPECT_EQ(AvroValue::CreateDate(19000).GetDate().value_or(0), 19000);
  EXPECT_EQ(AvroValue::CreateTimestampMicros(123).GetTimestampMicros()
                .value_or(0),
            123);

  AvroValue duration = AvroValue::CreateDuration(1, 2, 3);
  EXPECT_EQ(duration.GetDurationMonths().value_or(0), 1);
  EXPECT_EQ(duration.GetDurationDays().value_or(0), 2);
  EXPECT_EQ(duration.GetDurationMillis().value_or(0), 3);
}

TEST(AvroValueTest, ValidateAndResolve) {
  auto long_schema = AvroSchema::Parse("\"long\"");
  ASSERT_TRUE(long_schema.ok());
  EXPECT_TRUE(AvroValue::CreateLong(5).Validate(*long_schema));
  EXPECT_FALSE(AvroValue::CreateBoolean(true).Validate(*long_schema));

  auto resolved = AvroValue::CreateInt(41).Resolve(*long_schema);
  ASSERT_TRUE(resolved.ok());
  EXPECT_EQ(resolved->GetLong().value_or(0), 41);
}

TEST(AvroValueTest, ToJsonString) {
  AvroValue record = MakeUser(7, "x");
  auto json = record.ToJsonString();
  ASSERT_TRUE(json.ok());
  EXPECT_THAT(*json, HasSubstr("\"id\":7"));
  EXPECT_THAT(*json, HasSubstr("\"name\":\"x\""));
}

// -- Single datum ----------------------------------------------------------

TEST(DatumTest, EncodeDecodeRoundtrip) {
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  AvroValue value = MakeUser(3, "bob");
  auto encoded = EncodeDatum(*schema, value);
  ASSERT_TRUE(encoded.ok());
  auto decoded = DecodeDatum(*schema, *encoded);
  ASSERT_TRUE(decoded.ok());
  EXPECT_TRUE(*decoded == value);
}

TEST(DatumTest, EncodeMismatchedValueFails) {
  auto schema = AvroSchema::Parse("\"string\"");
  ASSERT_TRUE(schema.ok());
  EXPECT_EQ(EncodeDatum(*schema, AvroValue::CreateLong(1)).status().code(),
            absl::StatusCode::kInvalidArgument);
}

TEST(DatumTest, DecodeTruncatedFails) {
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  auto encoded = EncodeDatum(*schema, MakeUser(3, "bob"));
  ASSERT_TRUE(encoded.ok());
  EXPECT_FALSE(
      DecodeDatum(*schema, absl::string_view(*encoded).substr(0, 1)).ok());
}

TEST(DatumTest, ResolvedDecodePromotesAndDefaults) {
  auto writer = AvroSchema::Parse(R"({"type": "record", "name": "R", "fields": [
      {"name": "a", "type": "int"}]})");
  auto reader = AvroSchema::Parse(R"({"type": "record", "name": "R", "fields": [
      {"name": "a", "type": "long"},
      {"name": "b", "type": "string", "default": "fallback"}]})");
  ASSERT_TRUE(writer.ok() && reader.ok());

  AvroValue record = AvroValue::CreateRecord();
  ASSERT_TRUE(record.RecordPut("a", AvroValue::CreateInt(12)).ok());
  auto encoded = EncodeDatum(*writer, record);
  ASSERT_TRUE(encoded.ok());

  auto decoded = DecodeDatumResolved(*writer, *reader, *encoded);
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(decoded->GetRecordField("a")->GetLong().value_or(0), 12);
  EXPECT_EQ(decoded->GetRecordField("b")->GetString().value_or(""),
            "fallback");
}

TEST(DatumTest, SchemataRoundtrip) {
  std::vector<absl::string_view> jsons = {
      R"({"type": "record", "name": "Address", "fields": [
          {"name": "city", "type": "string"}]})",
      R"({"type": "record", "name": "Person", "fields": [
          {"name": "address", "type": "Address"}]})",
  };
  auto schemas = AvroSchema::ParseList(jsons);
  ASSERT_TRUE(schemas.ok());

  AvroValue address = AvroValue::CreateRecord();
  auto city = AvroValue::CreateString("Zurich");
  ASSERT_TRUE(city.ok());
  ASSERT_TRUE(address.RecordPut("city", *city).ok());
  AvroValue person = AvroValue::CreateRecord();
  ASSERT_TRUE(person.RecordPut("address", address).ok());

  auto encoded = EncodeDatumSchemata((*schemas)[1], *schemas, person);
  ASSERT_TRUE(encoded.ok());
  auto decoded = DecodeDatumSchemata((*schemas)[1], *schemas, *encoded);
  ASSERT_TRUE(decoded.ok());
  EXPECT_TRUE(*decoded == person);
}

TEST(DatumTest, SetMaxAllocationBytesFirstCallWins) {
  // The limit is process-global and initialized exactly once; assert the
  // invariant (second call returns the first observed value) rather than a
  // specific value, since decodes in other tests may initialize it first.
  size_t first = SetMaxAllocationBytes(512 * 1024 * 1024);
  size_t second = SetMaxAllocationBytes(1024);
  EXPECT_EQ(first, second);
}

// -- Object container files -------------------------------------------------

class ContainerCodecTest : public ::testing::TestWithParam<Codec> {};

TEST_P(ContainerCodecTest, WriteReadRoundtrip) {
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());

  auto writer = DataFileWriter::Create(*schema, GetParam());
  ASSERT_TRUE(writer.ok());
  AvroValue first = MakeUser(1, "a");
  AvroValue second = MakeUser(2, "b");
  ASSERT_TRUE(writer->Append(first).ok());
  ASSERT_TRUE(writer->Append(second).ok());
  EXPECT_EQ(writer->Count(), 2);
  EXPECT_TRUE(writer->Schema() == *schema);

  auto bytes = writer->ToBytes();
  ASSERT_TRUE(bytes.ok());

  auto reader = DataFileReader::FromBytes(*bytes);
  ASSERT_TRUE(reader.ok());
  EXPECT_EQ(reader->Count(), 2);
  EXPECT_TRUE(reader->WriterSchema() == *schema);
  ASSERT_TRUE(reader->HasNext());
  EXPECT_TRUE(*reader->NextValue() == first);
  EXPECT_TRUE(*reader->NextValue() == second);
  EXPECT_FALSE(reader->HasNext());
  EXPECT_EQ(reader->NextValue().status().code(),
            absl::StatusCode::kOutOfRange);
  reader->Rewind();
  EXPECT_TRUE(*reader->NextValue() == first);
}

INSTANTIATE_TEST_SUITE_P(AllCodecs, ContainerCodecTest,
                         ::testing::Values(Codec::kNull, Codec::kDeflate,
                                           Codec::kSnappy, Codec::kZstandard));

TEST(ContainerTest, AppendInvalidValueFails) {
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  auto writer = DataFileWriter::Create(*schema, Codec::kNull);
  ASSERT_TRUE(writer.ok());
  EXPECT_EQ(writer->Append(AvroValue::CreateLong(1)).code(),
            absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(writer->Count(), 0);
}

TEST(ContainerTest, CreateWithCrossReferencingSchemaFails) {
  // Schemas from ParseList contain unresolved references; using one for a
  // container file must fail cleanly (and not crash in Append, which used
  // to hit a panic inside apache-avro's validation).
  std::vector<absl::string_view> jsons = {
      R"({"type": "record", "name": "Address", "fields": [
          {"name": "city", "type": "string"}]})",
      R"({"type": "record", "name": "Person", "fields": [
          {"name": "address", "type": "Address"}]})",
  };
  auto schemas = AvroSchema::ParseList(jsons);
  ASSERT_TRUE(schemas.ok());
  auto writer = DataFileWriter::Create((*schemas)[1], Codec::kNull);
  EXPECT_EQ(writer.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_THAT(writer.status().message(), HasSubstr("not self-contained"));
}

TEST(ContainerTest, InvalidCodecEnumFails) {
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  EXPECT_EQ(
      DataFileWriter::Create(*schema, static_cast<Codec>(42)).status().code(),
      absl::StatusCode::kInvalidArgument);
}

TEST(ContainerTest, GarbageInputFails) {
  EXPECT_EQ(DataFileReader::FromBytes("not an avro file").status().code(),
            absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(DataFileReader::FromBytes("").status().code(),
            absl::StatusCode::kInvalidArgument);
}

TEST(ContainerTest, ReaderSchemaResolution) {
  auto writer_schema =
      AvroSchema::Parse(R"({"type": "record", "name": "R", "fields": [
          {"name": "a", "type": "int"}]})");
  auto reader_schema =
      AvroSchema::Parse(R"({"type": "record", "name": "R", "fields": [
          {"name": "a", "type": "long"},
          {"name": "b", "type": "string", "default": "d"}]})");
  ASSERT_TRUE(writer_schema.ok() && reader_schema.ok());

  AvroValue record = AvroValue::CreateRecord();
  ASSERT_TRUE(record.RecordPut("a", AvroValue::CreateInt(12)).ok());
  auto writer = DataFileWriter::Create(*writer_schema, Codec::kNull);
  ASSERT_TRUE(writer.ok());
  ASSERT_TRUE(writer->Append(record).ok());
  auto bytes = writer->ToBytes();
  ASSERT_TRUE(bytes.ok());

  auto reader = DataFileReader::FromBytesWithSchema(*reader_schema, *bytes);
  ASSERT_TRUE(reader.ok());
  auto value = reader->NextValue();
  ASSERT_TRUE(value.ok());
  EXPECT_EQ(value->GetRecordField("a")->GetLong().value_or(0), 12);
  EXPECT_EQ(value->GetRecordField("b")->GetString().value_or(""), "d");
  EXPECT_TRUE(reader->WriterSchema() == *writer_schema);
}

TEST(ContainerTest, PathRoundtrip) {
  std::string path = ::testing::TempDir() + "/avro_bridge_test_data.avro";

  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  auto writer = DataFileWriter::Create(*schema, Codec::kDeflate);
  ASSERT_TRUE(writer.ok());
  AvroValue value = MakeUser(11, "disk");
  ASSERT_TRUE(writer->Append(value).ok());
  ASSERT_TRUE(writer->WriteToPath(path).ok());

  auto reader = DataFileReader::FromPath(path);
  ASSERT_TRUE(reader.ok());
  EXPECT_TRUE(*reader->NextValue() == value);

  auto resolved = DataFileReader::FromPathWithSchema(*schema, path);
  ASSERT_TRUE(resolved.ok());
  EXPECT_TRUE(*resolved->NextValue() == value);
}

TEST(ContainerTest, MissingFileFails) {
  EXPECT_FALSE(DataFileReader::FromPath("/nonexistent/x.avro").ok());
}

TEST(ContainerTest, OcfMagicPresent) {
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  auto writer = DataFileWriter::Create(*schema, Codec::kNull);
  ASSERT_TRUE(writer.ok());
  ASSERT_TRUE(writer->Append(MakeUser(1, "m")).ok());
  auto bytes = writer->ToBytes();
  ASSERT_TRUE(bytes.ok());
  ASSERT_GE(bytes->size(), 4);
  EXPECT_EQ(bytes->substr(0, 4), std::string("Obj\x01", 4));
}

// -- Coverage backfill ------------------------------------------------------
// Tests added to close gaps surfaced by the security review: the
// timestamp/local-timestamp family, map length/key predicates,
// AvroValue::TypeName, AvroSchema::ToJsonString, non-finite-float JSON
// error paths, the empty container file, and trailing-byte rejection.
// (SetMaxAllocationBytes-actually-rejects-oversized-input is covered by the
// Rust integration test tests/max_allocation.rs, which runs in its own
// process because the limit is a one-shot process global.)

TEST(AvroValueTest, TypeName) {
  EXPECT_EQ(AvroValue::CreateInt(1).TypeName(), "int");
  EXPECT_EQ(AvroValue::CreateNull().TypeName(), "null");
  EXPECT_EQ(AvroValue::CreateMap().TypeName(), "map");
  EXPECT_EQ(AvroValue::CreateTimestampMillis(5).TypeName(), "timestamp-millis");
}

TEST(AvroValueTest, TimeAndTimestampFamily) {
  EXPECT_EQ(AvroValue::CreateTimeMillis(11).GetTimeMillis().value_or(0), 11);
  EXPECT_EQ(AvroValue::CreateTimeMicros(12).GetTimeMicros().value_or(0), 12);
  EXPECT_EQ(AvroValue::CreateTimestampMillis(13).GetTimestampMillis().value_or(0),
            13);
  EXPECT_EQ(AvroValue::CreateTimestampNanos(14).GetTimestampNanos().value_or(0),
            14);
  EXPECT_EQ(
      AvroValue::CreateLocalTimestampMillis(15).GetLocalTimestampMillis().value_or(0),
      15);
  EXPECT_EQ(
      AvroValue::CreateLocalTimestampMicros(16).GetLocalTimestampMicros().value_or(0),
      16);
  EXPECT_EQ(
      AvroValue::CreateLocalTimestampNanos(17).GetLocalTimestampNanos().value_or(0),
      17);

  // Accessors within the timestamp family do not silently coerce across
  // resolutions: asking a millis value for micros fails.
  EXPECT_EQ(
      AvroValue::CreateTimestampMillis(1).GetTimestampMicros().status().code(),
      absl::StatusCode::kFailedPrecondition);
  EXPECT_EQ(AvroValue::CreateTimeMillis(1).GetTimeMicros().status().code(),
            absl::StatusCode::kFailedPrecondition);
}

TEST(AvroValueTest, MapLenAndHasKey) {
  AvroValue map = AvroValue::CreateMap();
  EXPECT_EQ(map.GetMapLen().value_or(99), 0u);
  ASSERT_TRUE(map.MapPut("k", AvroValue::CreateInt(1)).ok());
  EXPECT_EQ(map.GetMapLen().value_or(0), 1u);
  EXPECT_TRUE(map.HasMapKey("k").value_or(false));
  EXPECT_FALSE(map.HasMapKey("absent").value_or(true));

  // Both fail on a non-map value (different type).
  EXPECT_EQ(AvroValue::CreateInt(1).GetMapLen().status().code(),
            absl::StatusCode::kFailedPrecondition);
  EXPECT_EQ(AvroValue::CreateInt(1).HasMapKey("k").status().code(),
            absl::StatusCode::kFailedPrecondition);
}

TEST(AvroValueTest, ToJsonStringRejectsNonFiniteFloat) {
  // NaN/Inf have no JSON representation; the conversion must fail rather
  // than emit invalid JSON.
  EXPECT_FALSE(AvroValue::CreateDouble(std::numeric_limits<double>::quiet_NaN())
                   .ToJsonString()
                   .ok());
  EXPECT_FALSE(AvroValue::CreateFloat(std::numeric_limits<float>::infinity())
                   .ToJsonString()
                   .ok());
}

TEST(AvroSchemaTest, ToJsonStringRoundtrips) {
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  auto json = schema->ToJsonString();
  ASSERT_TRUE(json.ok());
  auto reparsed = AvroSchema::Parse(*json);
  ASSERT_TRUE(reparsed.ok());
  EXPECT_TRUE(*schema == *reparsed);
}

TEST(ContainerTest, EmptyFileRoundtrips) {
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  auto writer = DataFileWriter::Create(*schema, Codec::kNull);
  ASSERT_TRUE(writer.ok());
  EXPECT_EQ(writer->Count(), 0u);
  auto bytes = writer->ToBytes();  // No Append calls: header only.
  ASSERT_TRUE(bytes.ok());
  auto reader = DataFileReader::FromBytes(*bytes);
  ASSERT_TRUE(reader.ok());
  EXPECT_EQ(reader->Count(), 0u);
  EXPECT_FALSE(reader->HasNext());
}

TEST(DatumTest, DecodeRejectsTrailingBytes) {
  // avrocpp silently ignores trailing bytes after a single datum; this
  // binding rejects them so a framing error cannot be silently swallowed.
  auto schema = AvroSchema::Parse("\"int\"");
  ASSERT_TRUE(schema.ok());
  auto encoded = EncodeDatum(*schema, AvroValue::CreateInt(7));
  ASSERT_TRUE(encoded.ok());
  std::string with_trailing = *encoded + std::string("\xde\xad", 2);
  EXPECT_FALSE(DecodeDatum(*schema, with_trailing).ok());
}

}  // namespace
}  // namespace security::avro
