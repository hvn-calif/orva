#include "avro_bridge.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
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

// Writes `values` through the writer, draining after each append, and
// returns the complete container file. Marks the test failed and returns ""
// on any writer error (EXPECT alone would keep running into operator-> on a
// failed StatusOr, which aborts without attribution).
std::string WriteFile(const AvroSchema& schema, Codec codec,
                      const std::vector<AvroValue>& values) {
  auto writer = DataFileWriter::Create(schema, codec);
  if (!writer.ok()) {
    ADD_FAILURE() << "Create failed: " << writer.status();
    return "";
  }
  std::string out;
  for (const AvroValue& value : values) {
    if (auto s = writer->Append(value); !s.ok()) {
      ADD_FAILURE() << "Append failed: " << s;
      return "";
    }
    auto chunk = writer->TakeBytes();
    if (!chunk.ok()) {
      ADD_FAILURE() << "TakeBytes failed: " << chunk.status();
      return "";
    }
    out += *chunk;
  }
  auto tail = writer->Finish();
  if (!tail.ok()) {
    ADD_FAILURE() << "Finish failed: " << tail.status();
    return "";
  }
  out += *tail;
  return out;
}

class ContainerCodecTest : public ::testing::TestWithParam<Codec> {};

TEST_P(ContainerCodecTest, WriteReadRoundtrip) {
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());

  auto writer = DataFileWriter::Create(*schema, GetParam());
  ASSERT_TRUE(writer.ok());
  EXPECT_TRUE(writer->Schema() == *schema);
  AvroValue first = MakeUser(1, "a");
  AvroValue second = MakeUser(2, "b");
  ASSERT_TRUE(writer->Append(first).ok());
  ASSERT_TRUE(writer->Append(second).ok());
  auto bytes = writer->Finish();
  ASSERT_TRUE(bytes.ok());
  EXPECT_TRUE(writer->IsFinished());

  auto reader = DataFileReader::FromBytes(*bytes);
  ASSERT_TRUE(reader.ok());
  EXPECT_TRUE(reader->HeaderReady());
  EXPECT_TRUE(*reader->WriterSchema() == *schema);
  ASSERT_TRUE(reader->NextReady().value_or(false));
  ASSERT_TRUE(reader->NextReady().value_or(false));  // idempotent
  EXPECT_TRUE(*reader->NextValue() == first);
  EXPECT_TRUE(*reader->NextValue() == second);
  EXPECT_FALSE(reader->NextReady().value_or(true));
  EXPECT_TRUE(reader->AtEnd());
  EXPECT_EQ(reader->NextValue().status().code(),
            absl::StatusCode::kOutOfRange);
}

INSTANTIATE_TEST_SUITE_P(AllCodecs, ContainerCodecTest,
                         ::testing::Values(Codec::kNull, Codec::kDeflate,
                                           Codec::kSnappy, Codec::kZstandard));

TEST(ContainerTest, NextValueIntoReusesCallerOwnedValue) {
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  AvroValue first = MakeUser(1, "alpha");
  AvroValue second = MakeUser(2, "bravo");
  std::string bytes = WriteFile(*schema, Codec::kNull, {first, second});

  auto reader = DataFileReader::FromBytes(bytes);
  ASSERT_TRUE(reader.ok());
  AvroValue value = AvroValue::CreateNull();
  ASSERT_TRUE(reader->NextValueInto(&value).value_or(false));
  EXPECT_TRUE(value == first);
  ASSERT_TRUE(reader->NextValueInto(&value).value_or(false));
  EXPECT_TRUE(value == second);
  EXPECT_FALSE(reader->NextValueInto(&value).value_or(true));
  EXPECT_TRUE(reader->AtEnd());
  EXPECT_EQ(reader->NextValueInto(nullptr).status().code(),
            absl::StatusCode::kInvalidArgument);
}

TEST(ContainerTest, ChunkedFeedRoundtrip) {
  // The streaming read path driven from C++: feed a multi-value file in
  // small chunks and drain values as they become decodable.
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  std::vector<AvroValue> values;
  for (int i = 0; i < 50; ++i) values.push_back(MakeUser(i, "chunked"));
  std::string bytes = WriteFile(*schema, Codec::kDeflate, values);

  DataFileReader reader = DataFileReader::Create();
  int seen = 0;
  const size_t kChunk = 7;
  for (size_t off = 0; off < bytes.size(); off += kChunk) {
    ASSERT_TRUE(
        reader.Feed(absl::string_view(bytes).substr(off, kChunk)).ok());
    while (reader.NextReady().value_or(false)) {
      auto value = reader.NextValue();
      ASSERT_TRUE(value.ok());
      EXPECT_EQ(value->GetRecordField("id")->GetLong().value_or(-1), seen);
      ++seen;
    }
  }
  ASSERT_TRUE(reader.CloseInput().ok());
  EXPECT_FALSE(reader.NextReady().value_or(true));
  EXPECT_TRUE(reader.AtEnd());
  EXPECT_EQ(seen, 50);
}

