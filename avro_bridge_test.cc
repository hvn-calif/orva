#include "avro_bridge.h"

#include <cstddef>
#include <cstring>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"

namespace security::avro {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

// Every setting this binding exposes is a process-global that only the first
// call sets, so one process can only ever observe one value of each. CMake
// therefore builds this source twice: `avro_bridge_test` runs it at the
// bridge's defaults, and `avro_bridge_strict_test` defines the macro below and
// flips every setting to its strict reading before any test body runs. Tests
// whose expected outcome depends on a setting branch on `kStrictSettings`
// rather than being duplicated.
#ifdef AVRO_BRIDGE_TEST_STRICT_SETTINGS
constexpr bool kStrictSettings = true;
#else
constexpr bool kStrictSettings = false;
#endif

// Installs the strict settings, in the strict binary only. This is where a
// caller wanting them has to do it: the settings are first-call-wins and the
// bridge installs its avrocpp-compatible defaults on its first Avro operation,
// so anything set after that loses. gtest runs environment SetUp before the
// first test body and nothing in this binary touches the bridge earlier, so
// these calls get there first -- which is what the return values assert.
class SettingsEnvironment : public ::testing::Environment {
 public:
  void SetUp() override {
    if (!kStrictSettings) return;
    ASSERT_TRUE(SetRejectTrailingBytes(true));
    ASSERT_FALSE(SetNonUtf8StringAsBytes(false));
    ASSERT_FALSE(SetUuidAsString(false));
  }
};

const ::testing::Environment* const kSettingsEnvironment =
    ::testing::AddGlobalTestEnvironment(new SettingsEnvironment);

// One Avro long, zigzag-varint encoded: a length prefix, a union branch index,
// an enum position or a block count, depending on where it sits.
std::string Varint(int64_t value) {
  uint64_t n = (static_cast<uint64_t>(value) << 1) ^
               static_cast<uint64_t>(value >> 63);
  std::string out;
  do {
    uint8_t byte = n & 0x7f;
    n >>= 7;
    if (n != 0) byte |= 0x80;
    out.push_back(static_cast<char>(byte));
  } while (n != 0);
  return out;
}

// One Avro string or bytes datum: the length prefix, then the payload.
std::string LengthPrefixed(absl::string_view payload) {
  return Varint(static_cast<int64_t>(payload.size())) + std::string(payload);
}

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

// -- Closed avrocpp divergences --------------------------------------------
// The differential fuzzer found each of these as a place where this binding and
// avrocpp disagreed, and each is now closed by a patch in orva's `patches/`.
// The tests below pin the binding's half in the always-built suite; the
// differential half, which needs avrocpp linked, stays in
// avro_bytes_fuzz_test.cc behind AVRO_BUILD_FUZZERS. See
// doc/specs/DivergenceClosure.md for the series and its policy.

// Asserts that the settings in force are the ones this build asked for, before
// any test relies on them. Reading a first-call-wins setting means calling its
// setter and seeing what comes back: the value actually in effect.
//
// What this catches is the strict binary losing the race -- something touching
// the bridge before SettingsEnvironment runs, or the environment registration
// being dropped. `SetNonUtf8StringAsBytes` is asked for false there and must
// report false, which holds only if SetUp got in before the bridge installed its
// own default of true.
//
// What it cannot catch is `AVRO_BRIDGE_TEST_STRICT_SETTINGS` never reaching the
// strict target: `kStrictSettings` would be false, the expectations here would
// flip with it, and the binary would become a duplicate of the default one
// rather than a failing one. Nothing inside the source can tell which build it
// is meant to be; that one is on the CMake block.
TEST(DatumTest, SettingsMatchTheBuild) {
  EXPECT_EQ(SetRejectTrailingBytes(kStrictSettings), kStrictSettings);
  EXPECT_EQ(SetNonUtf8StringAsBytes(!kStrictSettings), !kStrictSettings);
  EXPECT_EQ(SetUuidAsString(!kStrictSettings), !kStrictSettings);
}

// A1, and the most serious of the eleven: a truncated buffer decoded into
// values that were never on the wire. Three arms of apache-avro's decoder
// treated end of input as a value rather than an error, so a record of two
// booleans decoded from *zero bytes* into two nulls with an OK status, where
// avrocpp reports "EOF reached". None of the three fabricated values inhabits
// the schema it was decoded under.
TEST(DatumTest, TruncatedInputDoesNotFabricateValues) {
  for (const char* text : {
           R"("boolean")",
           R"(["int"])",
           R"({"type":"record","name":"R","fields":[)"
           R"({"name":"a","type":"boolean"},{"name":"b","type":"boolean"}]})",
       }) {
    auto schema = AvroSchema::Parse(text);
    ASSERT_TRUE(schema.ok()) << text << ": " << schema.status();
    EXPECT_FALSE(DecodeDatum(*schema, "").ok()) << text;
  }

  // The third fabrication site, and the one nothing had reached: an empty
  // buffer under "string" already failed at the missing length prefix, so no
  // test exercised a prefix with too few bytes behind it.
  auto string_schema = AvroSchema::Parse(R"("string")");
  ASSERT_TRUE(string_schema.ok());
  EXPECT_FALSE(DecodeDatum(*string_schema, "").ok());
  EXPECT_FALSE(DecodeDatum(*string_schema, Varint(2) + "a").ok());

  // "null" still decodes from an empty buffer, and has to: a null datum
  // occupies no bytes. This is why the fix could not be "reject an empty
  // buffer".
  auto null_schema = AvroSchema::Parse(R"("null")");
  ASSERT_TRUE(null_schema.ok());
  auto decoded = DecodeDatum(*null_schema, "");
  ASSERT_TRUE(decoded.ok()) << decoded.status();
  EXPECT_TRUE(decoded->IsNull());
}

// A2. No branch index is in range for a union with no members, so nothing
// encodes into one and no bytes decode under one, yet `[]` used to parse and
// render straight back -- producing a schema avrocpp cannot read.
TEST(AvroSchemaTest, EmptyUnionIsRejected) {
  for (const char* text : {
           R"([])",
           R"([[]])",
           R"({"type":"record","name":"R","fields":[{"name":"a","type":[]}]})",
           R"({"type":"array","items":[]})",
           R"({"type":"map","values":[]})",
       }) {
    EXPECT_EQ(AvroSchema::Parse(text).status().code(),
              absl::StatusCode::kInvalidArgument)
        << text;
  }

  // A one-branch union stays legal, because index 0 is in range and it does
  // have a valid encoding. This is what keeps the fix from over-reaching.
  auto one_branch = AvroSchema::Parse(R"(["int"])");
  ASSERT_TRUE(one_branch.ok()) << one_branch.status();
  auto decoded = DecodeDatum(*one_branch, Varint(0) + Varint(42));
  ASSERT_TRUE(decoded.ok()) << decoded.status();
  EXPECT_EQ(decoded->GetUnionValue()->GetInt().value_or(0), 42);
}

// A3, the same defect as A2 on a different construct: no symbol index is in
// range for an enum with no symbols.
//
// The check covers the parse path only. `EnumSchema` has public fields and a
// builder, unlike `UnionSchema` whose fields are crate-private, so Rust code
// can still hand-build one with no symbols. Untrusted input arrives by parsing,
// which is what this covers.
TEST(AvroSchemaTest, EmptyEnumIsRejected) {
  for (const char* text : {
           R"({"type":"enum","name":"E","symbols":[]})",
           R"({"type":"enum","name":"E","namespace":"ns","symbols":[]})",
           R"({"type":"record","name":"R","fields":[{"name":"a","type":)"
           R"({"type":"enum","name":"E","symbols":[]}}]})",
           R"({"type":"array","items":)"
           R"({"type":"enum","name":"E","symbols":[]}})",
       }) {
    EXPECT_EQ(AvroSchema::Parse(text).status().code(),
              absl::StatusCode::kInvalidArgument)
        << text;
  }

  // One symbol is enough: index 0 is in range.
  auto one_symbol =
      AvroSchema::Parse(R"({"type":"enum","name":"E","symbols":["A"]})");
  ASSERT_TRUE(one_symbol.ok()) << one_symbol.status();
  auto decoded = DecodeDatum(*one_symbol, Varint(0));
  ASSERT_TRUE(decoded.ok()) << decoded.status();
  EXPECT_EQ(decoded->GetEnumSymbol().value_or(""), "A");
}

// A4. A length prefix of zero under a decimal schema means an empty unscaled
// byte array, which both engines accept. avrocpp re-encodes it to the byte it
// came from; this binding used to hand back a value that said it was a decimal
// and could then be neither read nor re-encoded, both failing with the same
// sign-extension message, so a caller had no way to use or forward the result.
TEST(DatumTest, EmptyDecimalRoundTrips) {
  auto schema = AvroSchema::Parse(
      R"({"type":"bytes","logicalType":"decimal","precision":9,"scale":2})");
  ASSERT_TRUE(schema.ok()) << schema.status();

  const std::string zero_length = Varint(0);
  auto decoded = DecodeDatum(*schema, zero_length);
  ASSERT_TRUE(decoded.ok()) << decoded.status();
  EXPECT_TRUE(decoded->IsDecimal());

  auto bytes = decoded->GetDecimalBytes();
  ASSERT_TRUE(bytes.ok()) << bytes.status();
  EXPECT_EQ(*bytes, "");

  auto reencoded = EncodeDatum(*schema, *decoded);
  ASSERT_TRUE(reencoded.ok()) << reencoded.status();
  EXPECT_EQ(*reencoded, zero_length);

  // A one-byte unscaled value was never affected and still round-trips, so the
  // fix did not reach past the empty case.
  const std::string unscaled_42(1, '\x2a');
  const std::string one_byte = LengthPrefixed(unscaled_42);
  auto small = DecodeDatum(*schema, one_byte);
  ASSERT_TRUE(small.ok()) << small.status();
  EXPECT_EQ(small->GetDecimalBytes().value_or(""), unscaled_42);
  EXPECT_EQ(EncodeDatum(*schema, *small).value_or(""), one_byte);
}

// B1, the one closure that is this binding's own rather than an apache-avro
// patch: apache-avro's decoder already stops at the end of the first datum like
// avrocpp, and the rejection was something the binding added.
//
// Off is the default, which is avrocpp's behaviour, so code being migrated
// that hands a padded or over-allocated buffer to a decode keeps working. On
// is the stricter reading and stays reachable, because leftover bytes usually
// do mean framing has gone wrong.
TEST(DatumTest, TrailingBytesFollowTheSetting) {
  auto schema = AvroSchema::Parse(R"("int")");
  ASSERT_TRUE(schema.ok());
  auto encoded = EncodeDatum(*schema, AvroValue::CreateInt(7));
  ASSERT_TRUE(encoded.ok());
  const std::string padded = *encoded + std::string("\xde\xad", 2);

  auto decoded = DecodeDatum(*schema, padded);
  if (kStrictSettings) {
    ASSERT_FALSE(decoded.ok());
    EXPECT_THAT(decoded.status().message(), HasSubstr("trailing bytes"));
  } else {
    ASSERT_TRUE(decoded.ok()) << decoded.status();
    EXPECT_EQ(decoded->GetInt().value_or(0), 7);
  }

  // The setting is checked at every decode entry point, not just the plain one.
  auto long_schema = AvroSchema::Parse(R"("long")");
  ASSERT_TRUE(long_schema.ok());
  EXPECT_EQ(DecodeDatumResolved(*schema, *long_schema, padded).ok(),
            !kStrictSettings);
  EXPECT_EQ(DecodeDatumSchemata(*schema, absl::MakeConstSpan(&*schema, 1),
                                padded)
                .ok(),
            !kStrictSettings);

  // A correctly framed datum decodes either way, so what the setting governs is
  // the leftovers rather than the decode.
  auto clean = DecodeDatum(*schema, *encoded);
  ASSERT_TRUE(clean.ok()) << clean.status();
  EXPECT_EQ(clean->GetInt().value_or(0), 7);

  // And bytes *missing* from a datum are an error either way. Ignoring bytes
  // left over after a complete datum and accepting a datum cut short are
  // different things, and conflating them would undo A1 above.
  auto string_schema = AvroSchema::Parse(R"("string")");
  ASSERT_TRUE(string_schema.ok());
  EXPECT_FALSE(DecodeDatum(*string_schema, Varint(2) + "a").ok());
}

// Not a closure but a default this binding chose: avrocpp copies a `string`'s
// wire bytes into a byte-oriented std::string without validating them, and
// Java's Utf8 holds a raw byte[], so files carrying non-UTF-8 round-trip
// through both. Rejecting them here would make that data unreadable, so the
// bytes are kept in a distinct variant that re-encodes to exactly what arrived.
TEST(DatumTest, NonUtf8StringFollowsTheSetting) {
  auto schema = AvroSchema::Parse(R"("string")");
  ASSERT_TRUE(schema.ok());
  const std::string datum = LengthPrefixed(std::string("\xff\xfe", 2));

  auto decoded = DecodeDatum(*schema, datum);
  if (kStrictSettings) {
    ASSERT_FALSE(decoded.ok());
    EXPECT_THAT(decoded.status().message(), HasSubstr("utf-8"));
  } else {
    ASSERT_TRUE(decoded.ok()) << decoded.status();
    EXPECT_TRUE(decoded->IsBytes());
    EXPECT_EQ(decoded->GetBytes().value_or(""), std::string("\xff\xfe", 2));
    // Byte-exact: a value read and written back is the value that arrived.
    EXPECT_EQ(EncodeDatum(*schema, *decoded).value_or(""), datum);
  }

  // Valid UTF-8 decodes as a string either way, so the setting reaches only the
  // invalid case.
  auto text = DecodeDatum(*schema, LengthPrefixed("hi"));
  ASSERT_TRUE(text.ok()) << text.status();
  EXPECT_EQ(text->GetString().value_or(""), "hi");
}

// Also a default rather than a closure. Avro defines `uuid` as an annotation on
// `string` and a reader may leave it uninterpreted, which is what avrocpp
// does: it never parses or validates one. Parsing it rewrites the bytes into
// canonical form, reinterprets any 16-byte string as a raw uuid, and rejects
// text other implementations wrote, so the annotation is left uninterpreted
// here too.
TEST(DatumTest, UuidFollowsTheSetting) {
  auto schema = AvroSchema::Parse(R"({"type":"string","logicalType":"uuid"})");
  ASSERT_TRUE(schema.ok()) << schema.status();

  // Text that is not a uuid at all. avrocpp accepts it; parsing rejects it.
  const std::string not_a_uuid = LengthPrefixed("nope");
  auto decoded = DecodeDatum(*schema, not_a_uuid);
  if (kStrictSettings) {
    EXPECT_FALSE(decoded.ok());
  } else {
    ASSERT_TRUE(decoded.ok()) << decoded.status();
    EXPECT_EQ(decoded->GetString().value_or(""), "nope");
    EXPECT_EQ(EncodeDatum(*schema, *decoded).value_or(""), not_a_uuid);
  }

  // A well-formed uuid whose hex is upper case, which the canonical form is
  // not. Parsing rewrites it, so the value read back is not the value that
  // arrived; left uninterpreted, the text survives.
  const std::string non_canonical =
      LengthPrefixed("6F2B0E76-4D3D-4F8E-9D3A-2E1B8A7C6D5E");
  auto uuid = DecodeDatum(*schema, non_canonical);
  ASSERT_TRUE(uuid.ok()) << uuid.status();
  if (kStrictSettings) {
    EXPECT_EQ(uuid->GetUuid().value_or(""),
              "6f2b0e76-4d3d-4f8e-9d3a-2e1b8a7c6d5e");
  } else {
    EXPECT_EQ(uuid->GetString().value_or(""),
              "6F2B0E76-4D3D-4F8E-9D3A-2E1B8A7C6D5E");
    EXPECT_EQ(EncodeDatum(*schema, *uuid).value_or(""), non_canonical);
  }
}

// Avro carries raw IEEE-754 bits for `float` and `double`, so a NaN payload and
// the sign of a zero are data rather than noise. Measured: neither engine
// canonicalises either, so this is not a divergence -- see
// AvroBytes.NanPayloadsAndSignedZeroSurviveBothEngines for the avrocpp half.
// It is pinned because canonicalisation by either side would be a silent value
// change, and because the differential harness carries two divergence IDs for
// it, FLOAT_NAN_PAYLOAD and FLOAT_SIGNED_ZERO, that have never fired.
//
// The oracle is the encoded bytes, not operator==, and that is the second thing
// this pins. See the caveat on AvroValue::operator== in avro_bridge.h.
TEST(DatumTest, NonFiniteDoublesRoundTripBitExact) {
  auto schema = AvroSchema::Parse(R"("double")");
  ASSERT_TRUE(schema.ok());

  // A quiet NaN, then one whose payload no arithmetic would produce: the
  // signalling bit clear and low mantissa bits set. A canonicalising decoder
  // would return the first for both.
  const uint64_t kQuietNan = 0x7FF8000000000000ULL;
  const uint64_t kOddPayloadNan = 0x7FF0000000ABCDEFULL;
  const uint64_t kNegativeZero = 0x8000000000000000ULL;

  for (uint64_t bits : {kQuietNan, kOddPayloadNan, kNegativeZero}) {
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    auto encoded = EncodeDatum(*schema, AvroValue::CreateDouble(value));
    ASSERT_TRUE(encoded.ok()) << encoded.status();
    auto decoded = DecodeDatum(*schema, *encoded);
    ASSERT_TRUE(decoded.ok()) << decoded.status();

    auto out = decoded->GetDouble();
    ASSERT_TRUE(out.ok()) << out.status();
    uint64_t out_bits;
    std::memcpy(&out_bits, &*out, sizeof(out_bits));
    EXPECT_EQ(out_bits, bits);
    EXPECT_EQ(EncodeDatum(*schema, *decoded).value_or(""), *encoded);
  }
}

// Why the test above compares bytes rather than values: operator== delegates to
// Rust's PartialEq, which is IEEE-754 equality. It therefore reports a
// difference where the bits are identical, and no difference where they are not.
// Anything checking a float round-trip has to compare the encoded bytes.
TEST(AvroValueTest, EqualityIsIeeeEqualityForFloats) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(AvroValue::CreateDouble(nan) == AvroValue::CreateDouble(nan));

