// The differential properties: this binding against Apache avro-cpp.
//
// Every property here compares *values*, never encoded bytes. Two sources of
// run-to-run variation live inside the libraries -- random container sync
// markers, and Rust HashMap iteration order making encoded map bytes differ
// (D3) -- so a byte-level assertion would be flaky rather than informative.

#include <csignal>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/escaping.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
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

// A clean absl::Status whose text says apache-avro panicked is still a
// finding: the panic was contained at the FFI boundary, but it happened.
bool LooksLikeRustPanic(const absl::Status& status) {
  return absl::StrContains(status.message(),
                           "Rust panic caught while processing Avro input");
}

// The bridge refuses an allocation past its ceiling, by policy and
// deterministically. avro-cpp has no ceiling: it refuses the same input only
// when the allocation actually fails or the data runs out, which depends on
// how much memory the machine has. So a verdict comparison on such an input is
// not meaningful in either direction, which is what D9 records.
bool LooksLikeAllocationCeiling(const absl::Status& status) {
  return absl::StrContains(status.message(), "Unable to allocate");
}

// Avro encodes lengths, block counts and union branches as zigzag varints, so a
// test can spell out what its bytes mean instead of embedding a blob. The same
// helper exists in avro_bytes_fuzz_test.cc, which shares no code with this file
// on purpose.
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
                             absl::string_view bytes,
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

// avro-cpp resizes an array or map to its declared block count before reading a
// single item (Generic.cc:112), with no check against how much input is left and
// no allocation ceiling of its own. Two measured cases: 1.6 GB reserved from
// five bytes, and a 837 GB request from 27 bytes that ASan turned into an abort.
// Either ends a fuzzing run without teaching anything the pinned tests do not
// already record -- avro_bytes_fuzz_test.cc guards the same class with
// kMaxDeclaredLengthBytes, and this is the untriaged vector::resize finding in
// AGENTS.md, now also confirmed for maps.
//
// The guard is deliberately blunt: a varint anywhere in the buffer, at any
// offset, decoding beyond the cap drops the input. It does not try to work out
// which varints the decoder will actually read as counts, because that means
// reimplementing both decoders. The cost is that a large scalar long is also
// dropped here; DatumCircleAgrees carries the int64 boundary values, so that
// coverage is not lost, only moved.
//
// The cap is 16k rather than a million because avro-cpp allocates a GenericDatum
// per declared element up front: a million of them is over 100 MB of transient
// allocation per input, and at that cap this property climbed from 112 MB to
// 847 MB of resident memory and died on its RSS limit eighteen minutes into an
// hour. Framing and truncation are covered just as well by a count in the
// thousands.
constexpr int64_t kMaxDeclaredCount = 1 << 14;