TEST(ContainerTest, TruncatedStreamErrorsAndFuses) {
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  std::string bytes =
      WriteFile(*schema, Codec::kNull, {MakeUser(1, "a"), MakeUser(2, "b")});

  DataFileReader reader = DataFileReader::Create();
  ASSERT_TRUE(reader.Feed(bytes.substr(0, bytes.size() - 8)).ok());
  ASSERT_TRUE(reader.CloseInput().ok());
  bool errored = false;
  for (int i = 0; i < 100; ++i) {
    auto ready = reader.NextReady();
    if (!ready.ok()) {
      errored = true;
      break;
    }
    if (!*ready) break;
    if (!reader.NextValue().ok()) {
      errored = true;
      break;
    }
  }
  EXPECT_TRUE(errored);
  // Fatal errors are sticky: everything keeps failing, and the stream never
  // reports a clean end. NextValue reports the fused error as
  // kInvalidArgument, distinct from the benign kOutOfRange.
  EXPECT_TRUE(reader.HasFailed());
  EXPECT_FALSE(reader.NextReady().ok());
  EXPECT_EQ(reader.NextValue().status().code(),
            absl::StatusCode::kInvalidArgument);
  EXPECT_FALSE(reader.Feed("more").ok());
  EXPECT_FALSE(reader.AtEnd());
}

TEST(ContainerTest, NextValueBeforeDataIsBenign) {
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  AvroValue value = MakeUser(7, "later");
  std::string bytes = WriteFile(*schema, Codec::kNull, {value});

  DataFileReader reader = DataFileReader::Create();
  ASSERT_TRUE(reader.Feed(bytes.substr(0, 10)).ok());
  // Nothing decodable yet; the error is kOutOfRange and NOT fatal.
  EXPECT_EQ(reader.NextValue().status().code(), absl::StatusCode::kOutOfRange);
  ASSERT_TRUE(reader.Feed(bytes.substr(10)).ok());
  ASSERT_TRUE(reader.CloseInput().ok());
  EXPECT_TRUE(*reader.NextValue() == value);
  EXPECT_TRUE(reader.AtEnd());
}

TEST(ContainerTest, WriterSchemaBeforeHeaderFails) {
  DataFileReader reader = DataFileReader::Create();
  EXPECT_FALSE(reader.HeaderReady());
  EXPECT_EQ(reader.WriterSchema().status().code(),
            absl::StatusCode::kFailedPrecondition);
}

TEST(ContainerTest, SetMaxBlockSizeValidation) {
  DataFileReader reader = DataFileReader::Create();
  EXPECT_FALSE(reader.SetMaxBlockSize(0).ok());
  EXPECT_TRUE(reader.SetMaxBlockSize(1024 * 1024).ok());
}

TEST(ContainerTest, WriterFinishConsumes) {
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  auto writer = DataFileWriter::Create(*schema, Codec::kNull);
  ASSERT_TRUE(writer.ok());
  ASSERT_TRUE(writer->Append(MakeUser(1, "a")).ok());
  EXPECT_FALSE(writer->IsFinished());
  ASSERT_TRUE(writer->Finish().ok());
  EXPECT_TRUE(writer->IsFinished());
  EXPECT_FALSE(writer->Append(MakeUser(2, "b")).ok());
  EXPECT_FALSE(writer->TakeBytes().ok());
  EXPECT_FALSE(writer->Finish().ok());
  // The schema stays queryable after Finish.
  EXPECT_TRUE(writer->Schema() == *schema);
}

TEST(ContainerTest, TakeBytesIncremental) {
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  auto writer = DataFileWriter::Create(*schema, Codec::kNull);
  ASSERT_TRUE(writer.ok());
  std::string file;
  const int n = 4000;
  for (int i = 0; i < n; ++i) {
    ASSERT_TRUE(writer->Append(MakeUser(i, "sensor")).ok());
    auto chunk = writer->TakeBytes();
    ASSERT_TRUE(chunk.ok());
    file += *chunk;
  }
  // Batches were flushed and drained before finish.
  EXPECT_FALSE(file.empty());
  auto tail = writer->Finish();
  ASSERT_TRUE(tail.ok());
  file += *tail;

  auto reader = DataFileReader::FromBytes(file);
  ASSERT_TRUE(reader.ok());
  int count = 0;
  while (reader->NextReady().value_or(false)) {
    auto value = reader->NextValue();
    ASSERT_TRUE(value.ok());
    EXPECT_EQ(value->GetRecordField("id")->GetLong().value_or(-1), count);
    ++count;
  }
  EXPECT_EQ(count, n);
  EXPECT_TRUE(reader->AtEnd());
}

TEST(ContainerTest, AppendInvalidValueFailsAndWriterStaysUsable) {
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  auto writer = DataFileWriter::Create(*schema, Codec::kNull);
  ASSERT_TRUE(writer.ok());
  EXPECT_EQ(writer->Append(AvroValue::CreateLong(1)).code(),
            absl::StatusCode::kInvalidArgument);
  EXPECT_FALSE(writer->IsFinished());
  AvroValue value = MakeUser(1, "ok");
  ASSERT_TRUE(writer->Append(value).ok());
  auto bytes = writer->Finish();
  ASSERT_TRUE(bytes.ok());
  auto reader = DataFileReader::FromBytes(*bytes);
  ASSERT_TRUE(reader.ok());
  EXPECT_TRUE(*reader->NextValue() == value);
  EXPECT_FALSE(reader->NextReady().value_or(true));
}

