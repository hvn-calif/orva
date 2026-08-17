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

// The bridge refuses an allocation past its ceiling, by policy and
// deterministically. avro-cpp has no ceiling: it refuses the same input only
// when the allocation actually fails or the data runs out, which depends on
// how much memory the machine has. So a verdict comparison on such an input is
// not meaningful in either direction, which is what D9 records.
bool LooksLikeAllocationCeiling(const absl::Status& status) {
  return std::string(status.message()).find("Unable to allocate") !=
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

// avro-cpp stops at the end of the first datum and ignores whatever follows;
// the bridge requires the buffer to be exactly one datum. Re-encoding what
// avro-cpp decoded gives the length it consumed, so a shorter re-encoding means
// the input carried trailing bytes.
//
// A non-canonical varint in the input would also re-encode shorter, for an
// unrelated reason. This only chooses which divergence ID to report, never
// whether to report, so a misclassification costs a wrong label on a finding
// that is real either way.
bool TrailingBytesExplainIt(const ::avro::ValidSchema& schema,
                            const ::avro::GenericDatum& datum,
                            const std::string& bytes) {
  CppEncoded reencoded = EncodeWithAvrocpp(schema, datum);
  return reencoded.outcome.ok() && reencoded.bytes.size() < bytes.size();
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
// differential available on schema *rendering*: avro-cpp 1.11.4 has no
// canonical-form or fingerprint API, so the bridge's `CanonicalForm()` and
// `FingerprintRabin()` have nothing to be compared against and are out of
// scope here. See doc/CanonicalFormBug.md for a conformance bug on that
// surface, found while this harness was being written.
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

  ASSERT_TRUE(log.empty()) << log.Render() << "schema: " << schema_json;
}
FUZZ_TEST(Differential, SchemasCrossParse).WithDomains(AnyTree());

// ---------------------------------------------------------------------------
// Byte-oriented properties.
//
// The three above generate a tree and lower it, so they only ever exercise
// schemas the generator can build and values that match them. That is what
// makes them productive without Rust-side coverage instrumentation, but it
// also bounds them: Normalize legalises the tree, so `[]` and other malformed
// shapes are unreachable however long they run, and a lowered value never
// exercises a decoder's error paths.
//
// These two take the input as *bytes* instead, covering the complement.
// ---------------------------------------------------------------------------

// Schema text straight to both parsers. No tree, no lowering.
void SchemaTextVerdictsAgree(const std::string& text) {
  const bool bridge_ok = AvroSchema::Parse(text).ok();
  CppOutcome parsed = CallAvrocpp("compileJsonSchemaFromMemory", [&] {
    ::avro::compileJsonSchemaFromMemory(
        reinterpret_cast<const uint8_t*>(text.data()), text.size());
  });

  FindingLog log(&Suppressions());
  if (bridge_ok != parsed.ok()) {
    log.Report(DivergenceId::kSchemaParseVerdict, "$",
               std::string("the bridge ") + (bridge_ok ? "accepted" : "rejected") +
                   " schema text avro-cpp " +
                   (parsed.ok() ? "accepted" : "rejected") +
                   (parsed.ok() ? "" : " (" + parsed.what + ")"),
               parsed.ok() ? "bridge rejected" : "avrocpp rejected");
  }
  ASSERT_TRUE(log.empty()) << log.Render() << "text: " << text;
}
FUZZ_TEST(Differential, SchemaTextVerdictsAgree).WithDomains(AnySchemaText());

// A schema both engines accept, with arbitrary bytes as the encoded datum.
//
// The schema still comes from the tree generator, since random bytes are
// almost never a legal schema and an input where both parsers refuse teaches
// nothing. The *payload* is unconstrained, which is the point: a lowered value
// always encodes to well-formed bytes, so DatumCircleAgrees never reaches the
// decoders' length-prefix, framing or truncation paths.
void DecodersAgreeOnArbitraryBytes(const Node& raw, const std::string& bytes) {
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

  auto bridge_decoded = security::avro::DecodeDatum(*bridge_schema, bytes);
  ::avro::GenericDatum cpp_datum;
  CppOutcome cpp_decoded = DecodeWithAvrocpp(cpp_schema, bytes, &cpp_datum);

  const bool bridge_panicked =
      !bridge_decoded.ok() && LooksLikeRustPanic(bridge_decoded.status());
  if (bridge_panicked) {
    log.Report(DivergenceId::kRustPanicCaught, "$",
               "decoding panicked: " +
                   std::string(bridge_decoded.status().message()) + " [" +
                   Hex(bytes) + "]",
               "rust panic");
  }

  if (bridge_decoded.ok() && !cpp_decoded.ok()) {
    log.Report(DivergenceId::kDecodeVerdictBridgeLenient, "$",
               "the bridge decoded bytes avro-cpp rejected: " + cpp_decoded.what +
                   " [" + Hex(bytes) + "]",
               "avrocpp rejected");
  } else if (!bridge_decoded.ok() && cpp_decoded.ok() && !bridge_panicked) {
    // Three different things can put us here, and reporting them all as one
    // divergence would mislabel two of them.
    //
    // D9: the bridge's allocation ceiling fired. avro-cpp has no ceiling, so
    // whether it also refuses depends on the machine's memory rather than on
    // the input, and the two are not comparable.
    //
    // TRAILING_BYTES: avro-cpp stops at the end of the first datum and ignores
    // the rest; the bridge requires the buffer to hold exactly one datum.
    //
    // Anything else is a real decode disagreement.
    DivergenceId id = DivergenceId::kDecodeVerdictAvrocppLenient;
    const char* narrow = "bridge rejected";
    if (LooksLikeAllocationCeiling(bridge_decoded.status())) {
      id = DivergenceId::kD9AllocationCeiling;
      narrow = "allocation ceiling";
    } else if (TrailingBytesExplainIt(cpp_schema, cpp_datum, bytes)) {
      id = DivergenceId::kTrailingBytes;
      narrow = "trailing bytes";
    }
    log.Report(id, "$",
               "avro-cpp decoded bytes the bridge rejected: " +
                   std::string(bridge_decoded.status().message()) + " [" +
                   Hex(bytes) + "]",
               narrow);
  } else if (bridge_decoded.ok() && cpp_decoded.ok()) {
    CompareValues(*bridge_decoded, cpp_datum, &log);
  }

  ASSERT_TRUE(log.empty()) << log.Render() << "schema: " << schema_json;
}
FUZZ_TEST(Differential, DecodersAgreeOnArbitraryBytes)
    .WithDomains(AnyTree(), fuzztest::Arbitrary<std::string>().WithMaxSize(64));

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

// NEW FINDING, found by DecodersAgreeOnArbitraryBytes on its first input.
//
// The bridge decodes an *empty* buffer into a fully-formed value, fabricating
// nulls for fields that have no bytes behind them. avro-cpp reports EOF.
//
//   schema: {"type":"record","name":"R","fields":[
//              {"name":"a","type":"boolean"},{"name":"b","type":"boolean"}]}
//   input:  "" (zero bytes)
//   bridge: ok, {"a":null,"b":null}
//   avrocpp: avro::decode: EOF reached
//
// Two things are wrong. Decoding zero bytes should fail, and the value that
// comes back does not inhabit its own schema -- `null` is not a legal value of
// `boolean`. A caller handed a truncated message gets a success status and a
// record of nulls rather than an error, which is silent data fabrication: the
// same class the register reserves for its worst entries, but manufacturing
// data rather than losing it.
//
// The tree-based properties cannot reach this. A lowered value always encodes
// to well-formed bytes, so no generated input is ever truncated.
TEST(Differential, EmptyInputDecodesToFabricatedNulls) {
  const std::string schema_text =
      R"({"type":"record","name":"R","fields":[)"
      R"({"name":"a","type":"boolean"},{"name":"b","type":"boolean"}]})";

  auto bridge_schema = AvroSchema::Parse(schema_text);
  ASSERT_TRUE(bridge_schema.ok()) << bridge_schema.status();
  ::avro::ValidSchema cpp_schema;
  CppOutcome parsed = CallAvrocpp("compileJsonSchemaFromMemory", [&] {
    cpp_schema = ::avro::compileJsonSchemaFromMemory(
        reinterpret_cast<const uint8_t*>(schema_text.data()),
        schema_text.size());
  });
  ASSERT_TRUE(parsed.ok()) << parsed.what;

  ::avro::GenericDatum datum;
  CppOutcome cpp = DecodeWithAvrocpp(cpp_schema, std::string(), &datum);
  EXPECT_FALSE(cpp.ok()) << "avro-cpp is expected to report EOF on empty input";

  auto decoded = security::avro::DecodeDatum(*bridge_schema, std::string());
  ASSERT_TRUE(decoded.ok())
      << "the bridge is expected to accept empty input at this commit; if it "
         "now fails, this finding is fixed and the expectations below need "
         "flipping";
  auto json = decoded->ToJsonString();
  ASSERT_TRUE(json.ok()) << json.status();
  EXPECT_EQ(*json, R"({"a":null,"b":null})")
      << "both boolean fields were fabricated from no input at all";
}

// Same finding, minimal shapes. A bare `boolean` decodes to Null, and a union
// decodes to a union whose branch is Null -- neither inhabits its schema.
TEST(Differential, EmptyInputFabricatesNullForBooleanAndUnion) {
  auto boolean_schema = AvroSchema::Parse(R"("boolean")");
  ASSERT_TRUE(boolean_schema.ok());
  auto as_boolean = security::avro::DecodeDatum(*boolean_schema, std::string());
  ASSERT_TRUE(as_boolean.ok());
  EXPECT_TRUE(as_boolean->IsNull()) << "decoded Null under a boolean schema";
  EXPECT_FALSE(as_boolean->GetBoolean().ok());

  auto union_schema = AvroSchema::Parse(R"(["int"])");
  ASSERT_TRUE(union_schema.ok());
  auto as_union = security::avro::DecodeDatum(*union_schema, std::string());
  ASSERT_TRUE(as_union.ok());
  ASSERT_TRUE(as_union->IsUnion());
  auto branch = as_union->GetUnionValue();
  ASSERT_TRUE(branch.ok());
  EXPECT_TRUE(branch->IsNull())
      << "a union of int alone decoded to a null branch from no input";

  // `null` really does encode as zero bytes, so this one is correct and is
  // here to show the finding is not just "empty input always succeeds".
  auto null_schema = AvroSchema::Parse(R"("null")");
  ASSERT_TRUE(null_schema.ok());
  EXPECT_TRUE(security::avro::DecodeDatum(*null_schema, std::string()).ok());

  // And an int does fail, so the fabrication is type-dependent rather than
  // uniform -- which is what makes it easy to miss.
  auto int_schema = AvroSchema::Parse(R"("int")");
  ASSERT_TRUE(int_schema.ok());
  EXPECT_FALSE(security::avro::DecodeDatum(*int_schema, std::string()).ok());
}

// NEW FINDING, found by SchemaTextVerdictsAgree on its first input.
//
// An empty union `[]` and an empty enum are accepted by the bridge and
// rejected by avro-cpp ("bad node of type union"/"of type enum"). The bridge
// also re-renders `[]` as `[]`, so it round-trips a schema avro-cpp cannot
// read.
//
// This is the case the tree-based generator provably cannot produce:
// NormalizeChildren tops an empty union up to one branch, so no amount of
// running SchemaVerdictsAgree would have found it. Two bytes of schema text
// found it on the first input.
TEST(Differential, EmptyUnionAndEnumAcceptedOnlyByTheBridge) {
  for (const char* text : {R"([])",
                           R"({"type":"enum","name":"E","symbols":[]})",
                           R"({"type":"record","name":"R","fields":[)"
                           R"({"name":"a","type":[]}]})"}) {
    const std::string schema(text);
    EXPECT_TRUE(AvroSchema::Parse(schema).ok())
        << "the bridge is expected to accept " << schema;
    CppOutcome parsed = CallAvrocpp("compileJsonSchemaFromMemory", [&] {
      ::avro::compileJsonSchemaFromMemory(
          reinterpret_cast<const uint8_t*>(schema.data()), schema.size());
    });
    EXPECT_FALSE(parsed.ok()) << "avro-cpp is expected to reject " << schema;
  }
}

// NEW FINDING, found by SchemasCrossParse.
//
// The bridge re-renders a `duration`-annotated fixed in a shape avro-cpp
// cannot parse, and loses the schema's identity doing it:
//
//   in:  {"type":"fixed","name":"B","namespace":"ns","size":12,
//         "logicalType":"duration"}
//   out: {"type":{"type":"fixed","name":"duration","size":12},
//         "logicalType":"duration"}
//
// Both engines parse the input. The rendering is where they part company, in
// two distinct ways:
//
//   1. The fixed is nested inside "type" as an object. avro-cpp rejects that
//      with `Json field "type" is not a string`, so a schema that made the
//      round trip through the bridge can no longer be read by avro-cpp -- the
//      exact direction that breaks a partly-migrated deployment.
//   2. Name and namespace are dropped. `ns.B` comes back as `duration`, so
//      schema identity does not survive the round trip even for readers that
//      can parse the result.
TEST(Differential, DurationFixedRendersUnparseableByAvrocpp) {
  const std::string original =
      R"({"type":"fixed","name":"B","namespace":"ns","size":12,)"
      R"("logicalType":"duration"})";

  // Both engines accept the original.
  auto bridge_schema = AvroSchema::Parse(original);
  ASSERT_TRUE(bridge_schema.ok()) << bridge_schema.status();
  CppOutcome parsed = CallAvrocpp("compileJsonSchemaFromMemory", [&] {
    ::avro::compileJsonSchemaFromMemory(
        reinterpret_cast<const uint8_t*>(original.data()), original.size());
  });
  ASSERT_TRUE(parsed.ok()) << parsed.what;

  auto rendered = bridge_schema->ToJsonString();
  ASSERT_TRUE(rendered.ok()) << rendered.status();

  // Pins the buggy rendering as it stands at this commit. Both expectations
  // flip when it is fixed, which is the signal to turn them into the
  // correctness assertions described above.
  EXPECT_EQ(*rendered,
            R"({"type":{"type":"fixed","name":"duration","size":12},)"
            R"("logicalType":"duration"})")
      << "the name ns.B should have survived the round trip";

  CppOutcome reparsed = CallAvrocpp("compileJsonSchemaFromMemory", [&] {
    ::avro::compileJsonSchemaFromMemory(
        reinterpret_cast<const uint8_t*>(rendered->data()), rendered->size());
  });
  EXPECT_FALSE(reparsed.ok())
      << "avro-cpp now parses the bridge's duration rendering; this finding is "
         "fixed and the harness's expectations need updating";
}

// NEW FINDING, found by DecodersAgreeOnArbitraryBytes.
//
// avro-cpp stops at the end of the first datum and ignores whatever follows;
// the bridge requires the buffer to hold exactly one datum and rejects the
// remainder. Direction is bridge-stricter, so nothing is mis-decoded, and the
// bridge's behaviour is the more defensible of the two -- trailing bytes after
// a single datum usually mean framing has gone wrong. Recorded because it is a
// difference callers will hit when moving code that relied on avro-cpp
// tolerating a padded buffer.
TEST(Differential, TrailingBytesAcceptedOnlyByAvrocpp) {
  const std::string schema_text = R"("int")";
  const std::string bytes("\x02\xff", 2);  // one int, then a stray byte

  auto bridge_schema = AvroSchema::Parse(schema_text);
  ASSERT_TRUE(bridge_schema.ok()) << bridge_schema.status();
  ::avro::ValidSchema cpp_schema;
  CppOutcome parsed = CallAvrocpp("compileJsonSchemaFromMemory", [&] {
    cpp_schema = ::avro::compileJsonSchemaFromMemory(
        reinterpret_cast<const uint8_t*>(schema_text.data()),
        schema_text.size());
  });
  ASSERT_TRUE(parsed.ok()) << parsed.what;

  ::avro::GenericDatum datum;
  CppOutcome cpp = DecodeWithAvrocpp(cpp_schema, bytes, &datum);
  EXPECT_TRUE(cpp.ok()) << "avro-cpp is expected to ignore the trailing byte";

  auto decoded = security::avro::DecodeDatum(*bridge_schema, bytes);
  EXPECT_FALSE(decoded.ok()) << "the bridge is expected to reject trailing bytes";
}

// NEW FINDING, found by DatumCircleAgrees under coverage-guided fuzzing.
//
// A schema that defines the same full name twice is illegal Avro -- a name may
// be defined once. apache-avro 0.21 nevertheless *parses* it, and only blows up
// later, at encode time, with a panic rather than an error:
//
//   apache-avro-0.21.0/src/types.rs:369
//   Schemata didn't successfully resolve: Two named schema defined for same
//   fullname: ns.foo
//
// Both engines accept the illegal schema -- avro-cpp too, which was worth
// checking rather than assuming. The divergence is in what happens next:
// avro-cpp carries on, apache-avro panics.
//
// Two problems on the bridge side. The schema should have been rejected at
// parse time, and a malformed one should yield an error rather than a panic.
// catch_panic contains it, so the process survives and the caller sees an
// absl::Status -- but that guard is the only thing between an untrusted schema
// and an abort, and any entry point missing it is a denial of service.
TEST(Differential, DuplicateFullNameParsesThenPanicsOnEncode) {
  const std::string schema_json =
      R"({"type":"record","name":"foo","namespace":"ns","fields":[)"
      R"({"name":"a","type":{"type":"record","name":"foo","namespace":"ns",)"
      R"("fields":[]}}]})";

  auto bridge_schema = AvroSchema::Parse(schema_json);
  // Parsing succeeds even though the schema is illegal.
  ASSERT_TRUE(bridge_schema.ok())
      << "if this now fails, apache-avro rejects duplicate full names at parse "
         "time and the finding is fixed";

  AvroValue outer = AvroValue::CreateRecord();
  AvroValue inner = AvroValue::CreateRecord();
  ASSERT_TRUE(outer.RecordPut("a", inner).ok());

  auto encoded = security::avro::EncodeDatum(*bridge_schema, outer);
  ASSERT_FALSE(encoded.ok()) << "encoding an illegal schema should not succeed";
  EXPECT_NE(std::string(encoded.status().message())
                .find("Rust panic caught while processing Avro input"),
            std::string::npos)
      << "expected a contained panic, got: " << encoded.status().message();

  // avro-cpp accepts it too, so neither engine enforces the uniqueness rule at
  // parse time. Only apache-avro turns that into a panic later.
  CppOutcome parsed = CallAvrocpp("compileJsonSchemaFromMemory", [&] {
    ::avro::compileJsonSchemaFromMemory(
        reinterpret_cast<const uint8_t*>(schema_json.data()),
        schema_json.size());
  });
  EXPECT_TRUE(parsed.ok())
      << "if avro-cpp now rejects duplicate full names, the two engines have "
         "stopped agreeing at parse time and this finding needs revisiting: "
      << parsed.what;
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