bool HoldsAnOversizedVarint(absl::string_view bytes) {
  for (size_t start = 0; start < bytes.size(); ++start) {
    uint64_t raw = 0;
    int shift = 0;
    for (size_t i = start; i < bytes.size(); ++i) {
      raw |= static_cast<uint64_t>(bytes[i] & 0x7f) << shift;
      if ((bytes[i] & 0x80) == 0) break;
      shift += 7;
      if (shift > 63) return true;
    }
    // Zigzag. A negative count is avro-cpp's block-with-byte-size form, whose
    // magnitude is what gets reserved either way, so compare on magnitude.
    const int64_t value = static_cast<int64_t>(raw >> 1) ^ -(raw & 1);
    if (value > kMaxDeclaredCount || value < -kMaxDeclaredCount) return true;
  }
  return false;
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
                 absl::StrCat("avro-cpp could not decode bytes the bridge "
                              "encoded: ",
                              read.what, " [",
                              absl::BytesToHexString(*bridge_bytes), "]"),
                 "avrocpp rejected bridge output");
    } else {
      CompareValues(*bridge_value, round_tripped, &log);
    }
  } else if (LooksLikeRustPanic(bridge_bytes.status())) {
    log.Report(DivergenceId::kRustPanicCaught, "$",
               absl::StrCat("encoding panicked: ",
                            bridge_bytes.status().message()),
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
                 absl::StrCat("the bridge rejected bytes avro-cpp encoded: ",
                              decoded.status().message(), " [",
                              absl::BytesToHexString(cpp_bytes.bytes), "]"),
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
               absl::StrCat("the bridge ",
                            bridge_ok ? "accepted" : "rejected",
                            " a schema avro-cpp ",
                            parsed.ok() ? "accepted" : "rejected",
                            parsed.ok() ? "" : absl::StrCat(" (", parsed.what,
                                                            ")")),
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
               absl::StrCat("the bridge cannot parse the schema avro-cpp "
                            "rendered: ",
                            cpp_rendered),
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
                 absl::StrCat("avro-cpp cannot parse the schema the bridge "
                              "rendered: ",
                              *bridge_rendered, " (", reparsed.what, ")"),
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
               absl::StrCat("the bridge ",
                            bridge_ok ? "accepted" : "rejected",
                            " schema text avro-cpp ",
                            parsed.ok() ? "accepted" : "rejected",
                            parsed.ok() ? "" : absl::StrCat(" (", parsed.what,
                                                            ")")),
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
  if (HoldsAnOversizedVarint(bytes)) return;

  FindingLog log(&Suppressions());

  auto bridge_decoded = security::avro::DecodeDatum(*bridge_schema, bytes);
  ::avro::GenericDatum cpp_datum;
  CppOutcome cpp_decoded = DecodeWithAvrocpp(cpp_schema, bytes, &cpp_datum);

  const bool bridge_panicked =
      !bridge_decoded.ok() && LooksLikeRustPanic(bridge_decoded.status());
  if (bridge_panicked) {
    log.Report(DivergenceId::kRustPanicCaught, "$",
               absl::StrCat("decoding panicked: ",
                            bridge_decoded.status().message(), " [",
                            absl::BytesToHexString(bytes), "]"),
               "rust panic");
  }

  if (bridge_decoded.ok() && !cpp_decoded.ok()) {
    log.Report(DivergenceId::kDecodeVerdictBridgeLenient, "$",
               absl::StrCat("the bridge decoded bytes avro-cpp rejected: ",
                            cpp_decoded.what, " [",
                            absl::BytesToHexString(bytes), "]"),
               "avrocpp rejected");
  } else if (!bridge_decoded.ok() && cpp_decoded.ok() && !bridge_panicked) {
    // Three different things can put us here, and reporting them all as one
    // divergence would mislabel two of them.
    //
    // D9: the bridge's allocation ceiling fired. avro-cpp has no ceiling, so
    // whether it also refuses depends on the machine's memory rather than on
    // the input, and the two are not comparable.
    //
    // Anything else is a real decode disagreement.
    //
    // TRAILING_BYTES used to be classified here, by checking whether avro-cpp
    // re-encoded to fewer bytes than it was given. That test never looked at why
    // the bridge rejected, and the bridge no longer rejects merely because bytes
    // are left over -- SetRejectTrailingBytes is off by default, matching
    // avro-cpp. So avro-cpp under-consuming while the bridge rejects now means
    // the datum itself failed, and calling that "trailing bytes" would name the
    // wrong cause. The classification is gone rather than left to mislabel.
    DivergenceId id = DivergenceId::kDecodeVerdictAvrocppLenient;
    const char* narrow = "bridge rejected";
    if (LooksLikeAllocationCeiling(bridge_decoded.status())) {
      id = DivergenceId::kD9AllocationCeiling;
      narrow = "allocation ceiling";
    }
    log.Report(id, "$",
               absl::StrCat("avro-cpp decoded bytes the bridge rejected: ",
                            bridge_decoded.status().message(), " [",
                            absl::BytesToHexString(bytes), "]"),
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

// CLOSED by the strict-eof patch. Found by DecodersAgreeOnArbitraryBytes on its
// first input, when the bridge decoded an *empty* buffer into a fully-formed
// value, fabricating nulls for fields with no bytes behind them:
//
//   schema:  {"type":"record","name":"R","fields":[
//               {"name":"a","type":"boolean"},{"name":"b","type":"boolean"}]}
//   input:   "" (zero bytes)
//   bridge:  ok, {"a":null,"b":null}
//   avrocpp: avro::decode: EOF reached
//
// Two things were wrong. Decoding zero bytes should fail, and the value that
// came back did not inhabit its own schema -- `null` is not a legal value of
// `boolean`. A caller handed a truncated message got a success status and a
// record of nulls rather than an error.
//
// Both now report EOF. The tree-based properties still cannot reach this: a
// lowered value always encodes to well-formed bytes, so no generated input is
// ever truncated, which is why the byte-oriented properties found it.
TEST(Differential, EmptyInputIsRejectedByBothEngines) {
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
  EXPECT_FALSE(decoded.ok())
      << "the bridge accepted an empty buffer and returned "
      << decoded->ToJsonString().value_or("a value it could not render");
}

// Same finding, minimal shapes. A bare `boolean` used to decode to Null and a
// union to a union whose branch was Null, neither inhabiting its schema.
TEST(Differential, EmptyInputIsRejectedForBooleanAndUnion) {
  for (const char* text : {R"("boolean")", R"(["int"])", R"(["null","int"])",
                           R"("int")"}) {
    auto schema = AvroSchema::Parse(text);
    ASSERT_TRUE(schema.ok()) << text;
    EXPECT_FALSE(security::avro::DecodeDatum(*schema, std::string()).ok())
        << "an empty buffer decoded under " << text;
  }

  // `null` really does encode as zero bytes, so this one must keep succeeding.
  // It is why the fix could not be "reject an empty buffer".
  auto null_schema = AvroSchema::Parse(R"("null")");
  ASSERT_TRUE(null_schema.ok());
  auto as_null = security::avro::DecodeDatum(*null_schema, std::string());
  ASSERT_TRUE(as_null.ok()) << as_null.status();
  EXPECT_TRUE(as_null->IsNull());
}

// Both of these were found by SchemaTextVerdictsAgree on its first input, as one
// finding: the bridge accepted an empty union `[]` and an empty enum where
// avro-cpp rejects both ("bad node of type union" / "of type enum"), and
// re-rendered `[]` as `[]`, so it round-tripped a schema avro-cpp cannot read.
//
// Neither is reachable from the tree-based generator: NormalizeChildren tops an
// empty union up to one branch, so no amount of running SchemaVerdictsAgree
// would have found it. Two bytes of schema text found it on the first input.
//
// They are split because they closed separately, one patch each. Both are now
// closed; the split stays, because each records a distinct construct.
void ExpectParseVerdicts(absl::string_view schema, bool bridge_accepts,
                         bool avrocpp_accepts) {
  const std::string text(schema);
  EXPECT_EQ(AvroSchema::Parse(text).ok(), bridge_accepts)
      << "the bridge on " << text;
  CppOutcome parsed = CallAvrocpp("compileJsonSchemaFromMemory", [&] {
    ::avro::compileJsonSchemaFromMemory(
        reinterpret_cast<const uint8_t*>(text.data()), text.size());
  });
  EXPECT_EQ(parsed.ok(), avrocpp_accepts)
      << "avro-cpp on " << text << ": " << parsed.what;
}

// CLOSED by the empty-union patch. No branch index is in range for an empty
// union, so nothing encodes into one and no bytes decode under one.
TEST(Differential, EmptyUnionIsRejectedByBothEngines) {
  for (const char* text : {R"([])", R"([[]])",
                           R"({"type":"record","name":"R","fields":[)"
                           R"({"name":"a","type":[]}]})"}) {
    ExpectParseVerdicts(text, false, false);
  }
  // A one-branch union stays legal on both sides, which is what keeps the fix
  // from over-reaching.
  ExpectParseVerdicts(R"(["int"])", true, true);
}

// CLOSED by the empty-enum patch, in parse_enum rather than in a constructor:
// EnumSchema has public fields and a builder, so the parse path is what can be
// guarded, and that is the path untrusted input arrives on.
TEST(Differential, EmptyEnumIsRejectedByBothEngines) {
  ExpectParseVerdicts(R"({"type":"enum","name":"E","symbols":[]})", false,
                      false);
  // One symbol is enough: index 0 is in range.
  ExpectParseVerdicts(R"({"type":"enum","name":"E","symbols":["A"]})", true,
                      true);
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

// CLOSED, bridge-side rather than by an apache-avro patch: from_avro_datum
// already stops at the end of the first datum like avro-cpp, and the rejection
// was the bridge's own. It stays available through SetRejectTrailingBytes, off by
// default, since code being migrated may pass a padded or over-allocated buffer.
//
// This is the only patch in the series that removes a check the bridge shipped
// with, so both halves are pinned: here that the two engines now agree and read
// the same value, and in rust/tests/reject_trailing_bytes.rs that the knob still
// rejects. The knob is a set-once process global, so its `true` value cannot be
// exercised from this binary.
TEST(Differential, TrailingBytesAreIgnoredByBothEngines) {
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
  ASSERT_TRUE(cpp.ok()) << "avro-cpp is expected to ignore the trailing byte";
  EXPECT_EQ(datum.value<int32_t>(), 1);

  auto decoded = security::avro::DecodeDatum(*bridge_schema, bytes);
  ASSERT_TRUE(decoded.ok())
      << "the bridge should now ignore the trailing byte: " << decoded.status();
  EXPECT_EQ(decoded->GetInt().value_or(0), 1);

  // Bytes missing from a datum are still an error. Ignoring leftovers and
  // accepting a truncated datum are different things.
  auto string_schema = AvroSchema::Parse(R"("string")");
  ASSERT_TRUE(string_schema.ok());
  EXPECT_FALSE(
      security::avro::DecodeDatum(*string_schema, std::string("\x04\x61", 2))
          .ok());
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
  EXPECT_TRUE(absl::StrContains(encoded.status().message(),
                                "Rust panic caught while processing Avro input"))
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

// NEW FINDING, found by DatumCircleAgrees in 4.7 seconds of parallel fuzzing.
//
// avro::GenericDatum(NodePtr) builds a datum for every record field eagerly and
// guards neither depth nor cycles, so a record that reaches itself through
// records alone recurses until the stack is gone. Measured: 247 frames of
// GenericRecord/GenericDatum::init at about 1 KiB each, then SIGSEGV.
//
// Three schema shapes reach it, and the third is why this is not just "do not
// write recursive schemas":
//
//   1. a record referring to itself by name,
//   2. mutual reference, A holding B holding A,
//   3. two definitions of one full name, which avro-cpp resolves by turning the
//      second into a symbolic reference. The text never names a type twice, so
//      a caller cannot see this coming by reading its own schema.
//
// An array, map or union anywhere in the cycle saves it: those build empty or
// single-branch, so the recursion terminates. That is what made this survive
// months of the harness generating recursive named types -- the generator only
// emits a reference where a container can terminate it.
//
// avro-cpp is the reference implementation here, so by the scope rule this is an
// avro-cpp bug to report upstream rather than a bridge regression. The bridge
// parses all three shapes and builds no datum eagerly, so it does not crash.
// ToAvrocppDatum refuses these schemas before construction, which is what lets
// the differential properties run for an hour rather than dying on the first
// one.
// How the child dies depends on the build, so the predicate accepts any death.
// A plain build takes SIGSEGV on the guard page. Under ASan the fault is
// intercepted and turned into an abort, and the fuzzing build additionally
// installs absl's failure signal handler, which prints a trace and exits with a
// code rather than letting a signal terminate the process. Only the *manner* of
// death varies; a death test only consults this predicate when the statement
// failed to return, so accepting a non-zero exit still asserts the crash.
class DiedRatherThanReturned {
 public:
  bool operator()(int exit_status) const {
    if (WIFSIGNALED(exit_status)) {
      const int signal = WTERMSIG(exit_status);
      return signal == SIGSEGV || signal == SIGABRT;
    }
    return WIFEXITED(exit_status) && WEXITSTATUS(exit_status) != 0;
  }
};

TEST(Differential, RecursiveRecordSchemaCrashesAvrocppDatum) {
  const std::vector<std::string> crashing = {
      R"({"type":"record","name":"R","fields":[{"name":"a","type":"R"}]})",
      R"({"type":"record","name":"A","fields":[{"name":"b","type":)"
      R"({"type":"record","name":"B","fields":[{"name":"a","type":"A"}]}}]})",
      R"({"type":"record","name":"A","namespace":"ns","fields":[)"
      R"({"name":"f","type":{"type":"record","name":"A","fields":[]}}]})",
  };
  for (const std::string& text : crashing) {
    ::avro::ValidSchema schema;
    CppOutcome parsed = CallAvrocpp("compileJsonSchemaFromMemory", [&] {
      schema = ::avro::compileJsonSchemaFromMemory(
          reinterpret_cast<const uint8_t*>(text.data()), text.size());
    });
    ASSERT_TRUE(parsed.ok()) << "avro-cpp should still parse this: " << text;
    EXPECT_TRUE(AvroSchema::Parse(text).ok())
        << "the bridge should still parse this: " << text;

    EXPECT_EXIT(
        { ::avro::GenericDatum datum(schema); }, DiedRatherThanReturned(), "")
        << "avro-cpp no longer crashes building a datum for " << text
        << "; the upstream fix has landed and ToAvrocppDatum's guard can go";

    // A container in the cycle terminates the recursion, so the guard must not
    // be read as "recursive schemas crash".
    ::avro::GenericDatum survives;
    CppOutcome guarded = ToAvrocppDatum(Node{}, schema, &survives);
    EXPECT_FALSE(guarded.ok())
        << "the harness must refuse this schema before constructing a datum";
  }
}

TEST(Differential, RecursionUnderAContainerIsSafeInAvrocpp) {
  const std::vector<std::string> safe = {
      R"({"type":"record","name":"R","fields":[{"name":"a","type":)"
      R"({"type":"array","items":"R"}}]})",
      R"({"type":"record","name":"R","fields":[{"name":"a","type":)"
      R"({"type":"map","values":"R"}}]})",
      R"({"type":"record","name":"R","fields":[{"name":"a","type":)"
      R"(["null","R"]}]})",
  };
  for (const std::string& text : safe) {
    ::avro::ValidSchema schema;
    CppOutcome parsed = CallAvrocpp("compileJsonSchemaFromMemory", [&] {
      schema = ::avro::compileJsonSchemaFromMemory(
          reinterpret_cast<const uint8_t*>(text.data()), text.size());
    });
    ASSERT_TRUE(parsed.ok()) << parsed.what << " for " << text;
    CppOutcome built = CallAvrocpp("GenericDatum(NodePtr)", [&] {
      ::avro::GenericDatum datum(schema);
    });
    EXPECT_TRUE(built.ok())
        << "an array, map or union in the cycle should terminate it: " << text;
  }
}

// NEW FINDING, found by DecodersAgreeOnArbitraryBytes after 68 seconds and
// 430,000 runs of the one-hour parallel run.
//
// Four bytes under {"type":"array","items":"null"} and both engines accept, both
// return an array, and they disagree on how long it is. This is worse than the
// verdict splits in the table above: nothing errors, so neither caller has any
// signal that the other side read different data.
//
// An items type of null is what makes it reachable. A null item occupies zero
// bytes, so a declared block count needs no payload to back it and the decoders
// never run out of input to notice the framing is wrong. Read strictly, the
// input declares one item, then a negative-count block of one item with a byte
// size of zero, then a zero count to end the array: two items. Both engines
// return far more than that, by different amounts, so both are fabricating.
//
// The byte harness records this root cause as `array-block-framing`; what is new
// here is that it also produces a *value* difference rather than only a verdict
// difference, which is why ARRAY_LEN had never fired before.
//
// The exact counts are build-dependent, so only the disagreement is asserted.
// The one-hour ASan fuzzing build reported bridge 32 against avro-cpp 26; this
// build, without a sanitizer, gives bridge 2 against avro-cpp 0 for the input
// the fuzzer printed. Neither pair is explained. A length that moves with the
// build is a length that does not come from the input alone, which points at
// memory past the end of the buffer being read on at least one side -- worth
// investigating on its own, and the reason this test pins the relation rather
// than the numbers.
TEST(Differential, ArrayOfNullLengthsDisagree) {
  const std::string schema_text = R"({"type":"array","items":"null"})";
  const std::string bytes("\x02\x01\x00\x00", 4);

  auto bridge_schema = AvroSchema::Parse(schema_text);
  ASSERT_TRUE(bridge_schema.ok()) << bridge_schema.status();
  ::avro::ValidSchema cpp_schema;
  CppOutcome parsed = CallAvrocpp("compileJsonSchemaFromMemory", [&] {
    cpp_schema = ::avro::compileJsonSchemaFromMemory(
        reinterpret_cast<const uint8_t*>(schema_text.data()),
        schema_text.size());
  });
  ASSERT_TRUE(parsed.ok()) << parsed.what;

  auto decoded = security::avro::DecodeDatum(*bridge_schema, bytes);
  ASSERT_TRUE(decoded.ok()) << "the bridge is expected to accept these bytes: "
                            << decoded.status();
  auto bridge_len = decoded->GetArrayLen();
  ASSERT_TRUE(bridge_len.ok());

  ::avro::GenericDatum cpp_datum;
  CppOutcome cpp = DecodeWithAvrocpp(cpp_schema, bytes, &cpp_datum);
  ASSERT_TRUE(cpp.ok()) << "avro-cpp is expected to accept them too: "
                        << cpp.what;
  const size_t cpp_len = cpp_datum.value<::avro::GenericArray>().value().size();

  EXPECT_NE(*bridge_len, cpp_len)
      << "the two engines now agree at " << *bridge_len
      << " items; this finding is closed and ARRAY_LEN can come out of the run "
         "script's suppression list";
}

// NEW FINDING, found by DecodersAgreeOnArbitraryBytes after 293,811 runs.
//
// A string of exactly 16 bytes under a `uuid` logical type is read as a binary
// UUID by the bridge and as the text it is by avro-cpp. Both succeed, so the
// caller gets no signal that the value changed meaning.
//
//   schema: {"type":"string","logicalType":"uuid"}
//   input:  20 00*12 62 6f 6c 73        (length 16, then twelve NULs, "bols")
//
//   avrocpp: the 16 bytes verbatim
//   bridge:  "00000000-0000-0000-0000-0000626f6c73"
//
// The tail of the bridge's rendering, 626f6c73, is "bols" read as hex: the bytes
// were reinterpreted, not reformatted. The Avro specification puts `uuid` on
// `string` and defines its encoding as the 36-character text form, so avro-cpp is
// reading what the spec says is there. apache-avro additionally accepts a
// 16-byte payload as a binary UUID, which is what a uuid-on-fixed(16) field
// carries in Avro 1.12.
//
// Length 16 exactly is what makes it reachable, and why it took five figures of
// runs to find: any other length and both engines agree.
TEST(Differential, SixteenByteUuidStringIsReadAsBinaryByTheBridge) {
  const std::string schema_text = R"({"type":"string","logicalType":"uuid"})";
  const std::string payload = std::string(12, '\0') + "bols";
  ASSERT_EQ(payload.size(), 16u);
  const std::string bytes = Varint(static_cast<int64_t>(payload.size())) + payload;

  auto bridge_schema = AvroSchema::Parse(schema_text);
  ASSERT_TRUE(bridge_schema.ok()) << bridge_schema.status();
  ::avro::ValidSchema cpp_schema;
  CppOutcome parsed = CallAvrocpp("compileJsonSchemaFromMemory", [&] {
    cpp_schema = ::avro::compileJsonSchemaFromMemory(
        reinterpret_cast<const uint8_t*>(schema_text.data()),
        schema_text.size());
  });
  ASSERT_TRUE(parsed.ok()) << parsed.what;

  ::avro::GenericDatum cpp_datum;
  CppOutcome cpp = DecodeWithAvrocpp(cpp_schema, bytes, &cpp_datum);
  ASSERT_TRUE(cpp.ok()) << cpp.what;
  EXPECT_EQ(cpp_datum.value<std::string>(), payload)
      << "avro-cpp is expected to hand back the bytes as written";

  auto decoded = security::avro::DecodeDatum(*bridge_schema, bytes);
  ASSERT_TRUE(decoded.ok()) << decoded.status();
  auto text = decoded->GetUuid();
  ASSERT_TRUE(text.ok()) << text.status();
  EXPECT_EQ(*text, "00000000-0000-0000-0000-0000626f6c73")
      << "the bridge is expected to read the 16 bytes as a binary uuid; if it "
         "now returns them verbatim, this finding is fixed";
  EXPECT_NE(*text, payload) << "the two engines now agree";
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