TEST(AvroPathTest, ReadsLeavesAndAgreesWithTheCloningAccessors) {
  AvroValue item = AvroValue::CreateRecord();
  ASSERT_TRUE(item.RecordPut("key", *AvroValue::CreateString("first")).ok());
  ASSERT_TRUE(item.RecordPut("count", AvroValue::CreateLong(10)).ok());
  AvroValue items = AvroValue::CreateArray();
  ASSERT_TRUE(items.ArrayPush(item).ok());
  AvroValue tags = AvroValue::CreateMap();
  ASSERT_TRUE(tags.MapPut("a", AvroValue::CreateLong(7)).ok());
  AvroValue record = AvroValue::CreateRecord();
  ASSERT_TRUE(record.RecordPut("items", items).ok());
  ASSERT_TRUE(record.RecordPut("tags", tags).ok());

  AvroPath path;
  EXPECT_TRUE(path.IsEmpty());
  EXPECT_EQ(record.GetRecordLenAt(path).value_or(0), 2);
  ASSERT_TRUE(path.PushField("items").ok());
  EXPECT_EQ(record.GetArrayLenAt(path).value_or(0), 1);
  path.PushIndex(0);
  ASSERT_TRUE(path.PushField("count").ok());
  EXPECT_EQ(path.Length(), 3);
  EXPECT_EQ(record.GetLongAt(path).value_or(0), 10);

  ASSERT_TRUE(path.Pop().ok());
  ASSERT_TRUE(path.PushField("key").ok());
  EXPECT_EQ(record.GetStringAt(path).value_or(""), "first");
  EXPECT_EQ(record.TypeNameAt(path).value_or(""), "string");

  // Same answer as walking with the cloning accessors.
  auto by_clone = record.GetRecordField("items")
                      .value()
                      .GetArrayItem(0)
                      .value()
                      .GetRecordField("key")
                      .value()
                      .GetString();
  ASSERT_TRUE(by_clone.ok());
  EXPECT_EQ(record.GetStringAt(path).value_or(""), *by_clone);

  AvroPath map_path;
  ASSERT_TRUE(map_path.PushField("tags").ok());
  EXPECT_EQ(record.GetMapLenAt(map_path).value_or(0), 1);
  auto keys = record.GetMapKeysAt(map_path);
  ASSERT_TRUE(keys.ok());
  EXPECT_THAT(*keys, ::testing::ElementsAre("a"));
  ASSERT_TRUE(map_path.PushKey("a").ok());
  EXPECT_EQ(record.GetLongAt(map_path).value_or(0), 7);
}

TEST(AvroPathTest, MissingAndMistypedPathsFailWithTheOffendingPath) {
  AvroValue record = AvroValue::CreateRecord();
  ASSERT_TRUE(record.RecordPut("id", AvroValue::CreateLong(1)).ok());

  AvroPath missing;
  ASSERT_TRUE(missing.PushField("nope").ok());
  auto err = record.GetLongAt(missing);
  EXPECT_EQ(err.status().code(), absl::StatusCode::kNotFound);
  EXPECT_THAT(std::string(err.status().message()), ::testing::HasSubstr("nope"));

  AvroPath wrong_type;
  ASSERT_TRUE(wrong_type.PushField("id").ok());
  EXPECT_THAT(std::string(record.GetStringAt(wrong_type).status().message()),
              ::testing::HasSubstr("not string"));

  // Path editing is checked rather than silently ignored.
  AvroPath empty;
  EXPECT_FALSE(empty.Pop().ok());
  EXPECT_FALSE(empty.SetLastIndex(0).ok());
  ASSERT_TRUE(empty.PushField("id").ok());
  EXPECT_FALSE(empty.SetLastIndex(0).ok()) << "last step is a field";
  empty.PushIndex(1);
  EXPECT_TRUE(empty.SetLastIndex(2).ok());
  empty.Clear();
  EXPECT_TRUE(empty.IsEmpty());
}

TEST(AvroPathTest, UnionsAreTransparent) {
  AvroValue inner = AvroValue::CreateRecord();
  ASSERT_TRUE(inner.RecordPut("email", *AvroValue::CreateString("x@y")).ok());
  AvroValue record = AvroValue::CreateRecord();
  ASSERT_TRUE(record.RecordPut("contact", AvroValue::CreateUnion(1, inner)).ok());
  ASSERT_TRUE(
      record.RecordPut("maybe", AvroValue::CreateUnion(0, AvroValue::CreateNull()))
          .ok());

  // A step after the union applies to the branch it holds.
  AvroPath through;
  ASSERT_TRUE(through.PushField("contact").ok());
  ASSERT_TRUE(through.PushField("email").ok());
  EXPECT_EQ(record.GetStringAt(through).value_or(""), "x@y");

  AvroPath null_branch;
  ASSERT_TRUE(null_branch.PushField("maybe").ok());
  EXPECT_TRUE(record.IsNullAt(null_branch).value_or(false));

  // GetValueAt keeps the union, so it stays inspectable.
  AvroPath contact;
  ASSERT_TRUE(contact.PushField("contact").ok());
  auto raw = record.GetValueAt(contact);
  ASSERT_TRUE(raw.ok());
  EXPECT_TRUE(raw->IsUnion());
}