  // The other direction, and the one that hides data: two values that compare
  // equal and do not encode to the same bytes.
  EXPECT_TRUE(AvroValue::CreateDouble(-0.0) == AvroValue::CreateDouble(0.0));
  auto schema = AvroSchema::Parse(R"("double")");
  ASSERT_TRUE(schema.ok());
  EXPECT_NE(EncodeDatum(*schema, AvroValue::CreateDouble(-0.0)).value_or(""),
            EncodeDatum(*schema, AvroValue::CreateDouble(0.0)).value_or("x"));
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
// error paths, and the empty container file.
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

// -- Streaming container files ----------------------------------------------

// Writes `values` through the streaming writer, draining after each append,
// and returns the complete container file.
std::string StreamWrite(const AvroSchema& schema, Codec codec,
                        const std::vector<AvroValue>& values) {
  auto writer = StreamingDataFileWriter::Create(schema, codec);
  EXPECT_TRUE(writer.ok());
  std::string out;
  for (const AvroValue& value : values) {
    EXPECT_TRUE(writer->Append(value).ok());
    auto chunk = writer->TakeBytes();
    EXPECT_TRUE(chunk.ok());
    out += *chunk;
  }
  auto tail = writer->Finish();
  EXPECT_TRUE(tail.ok());
  out += *tail;
  return out;
}

class StreamingCodecTest : public ::testing::TestWithParam<Codec> {};

TEST_P(StreamingCodecTest, WriteReadRoundtrip) {
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  std::vector<AvroValue> values;
  values.push_back(MakeUser(1, "a"));
  values.push_back(MakeUser(2, "b"));
  std::string bytes = StreamWrite(*schema, GetParam(), values);

  auto reader = StreamingDataFileReader::FromBytes(bytes);
  ASSERT_TRUE(reader.ok());
  EXPECT_TRUE(reader->WriterSchema() == *schema);
  ASSERT_TRUE(reader->HasNext());
  EXPECT_TRUE(reader->HasNext());  // idempotent
  EXPECT_TRUE(*reader->NextValue() == values[0]);
  EXPECT_TRUE(*reader->NextValue() == values[1]);
  EXPECT_FALSE(reader->HasNext());
  EXPECT_EQ(reader->NextValue().status().code(), absl::StatusCode::kOutOfRange);
}

INSTANTIATE_TEST_SUITE_P(AllCodecs, StreamingCodecTest,
                         ::testing::Values(Codec::kNull, Codec::kDeflate,
                                           Codec::kSnappy, Codec::kZstandard));

TEST(StreamingContainerTest, FinishConsumes) {
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  auto writer = StreamingDataFileWriter::Create(*schema, Codec::kNull);
  ASSERT_TRUE(writer.ok());
  ASSERT_TRUE(writer->Append(MakeUser(1, "a")).ok());
  EXPECT_FALSE(writer->IsFinished());
  ASSERT_TRUE(writer->Finish().ok());
  EXPECT_TRUE(writer->IsFinished());
  EXPECT_FALSE(writer->Append(MakeUser(2, "b")).ok());
  EXPECT_FALSE(writer->TakeBytes().ok());
  EXPECT_FALSE(writer->Finish().ok());
  EXPECT_FALSE(writer->Schema().ok());
}

TEST(StreamingContainerTest, TakeBytesIncremental) {
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  auto writer = StreamingDataFileWriter::Create(*schema, Codec::kNull);
  ASSERT_TRUE(writer.ok());
  std::string file;
  const int n = 4000;
  for (int i = 0; i < n; ++i) {
    ASSERT_TRUE(writer->Append(MakeUser(i, "sensor")).ok());
    auto chunk = writer->TakeBytes();
    ASSERT_TRUE(chunk.ok());
    file += *chunk;
  }
  // Full blocks were flushed and drained before finish.
  EXPECT_FALSE(file.empty());
  auto tail = writer->Finish();
  ASSERT_TRUE(tail.ok());
  file += *tail;

  auto reader = StreamingDataFileReader::FromBytes(file);
  ASSERT_TRUE(reader.ok());
  int count = 0;
  while (reader->HasNext()) {
    auto value = reader->NextValue();
    ASSERT_TRUE(value.ok());
    EXPECT_EQ(value->GetRecordField("id")->GetLong().value_or(-1), count);
    ++count;
  }
  EXPECT_EQ(count, n);
}

TEST(StreamingContainerTest, GarbageFails) {
  EXPECT_FALSE(StreamingDataFileReader::FromBytes("not an avro file").ok());
  EXPECT_FALSE(StreamingDataFileReader::FromBytes("").ok());
}

TEST(StreamingContainerTest, ReaderSchemaResolution) {
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
  std::vector<AvroValue> values;
  values.push_back(record);
  std::string bytes = StreamWrite(*writer_schema, Codec::kNull, values);

  auto reader = StreamingDataFileReader::FromBytesWithSchema(*reader_schema, bytes);
  ASSERT_TRUE(reader.ok());
  auto value = reader->NextValue();
  ASSERT_TRUE(value.ok());
  EXPECT_EQ(value->GetRecordField("a")->GetLong().value_or(0), 12);
  EXPECT_EQ(value->GetRecordField("b")->GetString().value_or(""), "d");
  EXPECT_TRUE(reader->WriterSchema() == *writer_schema);
}

TEST(StreamingContainerTest, PathRoundtrip) {
  std::string path = ::testing::TempDir() + "/avro_streaming_test_data.avro";
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  // Produce a file with the buffered writer; read it with the streaming reader.
  auto writer = DataFileWriter::Create(*schema, Codec::kDeflate);
  ASSERT_TRUE(writer.ok());
  AvroValue value = MakeUser(11, "disk");
  ASSERT_TRUE(writer->Append(value).ok());
  ASSERT_TRUE(writer->WriteToPath(path).ok());

  auto reader = StreamingDataFileReader::FromPath(path);
  ASSERT_TRUE(reader.ok());
  ASSERT_TRUE(reader->HasNext());
  EXPECT_TRUE(*reader->NextValue() == value);
}

TEST(StreamingContainerTest, CrossParityWithBuffered) {
  auto schema = AvroSchema::Parse(kRecordSchema);
  ASSERT_TRUE(schema.ok());
  AvroValue a = MakeUser(1, "a");
  AvroValue b = MakeUser(2, "b");

  // Streaming output is read by the buffered reader.
  std::vector<AvroValue> values;
  values.push_back(a);
  values.push_back(b);
  std::string streamed = StreamWrite(*schema, Codec::kSnappy, values);
  auto buffered_reader = DataFileReader::FromBytes(streamed);
  ASSERT_TRUE(buffered_reader.ok());
  EXPECT_EQ(buffered_reader->Count(), 2);
  EXPECT_TRUE(*buffered_reader->NextValue() == a);
  EXPECT_TRUE(*buffered_reader->NextValue() == b);

  // Buffered output is read by the streaming reader.
  auto writer = DataFileWriter::Create(*schema, Codec::kDeflate);
  ASSERT_TRUE(writer.ok());
  ASSERT_TRUE(writer->Append(a).ok());
  ASSERT_TRUE(writer->Append(b).ok());
  auto bytes = writer->ToBytes();
  ASSERT_TRUE(bytes.ok());
  auto streaming_reader = StreamingDataFileReader::FromBytes(*bytes);
  ASSERT_TRUE(streaming_reader.ok());
  EXPECT_TRUE(*streaming_reader->NextValue() == a);
  EXPECT_TRUE(*streaming_reader->NextValue() == b);
  EXPECT_FALSE(streaming_reader->HasNext());
}

}  // namespace
}  // namespace security::avro
