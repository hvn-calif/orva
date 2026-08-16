// The differential properties: this binding against Apache avro-cpp.
//
// Every property here compares *values*, never encoded bytes. Two sources of
// run-to-run variation live inside the libraries -- random container sync
// markers, and Rust HashMap iteration order making encoded map bytes differ
// (D3) -- so a byte-level assertion would be flaky rather than informative.

#include <memory>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "avro/Compiler.hh"
#include "avro/Decoder.hh"
#include "avro/Encoder.hh"
#include "avro/Generic.hh"
#include "avro/GenericDatum.hh"
// The free avro::encode/avro::decode templates live here; Generic.hh only
// supplies the codec_traits<GenericDatum> specialisation they dispatch to.
#include "avro/Specific.hh"
#include "avro/Stream.hh"
#include "avro/ValidSchema.hh"
#include "avro_bridge.h"
#include "fuzz/compare.h"
#include "fuzz/domains.h"
#include "fuzz/ir.h"
#include "fuzz/lower_avrocpp.h"
#include "fuzz/lower_bridge.h"
#include "fuzz/lower_schema.h"
#include "fuzz/suppress.h"
#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

namespace security::avro_fuzz {
namespace {

using ::security::avro::AvroSchema;
using ::security::avro::AvroValue;

// The bridge's allocation ceiling is a process-global that takes effect on the
// first call and ignores every later one. Setting it from a static initialiser
// means it is in force before any property runs, so the first input cannot
// behave differently from the rest -- which would make a corpus replay depend
// on ordering. The 512 MiB default does not survive ASan's shadow overhead on
// this machine.
const bool kAllocationCeilingSet = [] {
  security::avro::SetMaxAllocationBytes(64u << 20);
  return true;
}();

std::string Hex(const std::string& raw) {
  static const char* kDigits = "0123456789abcdef";
  std::string out;
  for (unsigned char c : raw) {
    out += kDigits[c >> 4];
    out += kDigits[c & 0xF];
  }
  return out;
}

// A clean absl::Status whose text says apache-avro panicked is still a
// finding: the panic was contained at the FFI boundary, but it happened.
bool LooksLikeRustPanic(const absl::Status& status) {
  return std::string(status.message())
             .find("Rust panic caught while processing Avro input") !=
         std::string::npos;
}

struct CppEncoded {
  CppOutcome outcome;
  std::string bytes;
};

CppEncoded EncodeWithAvrocpp(const ::avro::ValidSchema& schema,
                             const ::avro::GenericDatum& datum) {
  CppEncoded result;
  result.outcome = CallAvrocpp("avro::encode", [&] {
    std::unique_ptr<::avro::OutputStream> out = ::avro::memoryOutputStream();
    ::avro::EncoderPtr encoder = ::avro::binaryEncoder();
    encoder->init(*out);
    ::avro::encode(*encoder, datum);
    encoder->flush();
    std::unique_ptr<::avro::InputStream> in = ::avro::memoryInputStream(*out);
    const uint8_t* data = nullptr;
    size_t length = 0;
    while (in->next(&data, &length)) {
      result.bytes.append(reinterpret_cast<const char*>(data), length);
    }
  });
  return result;
}

CppOutcome DecodeWithAvrocpp(const ::avro::ValidSchema& schema,
                             const std::string& bytes,
                             ::avro::GenericDatum* out) {
  return CallAvrocpp("avro::decode", [&] {
    std::unique_ptr<::avro::InputStream> in = ::avro::memoryInputStream(
        reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    ::avro::DecoderPtr decoder = ::avro::binaryDecoder();
    decoder->init(*in);
    *out = ::avro::GenericDatum(schema);
    ::avro::decode(*decoder, *out);
  });
}

// ---------------------------------------------------------------------------

// The central property. One tree becomes one schema and one value; both engines
// encode it and both decode the other's bytes, and every result is compared
// back against the original. Covers three of the four directions at once, with
// the failing leg named in the message.
void DatumCircleAgrees(const Node& raw) {
  const Node tree = Normalize(raw, NormalizeOptions{});
  const std::string schema_json = ToSchemaJson(tree);

  FindingLog log(&Suppressions());

  // Both parsers must agree the schema is legal before anything else means
  // something. A split verdict is itself a finding, reported by SchemaAgrees;
  // here it just ends the input.
  auto bridge_schema = AvroSchema::Parse(schema_json);
  ::avro::ValidSchema cpp_schema;
  CppOutcome parsed = CallAvrocpp("compileJsonSchemaFromMemory", [&] {
    cpp_schema = ::avro::compileJsonSchemaFromMemory(
        reinterpret_cast<const uint8_t*>(schema_json.data()),
        schema_json.size());
  });
  if (!bridge_schema.ok() || !parsed.ok()) return;

  auto bridge_value = ToBridgeValue(tree, &log);
  ::avro::GenericDatum cpp_datum;
  CppOutcome built = ToAvrocppDatum(tree, cpp_schema, &cpp_datum);

  // ToBridgeValue reports D1 and D2 into the log and then fails. That failure
  // is the finding, so assert on the log rather than returning quietly.
  ASSERT_TRUE(log.empty()) << log.Render() << "schema: " << schema_json << "\n"
                           << "tree:   " << ToDebugString(tree);
  if (!bridge_value.ok() || !built.ok()) return;

  // Leg 1: the bridge writes, avro-cpp reads.
  auto bridge_bytes = security::avro::EncodeDatum(*bridge_schema, *bridge_value);
  if (bridge_bytes.ok()) {
    ::avro::GenericDatum round_tripped;
    CppOutcome read = DecodeWithAvrocpp(cpp_schema, *bridge_bytes, &round_tripped);
    if (!read.ok()) {
      log.Report(DivergenceId::kBridgeCannotRepresent, "$",
                 "avro-cpp could not decode bytes the bridge encoded: " +
                     read.what + " [" + Hex(*bridge_bytes) + "]",
                 "avrocpp rejected bridge output");
    } else {
      CompareValues(*bridge_value, round_tripped, &log);
    }
  } else if (LooksLikeRustPanic(bridge_bytes.status())) {
    log.Report(DivergenceId::kRustPanicCaught, "$",
               "encoding panicked: " + std::string(bridge_bytes.status().message()),
               "rust panic");
  }

  // Leg 2: avro-cpp writes, the bridge reads. This is the direction that
  // surfaces the read side of D1, because avro-cpp will happily encode a
  // string holding bytes the bridge refuses to decode.
  CppEncoded cpp_bytes = EncodeWithAvrocpp(cpp_schema, cpp_datum);
  if (cpp_bytes.outcome.ok()) {
    auto decoded = security::avro::DecodeDatum(*bridge_schema, cpp_bytes.bytes);
    if (!decoded.ok()) {
      log.Report(DivergenceId::kDecodeVerdictAvrocppLenient, "$",
                 "the bridge rejected bytes avro-cpp encoded: " +
                     std::string(decoded.status().message()) + " [" +
                     Hex(cpp_bytes.bytes) + "]",
                 LooksLikeRustPanic(decoded.status()) ? "rust panic"
                                                      : "bridge rejected");
    } else {
      CompareValues(*decoded, cpp_datum, &log);

      // Leg 3: the same bytes through both readers must agree with each other.
      ::avro::GenericDatum cpp_reread;
      CppOutcome reread = DecodeWithAvrocpp(cpp_schema, cpp_bytes.bytes, &cpp_reread);
      if (reread.ok()) CompareValues(*decoded, cpp_reread, &log);
    }
  }

  ASSERT_TRUE(log.empty()) << log.Render() << "schema: " << schema_json << "\n"
                           << "tree:   " << ToDebugString(tree);
}
FUZZ_TEST(Differential, DatumCircleAgrees).WithDomains(AnyTree());

// Do the two parsers agree on which schemas are legal? This is the surface the
// divergence register lists as entirely uninvestigated, so it is the most
// likely source of new findings. Runs in kSchemaOnly mode, which leaves the
// illegality in: reserved words as names, redefinitions, scale beyond
// precision, recursive references.
void SchemaVerdictsAgree(const Node& raw) {
  NormalizeOptions options;
  options.mode = NormalizeMode::kSchemaOnly;
  const std::string schema_json = ToSchemaJson(Normalize(raw, options));

  const bool bridge_ok = AvroSchema::Parse(schema_json).ok();
  ::avro::ValidSchema cpp_schema;
  CppOutcome parsed = CallAvrocpp("compileJsonSchemaFromMemory", [&] {
    cpp_schema = ::avro::compileJsonSchemaFromMemory(
        reinterpret_cast<const uint8_t*>(schema_json.data()),
        schema_json.size());
  });

  FindingLog log(&Suppressions());
  if (bridge_ok != parsed.ok()) {
    log.Report(DivergenceId::kSchemaParseVerdict, "$",
               std::string("the bridge ") + (bridge_ok ? "accepted" : "rejected") +
                   " a schema avro-cpp " + (parsed.ok() ? "accepted" : "rejected") +
                   (parsed.ok() ? "" : " (" + parsed.what + ")"),
               parsed.ok() ? "bridge rejected" : "avrocpp rejected");
  }
  ASSERT_TRUE(log.empty()) << log.Render() << "schema: " << schema_json;
}
FUZZ_TEST(Differential, SchemaVerdictsAgree).WithDomains(AnyTree());

// A schema rendered by one engine must parse in the other. This is the only
// differential available on schema *rendering*, since avro-cpp 1.11.4 has no
// canonical-form or fingerprint API to compare against.
void SchemasCrossParse(const Node& raw) {
  const Node tree = Normalize(raw, NormalizeOptions{});
  const std::string schema_json = ToSchemaJson(tree);

  auto bridge_schema = AvroSchema::Parse(schema_json);
  ::avro::ValidSchema cpp_schema;
  CppOutcome parsed = CallAvrocpp("compileJsonSchemaFromMemory", [&] {
    cpp_schema = ::avro::compileJsonSchemaFromMemory(
        reinterpret_cast<const uint8_t*>(schema_json.data()),
        schema_json.size());
  });
  if (!bridge_schema.ok() || !parsed.ok()) return;

  FindingLog log(&Suppressions());

  // avro-cpp's rendering must parse in the bridge.
  std::string cpp_rendered;
  CppOutcome rendered = CallAvrocpp("ValidSchema::toJson", [&] {
    cpp_rendered = cpp_schema.toJson(false);
  });
  if (rendered.ok() && !AvroSchema::Parse(cpp_rendered).ok()) {
    log.Report(DivergenceId::kCrossParseRoundTrip, "$",
               "the bridge cannot parse the schema avro-cpp rendered: " +
                   cpp_rendered,
               "bridge cannot parse avrocpp rendering");
  }

  // And the bridge's rendering must parse in avro-cpp.
  auto bridge_rendered = bridge_schema->ToJsonString();
  if (bridge_rendered.ok()) {
    CppOutcome reparsed = CallAvrocpp("compileJsonSchemaFromMemory", [&] {
      ::avro::compileJsonSchemaFromMemory(
          reinterpret_cast<const uint8_t*>(bridge_rendered->data()),
          bridge_rendered->size());
    });
    if (!reparsed.ok()) {
      log.Report(DivergenceId::kCrossParseRoundTrip, "$",
                 "avro-cpp cannot parse the schema the bridge rendered: " +
                     *bridge_rendered + " (" + reparsed.what + ")",
                 "avrocpp cannot parse bridge rendering");
    }
  }

  // Intra-bridge, since avro-cpp has no counterpart: the canonical form must
  // reparse to a schema with the same fingerprint.
  auto canonical = AvroSchema::Parse(bridge_schema->CanonicalForm());
  if (canonical.ok() &&
      canonical->FingerprintRabin() != bridge_schema->FingerprintRabin()) {
    log.Report(DivergenceId::kCrossParseRoundTrip, "$",
               "the canonical form reparses to a different fingerprint",
               "canonical fingerprint");
  }

  ASSERT_TRUE(log.empty()) << log.Render() << "schema: " << schema_json;
}
FUZZ_TEST(Differential, SchemasCrossParse).WithDomains(AnyTree());

// ---------------------------------------------------------------------------
// Regression seeds. Triaged findings are pinned here so they stay fixed.
// ---------------------------------------------------------------------------

// D1, write side: avro-cpp stores arbitrary bytes in a string; the bridge
// validates UTF-8 and refuses. Open at this commit.
TEST(Differential, D1NonUtf8StringIsRejectedByTheBridge) {
  auto value = AvroValue::CreateString("\xff\xfe");
  EXPECT_FALSE(value.ok())
      << "if this now succeeds, D1's write side has been fixed and the "
         "harness's acceptance test needs updating";
}

// D2: avro-cpp's GenericMap is a vector of pairs and keeps both entries; the
// bridge's HashMap collapses them. Open at this commit.
TEST(Differential, D2DuplicateMapKeyCollapsesInTheBridge) {
  AvroValue map = AvroValue::CreateMap();
  ASSERT_TRUE(map.MapPut("k", AvroValue::CreateInt(1)).ok());
  ASSERT_TRUE(map.MapPut("k", AvroValue::CreateInt(2)).ok());
  auto length = map.GetMapLen();
  ASSERT_TRUE(length.ok());
  EXPECT_EQ(*length, 1u) << "the bridge is expected to collapse duplicate keys "
                            "at this commit; avro-cpp keeps both";
}

// NEW FINDING, found cold by SchemasCrossParse.
//
// A logical type layered on a primitive produces a canonical form that is not
// canonical. Avro's Parsing Canonical Form requires primitives in their simple
// form -- `"int"`, not `{"type":"int"}` -- so the value below violates the
// spec, is not idempotent under reparsing, and yields a Rabin fingerprint that
// disagrees with the spec's.
//
// 8247732601305521295 is the fingerprint the Avro spec's own test data gives
// for `"int"`, and it is what rust/schema.rs already asserts. So the annotated
// schema's 8145260995063234477 is simply wrong: two schemas whose canonical
// forms must be identical fingerprint differently.
//
// This matters beyond tidiness -- fingerprints are how schema registries
// establish schema identity, so a wrong one means a missed cache hit or a
// failed lookup.
//
// The assertions below pin the buggy behaviour as it stands at this commit.
// When it is fixed they will fail, which is the signal to promote them back to
// the correctness assertions in the comment above.
TEST(Differential, CanonicalFormIsNotCanonicalForLogicalTypes) {
  auto annotated =
      AvroSchema::Parse(R"({"type":"int","logicalType":"time-millis"})");
  ASSERT_TRUE(annotated.ok()) << annotated.status();
  auto plain = AvroSchema::Parse(R"("int")");
  ASSERT_TRUE(plain.ok()) << plain.status();

  // Should be `"int"` per the spec's PRIMITIVES rule.
  EXPECT_EQ(annotated->CanonicalForm(), R"({"type":"int"})");
  EXPECT_EQ(plain->CanonicalForm(), R"("int")");

  // So the canonical form is not idempotent.
  auto reparsed = AvroSchema::Parse(annotated->CanonicalForm());
  ASSERT_TRUE(reparsed.ok()) << reparsed.status();
  EXPECT_NE(annotated->CanonicalForm(), reparsed->CanonicalForm());

  // And the fingerprint disagrees with the spec value for an int schema.
  EXPECT_EQ(plain->FingerprintRabin(), 8247732601305521295LL);
  EXPECT_NE(annotated->FingerprintRabin(), plain->FingerprintRabin());
}

// The harness's own sanity check: a plain record must survive the full circle.
// If this fails, a lowering is wrong and every other result is suspect.
TEST(Differential, SimpleRecordSurvivesTheCircle) {
  const std::string schema_json =
      R"({"type":"record","name":"R","fields":[)"
      R"({"name":"i","type":"int"},{"name":"s","type":"string"}]})";

  auto bridge_schema = AvroSchema::Parse(schema_json);
  ASSERT_TRUE(bridge_schema.ok()) << bridge_schema.status();

  ::avro::ValidSchema cpp_schema;
  CppOutcome parsed = CallAvrocpp("compile", [&] {
    cpp_schema = ::avro::compileJsonSchemaFromMemory(
        reinterpret_cast<const uint8_t*>(schema_json.data()),
        schema_json.size());
  });
  ASSERT_TRUE(parsed.ok()) << parsed.what;

  AvroValue record = AvroValue::CreateRecord();
  ASSERT_TRUE(record.RecordPut("i", AvroValue::CreateInt(7)).ok());
  auto text = AvroValue::CreateString("hello");
  ASSERT_TRUE(text.ok());
  ASSERT_TRUE(record.RecordPut("s", *text).ok());

  auto bytes = security::avro::EncodeDatum(*bridge_schema, record);
  ASSERT_TRUE(bytes.ok()) << bytes.status();

  ::avro::GenericDatum datum;
  CppOutcome read = DecodeWithAvrocpp(cpp_schema, *bytes, &datum);
  ASSERT_TRUE(read.ok()) << read.what;

  FindingLog log(&Suppressions());
  EXPECT_TRUE(CompareValues(record, datum, &log)) << log.Render();
}

}  // namespace
}  // namespace security::avro_fuzz