TEST(ContainerTest, AppendMoveWritesTheSameFileAndEmptiesTheValue) {
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  const AvroValue original = MakeUser(7, "moved");

  auto copying = DataFileWriter::Create(*schema, Codec::kNull);
  ASSERT_TRUE(copying.ok());
  ASSERT_TRUE(copying->Append(original).ok());
  auto copied = copying->Finish();
  ASSERT_TRUE(copied.ok());

  auto moving = DataFileWriter::Create(*schema, Codec::kNull);
  ASSERT_TRUE(moving.ok());
  AvroValue donor = MakeUser(7, "moved");
  ASSERT_TRUE(moving->Append(std::move(donor)).ok());
  auto moved = moving->Finish();
  ASSERT_TRUE(moved.ok());

  EXPECT_TRUE(donor.IsNull()) << "the moved-from value should be emptied";

  // Sync markers are random per writer, so compare decoded values.
  auto from_copy = DataFileReader::FromBytes(*copied);
  auto from_move = DataFileReader::FromBytes(*moved);
  ASSERT_TRUE(from_copy.ok() && from_move.ok());
  auto copied_value = from_copy->NextValue();
  auto moved_value = from_move->NextValue();
  ASSERT_TRUE(copied_value.ok() && moved_value.ok());
  EXPECT_TRUE(*moved_value == original);
  EXPECT_TRUE(*moved_value == *copied_value);
}

TEST(ContainerTest, AppendMoveLeavesARejectedValueIntact) {
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  auto writer = DataFileWriter::Create(*schema, Codec::kNull);
  ASSERT_TRUE(writer.ok());
  AvroValue wrong_type = AvroValue::CreateLong(1);
  EXPECT_EQ(writer->Append(std::move(wrong_type)).code(),
            absl::StatusCode::kInvalidArgument);
  // A rejection must not consume the caller's value.
  EXPECT_FALSE(wrong_type.IsNull());
  EXPECT_EQ(wrong_type.GetLong().value_or(0), 1);
  EXPECT_FALSE(writer->IsFinished());
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
  // The streaming path fails as soon as the magic bytes mismatch.
  DataFileReader reader = DataFileReader::Create();
  ASSERT_TRUE(reader.Feed("XXXX").ok());
  EXPECT_FALSE(reader.NextReady().ok());
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
  std::string bytes = WriteFile(*writer_schema, Codec::kNull, {record});

  auto reader = DataFileReader::FromBytesWithSchema(*reader_schema, bytes);
  ASSERT_TRUE(reader.ok());
  auto value = reader->NextValue();
  ASSERT_TRUE(value.ok());
  EXPECT_EQ(value->GetRecordField("a")->GetLong().value_or(0), 12);
  EXPECT_EQ(value->GetRecordField("b")->GetString().value_or(""), "d");
  EXPECT_TRUE(*reader->WriterSchema() == *writer_schema);
}

TEST(ContainerTest, ProjectedReadDropsUnwantedFields) {
  auto writer_schema =
      AvroSchema::Parse(R"({"type": "record", "name": "R", "fields": [
          {"name": "keep", "type": "long"},
          {"name": "drop_map", "type": {"type": "map", "values": "long"}},
          {"name": "drop_blob", "type": "bytes"},
          {"name": "also_keep", "type": "string"}]})");
  auto projection =
      AvroSchema::Parse(R"({"type": "record", "name": "R", "fields": [
          {"name": "keep", "type": "long"},
          {"name": "also_keep", "type": "string"}]})");
  ASSERT_TRUE(writer_schema.ok() && projection.ok());

  std::vector<AvroValue> values;
  for (int i = 0; i < 40; ++i) {
    AvroValue tags = AvroValue::CreateMap();
    ASSERT_TRUE(tags.MapPut("x", AvroValue::CreateLong(i)).ok());
    AvroValue record = AvroValue::CreateRecord();
    ASSERT_TRUE(record.RecordPut("keep", AvroValue::CreateLong(i)).ok());
    ASSERT_TRUE(record.RecordPut("drop_map", tags).ok());
    ASSERT_TRUE(
        record.RecordPut("drop_blob", AvroValue::CreateBytes(std::string(32, 'z')))
            .ok());
    ASSERT_TRUE(record
                    .RecordPut("also_keep",
                               *AvroValue::CreateString("row-" + std::to_string(i)))
                    .ok());
    values.push_back(std::move(record));
  }
  std::string bytes = WriteFile(*writer_schema, Codec::kNull, values);

  auto reader = DataFileReader::FromBytesWithProjection(*projection, bytes);
  ASSERT_TRUE(reader.ok()) << reader.status();
  int seen = 0;
  while (true) {
    auto value = reader->NextValue();
    if (!value.ok()) break;
    EXPECT_EQ(value->GetRecordField("keep")->GetLong().value_or(-1), seen);
    EXPECT_EQ(value->GetRecordField("also_keep")->GetString().value_or(""),
              "row-" + std::to_string(seen));
    // Dropped fields are absent, not null-filled.
    EXPECT_FALSE(value->HasRecordField("drop_map").value_or(true));
    EXPECT_FALSE(value->HasRecordField("drop_blob").value_or(true));
    ++seen;
  }
  // Reading every value proves the skipped bytes were counted exactly: a
  // miscount would put the next datum at the wrong offset in the block.
  EXPECT_EQ(seen, 40);
  EXPECT_TRUE(reader->AtEnd());
}

