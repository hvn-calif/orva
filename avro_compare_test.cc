#include "avro_compare.h"

#include <string>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "avro_bridge.h"

namespace security::avro_compare {
namespace {

namespace bridge = security::avro;

// CreateString only fails on invalid UTF-8, so an ASCII literal always
// succeeds; this just drops the StatusOr boilerplate.
bridge::AvroValue MustCreateString(absl::string_view value) {
  absl::StatusOr<bridge::AvroValue> result = bridge::AvroValue::CreateString(value);
  EXPECT_TRUE(result.ok()) << result.status();
  return *std::move(result);
}

// D1 decodes an invalid-UTF-8 `string` to Value::Bytes, so operator== would
// report a correct decode as a divergence. ValuesEqual normalizes that
// difference and only that: the raw bytes must still match.
TEST(ValuesEqualTest, StringAndBytesWithSameContentAreEqual) {
  bridge::AvroValue str = MustCreateString("a");
  bridge::AvroValue bytes = bridge::AvroValue::CreateBytes("a");

  // The gap being closed.
  EXPECT_FALSE(str == bytes);

  EXPECT_TRUE(ValuesEqual(str, bytes));
}

TEST(ValuesEqualTest, DifferingPayloadsAreUnequal) {
  bridge::AvroValue str = MustCreateString("a");
  bridge::AvroValue bytes = bridge::AvroValue::CreateBytes("b");

  EXPECT_FALSE(ValuesEqual(str, bytes));
}

// A non-UTF-8 field turns up nested in real data, not as a bare datum.
TEST(ValuesEqualTest, NormalizesNestedInRecordField) {
  bridge::AvroValue with_string = bridge::AvroValue::CreateRecord();
  ASSERT_TRUE(with_string.RecordPut("payload", MustCreateString("legacy")).ok());
  bridge::AvroValue with_bytes = bridge::AvroValue::CreateRecord();
  ASSERT_TRUE(
      with_bytes.RecordPut("payload", bridge::AvroValue::CreateBytes("legacy"))
          .ok());

  EXPECT_TRUE(ValuesEqual(with_string, with_bytes));
}

TEST(ValuesEqualTest, TypeMismatchNotNormalizedAway) {
  bridge::AvroValue str = MustCreateString("1");
  bridge::AvroValue number = bridge::AvroValue::CreateInt(1);

  EXPECT_FALSE(ValuesEqual(str, number));
}

// A value from the real D1 read path rather than one hand-tagged as Bytes.
// Zigzag length 2, then two bytes that are not UTF-8.
constexpr char kInvalidUtf8PayloadDatum[] = "\x04\xff\xfe";
constexpr absl::string_view kInvalidUtf8PayloadWire(kInvalidUtf8PayloadDatum, 3);

TEST(ValuesEqualTest, RealDecodedNonUtf8FieldComparesByRawBytes) {
  constexpr char kSchemaJson[] = R"({
    "type": "record",
    "name": "Legacy",
    "fields": [{"name": "payload", "type": "string"}]
  })";
  absl::StatusOr<bridge::AvroSchema> schema = bridge::AvroSchema::Parse(kSchemaJson);
  ASSERT_TRUE(schema.ok());
  absl::StatusOr<bridge::AvroProjection> projection =
      bridge::AvroProjection::Compile(*schema, *schema);
  ASSERT_TRUE(projection.ok()) << projection.status();

  absl::StatusOr<bridge::AvroValue> decoded =
      projection->DecodeDatum(kInvalidUtf8PayloadWire);
  ASSERT_TRUE(decoded.ok()) << decoded.status();

  bridge::AvroValue expected = bridge::AvroValue::CreateRecord();
  ASSERT_TRUE(expected
                  .RecordPut("payload", bridge::AvroValue::CreateBytes(
                                            std::string("\xff\xfe", 2)))
                  .ok());

  EXPECT_TRUE(ValuesEqual(*decoded, expected));
}

}  // namespace
}  // namespace security::avro_compare