TEST(ContainerTest, ProjectionNotMatchingWriterSchemaFails) {
  auto writer_schema =
      AvroSchema::Parse(R"({"type": "record", "name": "R", "fields": [
          {"name": "a", "type": "long"}]})");
  auto bogus =
      AvroSchema::Parse(R"({"type": "record", "name": "R", "fields": [
          {"name": "missing", "type": "long"}]})");
  ASSERT_TRUE(writer_schema.ok() && bogus.ok());

  AvroValue record = AvroValue::CreateRecord();
  ASSERT_TRUE(record.RecordPut("a", AvroValue::CreateLong(1)).ok());
  std::string bytes = WriteFile(*writer_schema, Codec::kNull, {record});

  EXPECT_FALSE(DataFileReader::FromBytesWithProjection(*bogus, bytes).ok());
}

TEST(DatumTest, ProjectedDatumDecode) {
  auto writer_schema =
      AvroSchema::Parse(R"({"type": "record", "name": "R", "fields": [
          {"name": "a", "type": "long"},
          {"name": "b", "type": "string"}]})");
  auto projection =
      AvroSchema::Parse(R"({"type": "record", "name": "R", "fields": [
          {"name": "b", "type": "string"}]})");
  ASSERT_TRUE(writer_schema.ok() && projection.ok());

  AvroValue record = AvroValue::CreateRecord();
  ASSERT_TRUE(record.RecordPut("a", AvroValue::CreateLong(7)).ok());
  ASSERT_TRUE(record.RecordPut("b", *AvroValue::CreateString("hi")).ok());
  auto encoded = EncodeDatum(*writer_schema, record);
  ASSERT_TRUE(encoded.ok());

  auto compiled = AvroProjection::Compile(*writer_schema, *projection);
  ASSERT_TRUE(compiled.ok()) << compiled.status();
  auto decoded = compiled->DecodeDatum(*encoded);
  ASSERT_TRUE(decoded.ok()) << decoded.status();
  EXPECT_EQ(decoded->GetRecordField("b")->GetString().value_or(""), "hi");
  EXPECT_FALSE(decoded->HasRecordField("a").value_or(true));
}

// -- Zero-copy stream reader --------------------------------------------------

// Test stream that yields a string in fixed-size chunks and records whether
// the reader ever calls BackUp (it must not; see ZeroCopyInputStream docs).
class ChunkedStream final : public ZeroCopyInputStream {
 public:
  ChunkedStream(std::string data, size_t chunk_size)
      : data_(std::move(data)), chunk_size_(chunk_size) {}

  bool Next(const void** data, int* size) override {
    if (pos_ >= data_.size()) return false;
    size_t n = std::min(chunk_size_, data_.size() - pos_);
    *data = data_.data() + pos_;
    *size = static_cast<int>(n);
    pos_ += n;
    return true;
  }
  void BackUp(int count) override {
    backed_up_ = true;
    pos_ -= static_cast<size_t>(count);
  }
  int64_t ByteCount() const override { return static_cast<int64_t>(pos_); }

  bool backed_up() const { return backed_up_; }

 private:
  std::string data_;
  size_t chunk_size_;
  size_t pos_ = 0;
  bool backed_up_ = false;
};

TEST(StreamReaderTest, RoundtripSingleByteChunks) {
  // One-byte chunks hit every possible split point (mid-magic, mid-varint,
  // mid-payload, mid-marker).
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  std::vector<AvroValue> values;
  for (int i = 0; i < 50; ++i) values.push_back(MakeUser(i, "stream"));
  ChunkedStream stream(WriteFile(*schema, Codec::kDeflate, values), 1);

  auto reader = DataFileStreamReader::Create(&stream);
  ASSERT_TRUE(reader.ok());
  EXPECT_TRUE(*reader->WriterSchema() == *schema);
  int seen = 0;
  while (reader->HasNext().value_or(false)) {
    auto value = reader->NextValue();
    ASSERT_TRUE(value.ok());
    EXPECT_EQ(value->GetRecordField("id")->GetLong().value_or(-1), seen);
    ++seen;
  }
  EXPECT_EQ(seen, 50);
  // Clean end: HasNext is false (not an error) and NextValue is benign.
  EXPECT_FALSE(reader->HasNext().value_or(true));
  EXPECT_EQ(reader->NextValue().status().code(),
            absl::StatusCode::kOutOfRange);
  EXPECT_FALSE(stream.backed_up());
}

TEST(StreamReaderTest, StreamAdapterCompilesAndDelegates) {
  // The header-only adapter is meant for protobuf's ZeroCopyInputStream;
  // any type with the same shape proves the delegation.
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  AvroValue value = MakeUser(3, "adapted");
  ChunkedStream stream(WriteFile(*schema, Codec::kNull, {value}), 16);
  StreamAdapter<ChunkedStream> adapter(&stream);

  auto reader = DataFileStreamReader::Create(&adapter);
  ASSERT_TRUE(reader.ok());
  EXPECT_TRUE(*reader->NextValue() == value);
  EXPECT_FALSE(reader->HasNext().value_or(true));
}

TEST(StreamReaderTest, ReaderSchemaResolution) {
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
  ChunkedStream stream(WriteFile(*writer_schema, Codec::kNull, {record}), 5);

  auto reader = DataFileStreamReader::CreateWithReaderSchema(*reader_schema,
                                                             &stream);
  ASSERT_TRUE(reader.ok());
  auto value = reader->NextValue();
  ASSERT_TRUE(value.ok());
  EXPECT_EQ(value->GetRecordField("a")->GetLong().value_or(0), 12);
  EXPECT_EQ(value->GetRecordField("b")->GetString().value_or(""), "d");
  EXPECT_TRUE(*reader->WriterSchema() == *writer_schema);
}

TEST(StreamReaderTest, EmptyFileEndsCleanly) {
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  ChunkedStream stream(WriteFile(*schema, Codec::kNull, {}), 3);

  auto reader = DataFileStreamReader::Create(&stream);
  ASSERT_TRUE(reader.ok());
  EXPECT_TRUE(*reader->WriterSchema() == *schema);
  EXPECT_FALSE(reader->HasNext().value_or(true));
  EXPECT_EQ(reader->NextValue().status().code(),
            absl::StatusCode::kOutOfRange);
}

TEST(StreamReaderTest, GarbageAndEmptyStreamsFailAtCreate) {
  ChunkedStream garbage("not an avro file", 4);
  EXPECT_EQ(DataFileStreamReader::Create(&garbage).status().code(),
            absl::StatusCode::kInvalidArgument);

  ChunkedStream empty("", 4);
  EXPECT_EQ(DataFileStreamReader::Create(&empty).status().code(),
            absl::StatusCode::kInvalidArgument);

  EXPECT_EQ(DataFileStreamReader::Create(nullptr).status().code(),
            absl::StatusCode::kInvalidArgument);
}

TEST(StreamReaderTest, TruncatedInFirstBlockFailsOnFirstRead) {
  // Create parses only up to the header, so a stream truncated inside the
  // first block constructs fine and fails fatally on the first read.
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  std::string bytes = WriteFile(*schema, Codec::kNull, {MakeUser(1, "t")});
  ChunkedStream stream(bytes.substr(0, bytes.size() - 8), 7);

  auto reader = DataFileStreamReader::Create(&stream);
  ASSERT_TRUE(reader.ok());
  auto value = reader->NextValue();
  EXPECT_EQ(value.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_THAT(std::string(value.status().message()), HasSubstr("Truncated"));
}

TEST(StreamReaderTest, SetMaxBlockSizeAppliesToFirstBlock) {
  // Because Create stops pulling at the header, a cap set right after
  // Create still governs the first block. One-byte chunks guarantee no
  // block framing sneaks into the chunk that completes the header.
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  ChunkedStream stream(WriteFile(*schema, Codec::kNull, {MakeUser(1, "b")}),
                       1);

  auto reader = DataFileStreamReader::Create(&stream);
  ASSERT_TRUE(reader.ok());
  ASSERT_TRUE(reader->SetMaxBlockSize(1).ok());
  auto ready = reader->HasNext();
  ASSERT_FALSE(ready.ok());
  EXPECT_THAT(std::string(ready.status().message()),
              HasSubstr("exceeds the maximum"));
}

// Delegates to ChunkedStream but yields an empty (true, size 0) chunk
// before every real one, which the interface explicitly allows.
class EmptyChunkInterleavingStream final : public ZeroCopyInputStream {
 public:
  EmptyChunkInterleavingStream(std::string data, size_t chunk_size)
      : inner_(std::move(data), chunk_size) {}

  bool Next(const void** data, int* size) override {
    yield_empty_ = !yield_empty_;
    if (yield_empty_) {
      *data = "";
      *size = 0;
      return true;
    }
    return inner_.Next(data, size);
  }
  void BackUp(int count) override { inner_.BackUp(count); }
  int64_t ByteCount() const override { return inner_.ByteCount(); }

 private:
  ChunkedStream inner_;
  bool yield_empty_ = false;
};

// Never ends and never yields a byte: the degenerate stream a stuck or
// buggy implementation produces.
class ForeverEmptyStream final : public ZeroCopyInputStream {
 public:
  bool Next(const void** data, int* size) override {
    *data = "";
    *size = 0;
    return true;
  }
  void BackUp(int count) override {}
  int64_t ByteCount() const override { return 0; }
};

class NegativeSizeStream final : public ZeroCopyInputStream {
 public:
  bool Next(const void** data, int* size) override {
    *data = "";
    *size = -1;
    return true;
  }
  void BackUp(int count) override {}
  int64_t ByteCount() const override { return 0; }
};

TEST(StreamReaderTest, OccasionalEmptyChunksAreSkipped) {
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  AvroValue value = MakeUser(9, "gappy");
  EmptyChunkInterleavingStream stream(
      WriteFile(*schema, Codec::kNull, {value}), 3);

  auto reader = DataFileStreamReader::Create(&stream);
  ASSERT_TRUE(reader.ok());
  EXPECT_TRUE(*reader->NextValue() == value);
  EXPECT_FALSE(reader->HasNext().value_or(true));
}

TEST(StreamReaderTest, ForeverEmptyStreamFailsInsteadOfLivelocking) {
  ForeverEmptyStream stream;
  auto reader = DataFileStreamReader::Create(&stream);
  ASSERT_FALSE(reader.ok());
  EXPECT_THAT(std::string(reader.status().message()),
              HasSubstr("consecutive empty chunks"));
}

TEST(StreamReaderTest, NegativeChunkSizeFails) {
  NegativeSizeStream stream;
  auto reader = DataFileStreamReader::Create(&stream);
  ASSERT_FALSE(reader.ok());
  EXPECT_THAT(std::string(reader.status().message()),
              HasSubstr("negative chunk size"));
}

TEST(StreamReaderTest, TruncatedInLaterBlockFailsMidIteration) {
  // Enough values for several 1024-value flush batches, so truncating the
  // tail leaves earlier blocks intact: Create succeeds, iteration fails
  // partway with a fatal (kInvalidArgument) error, never a silent short
  // read.
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  std::vector<AvroValue> values;
  for (int i = 0; i < 3000; ++i) values.push_back(MakeUser(i, "x"));
  std::string bytes = WriteFile(*schema, Codec::kNull, values);
  ChunkedStream stream(bytes.substr(0, bytes.size() - 8), 4096);

  auto reader = DataFileStreamReader::Create(&stream);
  ASSERT_TRUE(reader.ok());
  int seen = 0;
  absl::Status error = absl::OkStatus();
  while (true) {
    auto ready = reader->HasNext();
    if (!ready.ok()) {
      error = ready.status();
      break;
    }
    if (!*ready) break;
    auto value = reader->NextValue();
    if (!value.ok()) {
      error = value.status();
      break;
    }
    ++seen;
  }
  EXPECT_EQ(error.code(), absl::StatusCode::kInvalidArgument);
  EXPECT_THAT(std::string(error.message()), HasSubstr("Truncated"));
  EXPECT_GT(seen, 0);
  EXPECT_LT(seen, 3000);
}

TEST(ContainerTest, PathRoundtrip) {
  std::string path = ::testing::TempDir() + "/avro_bridge_test_data.avro";

  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  auto writer = DataFileWriter::Create(*schema, Codec::kDeflate);
  ASSERT_TRUE(writer.ok());
  AvroValue value = MakeUser(11, "disk");
  ASSERT_TRUE(writer->Append(value).ok());
  ASSERT_TRUE(writer->FinishToPath(path).ok());
  EXPECT_TRUE(writer->IsFinished());

  auto reader = DataFileReader::FromPath(path);
  ASSERT_TRUE(reader.ok());
  EXPECT_TRUE(*reader->NextValue() == value);

  auto resolved = DataFileReader::FromPathWithSchema(*schema, path);
  ASSERT_TRUE(resolved.ok());
  EXPECT_TRUE(*resolved->NextValue() == value);
}

TEST(ContainerTest, FinishToPathAfterTakeBytesFails) {
  // TakeBytes only returns bytes once a 1024-value batch has flushed; after
  // that, FinishToPath would write a file missing those bytes and must
  // refuse (kFailedPrecondition) instead of reporting a bogus success.
  std::string path =
      ::testing::TempDir() + "/avro_bridge_test_finish_after_take.avro";
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  auto writer = DataFileWriter::Create(*schema, Codec::kNull);
  ASSERT_TRUE(writer.ok());
  for (int i = 0; i < 1024; ++i) {
    ASSERT_TRUE(writer->Append(MakeUser(i, "x")).ok());
  }
  auto taken = writer->TakeBytes();
  ASSERT_TRUE(taken.ok());
  ASSERT_FALSE(taken->empty());

  EXPECT_EQ(writer->FinishToPath(path).code(),
            absl::StatusCode::kFailedPrecondition);
  // The refusal leaves the writer usable: the caller can still assemble the
  // complete file from the taken bytes plus Finish.
  auto tail = writer->Finish();
  ASSERT_TRUE(tail.ok());
  auto reader = DataFileReader::FromBytes(*taken + *tail);
  ASSERT_TRUE(reader.ok());
}

TEST(ContainerTest, FinishToPathAfterEmptyTakeBytesSucceeds) {
  // A TakeBytes that returned nothing took nothing: the path convenience
  // still writes a complete file.
  std::string path =
      ::testing::TempDir() + "/avro_bridge_test_finish_after_empty_take.avro";
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  auto writer = DataFileWriter::Create(*schema, Codec::kNull);
  ASSERT_TRUE(writer.ok());
  AvroValue value = MakeUser(1, "kept");
  ASSERT_TRUE(writer->Append(value).ok());
  auto taken = writer->TakeBytes();
  ASSERT_TRUE(taken.ok());
  ASSERT_TRUE(taken->empty());

  ASSERT_TRUE(writer->FinishToPath(path).ok());
  auto reader = DataFileReader::FromPath(path);
  ASSERT_TRUE(reader.ok());
  EXPECT_TRUE(*reader->NextValue() == value);
}

TEST(ContainerTest, MissingFileFails) {
  EXPECT_FALSE(DataFileReader::FromPath("/nonexistent/x.avro").ok());
}

TEST(ContainerTest, OcfMagicPresent) {
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  std::string bytes = WriteFile(*schema, Codec::kNull, {MakeUser(1, "m")});
  ASSERT_GE(bytes.size(), 4);
  EXPECT_EQ(bytes.substr(0, 4), std::string("Obj\x01", 4));
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
  auto bytes = writer->Finish();  // No Append calls: header only.
  ASSERT_TRUE(bytes.ok());
  auto reader = DataFileReader::FromBytes(*bytes);
  ASSERT_TRUE(reader.ok());
  EXPECT_TRUE(*reader->WriterSchema() == *schema);
  EXPECT_FALSE(reader->NextReady().value_or(true));
  EXPECT_TRUE(reader->AtEnd());
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

TEST(AvroDatumReaderTest, DecodesTheSameValuesAsTheFreeFunction) {
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  auto reader = AvroDatumReader::Create(*schema);
  ASSERT_TRUE(reader.ok());

  for (int64_t id : {int64_t{0}, int64_t{7}, std::numeric_limits<int64_t>::max()}) {
    AvroValue value = MakeUser(id, "bob");
    auto encoded = EncodeDatum(*schema, value);
    ASSERT_TRUE(encoded.ok());
    auto by_reader = reader->Decode(*encoded);
    ASSERT_TRUE(by_reader.ok());
    EXPECT_TRUE(*by_reader == value);
  }
}

TEST(AvroDatumReaderTest, DecodeIntoReusesAndOverwrites) {
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  auto reader = AvroDatumReader::Create(*schema);
  ASSERT_TRUE(reader.ok());
  auto first = EncodeDatum(*schema, MakeUser(1, "ann"));
  auto second = EncodeDatum(*schema, MakeUser(2, "bob"));
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());

  AvroValue value = AvroValue::CreateNull();
  ASSERT_TRUE(reader->DecodeInto(*first, &value).ok());
  EXPECT_TRUE(value == MakeUser(1, "ann"));
  // The second decode must fully replace the first, not merge into it.
  ASSERT_TRUE(reader->DecodeInto(*second, &value).ok());
  EXPECT_TRUE(value == MakeUser(2, "bob"));
}

TEST(AvroDatumReaderTest, DecodeIntoRejectsNull) {
  auto schema = AvroSchema::Parse("\"int\"");
  ASSERT_TRUE(schema.ok());
  auto reader = AvroDatumReader::Create(*schema);
  ASSERT_TRUE(reader.ok());
  auto encoded = EncodeDatum(*schema, AvroValue::CreateInt(1));
  ASSERT_TRUE(encoded.ok());
  EXPECT_EQ(reader->DecodeInto(*encoded, nullptr).code(),
            absl::StatusCode::kInvalidArgument);
}

TEST(AvroDatumReaderTest, RejectsTrailingBytesAndTruncation) {
  auto schema = AvroSchema::Parse("\"int\"");
  ASSERT_TRUE(schema.ok());
  auto reader = AvroDatumReader::Create(*schema);
  ASSERT_TRUE(reader.ok());
  auto encoded = EncodeDatum(*schema, AvroValue::CreateInt(7));
  ASSERT_TRUE(encoded.ok());

  std::string with_trailing = *encoded + std::string("\xde\xad", 2);
  AvroValue value = AvroValue::CreateNull();
  EXPECT_FALSE(reader->Decode(with_trailing).ok());
  EXPECT_FALSE(reader->DecodeInto(with_trailing, &value).ok());
  EXPECT_FALSE(reader->Decode("").ok());
}

TEST(AvroDatumReaderTest, ReportsWriterSchema) {
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  auto reader = AvroDatumReader::Create(*schema);
  ASSERT_TRUE(reader.ok());
  auto reported = reader->WriterSchema();
  ASSERT_TRUE(reported.ok());
  EXPECT_TRUE(*reported == *schema);
}

TEST(AvroDatumReaderTest, MovedFromReaderFailsInsteadOfDecoding) {
  auto schema = AvroSchema::Parse("\"int\"");
  ASSERT_TRUE(schema.ok());
  auto reader = AvroDatumReader::Create(*schema);
  ASSERT_TRUE(reader.ok());
  auto encoded = EncodeDatum(*schema, AvroValue::CreateInt(1));
  ASSERT_TRUE(encoded.ok());

  AvroDatumReader moved = *std::move(reader);
  ASSERT_TRUE(moved.Decode(*encoded).ok());
  // A use-after-move must error rather than behave like a fresh reader.
  AvroValue value = AvroValue::CreateNull();
  EXPECT_FALSE(reader->Decode(*encoded).ok());
  EXPECT_FALSE(reader->DecodeInto(*encoded, &value).ok());
  EXPECT_FALSE(reader->WriterSchema().ok());
}


}  // namespace
}  // namespace security::avro
