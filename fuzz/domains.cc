#include "fuzz/domains.h"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "fuzz/ir.h"
#include "fuzz/lower_schema.h"
#include "fuzztest/fuzztest.h"

namespace security::avro_fuzz {
namespace {

using fuzztest::Arbitrary;
using fuzztest::ElementOf;
using fuzztest::InRange;
using fuzztest::Just;
using fuzztest::OneOf;
using fuzztest::PairOf;
using fuzztest::StructOf;
using fuzztest::VectorOf;

// Kinds with no children. Enum, fixed, decimal-on-fixed and duration are named
// types but still leaves, since their structure is in their attributes.
const std::vector<Kind>& LeafKinds() {
  static const std::vector<Kind>* kinds = new std::vector<Kind>{
      Kind::kNull,          Kind::kBoolean,       Kind::kInt,
      Kind::kLong,          Kind::kFloat,         Kind::kDouble,
      Kind::kBytes,         Kind::kString,        Kind::kEnum,
      Kind::kFixed,         Kind::kDecimalBytes,  Kind::kDecimalFixed,
      Kind::kUuid,          Kind::kDate,          Kind::kTimeMillis,
      Kind::kTimeMicros,    Kind::kTimestampMillis,
      Kind::kTimestampMicros,
      // The four below do not exist in avro-cpp 1.11.4, which falls back to
      // the underlying long. Generated on purpose: that silent downgrade is a
      // real migration hazard worth characterising, not noise to exclude.
      Kind::kTimestampNanos,
      Kind::kLocalTimestampMillis, Kind::kLocalTimestampMicros,
      Kind::kLocalTimestampNanos,  Kind::kDuration};
  return *kinds;
}

const std::vector<Kind>& CompositeKinds() {
  static const std::vector<Kind>* kinds = new std::vector<Kind>{
      // Arrays and maps are repeated so they outweigh records. Both are
      // termination guards, which is what makes a recursive named type
      // inhabitable, and recursion is otherwise very hard to reach.
      Kind::kRecord, Kind::kArray, Kind::kArray,
      Kind::kMap,    Kind::kMap,   Kind::kUnion};
  return *kinds;
}

fuzztest::Domain<Naming> AnyNaming() {
  return StructOf<Naming>(
      // Weighted toward the reuse strategies: shadowing and references are the
      // interesting cases, and kFresh can never collide by construction.
      ElementOf<NameStrategy>({NameStrategy::kFresh, NameStrategy::kPool,
                               NameStrategy::kPool,
                               NameStrategy::kReuseAncestor,
                               NameStrategy::kReuseAncestor,
                               NameStrategy::kReuseAnyInScope,
                               NameStrategy::kReserved}),
      InRange<uint8_t>(0, 15),
      InRange<uint8_t>(0, 7),
      // Biased true: a reference is only honoured where the value can still
      // terminate, so most of these are demoted to definitions anyway.
      ElementOf<bool>({true, true, false}),
      Arbitrary<bool>(),
      Arbitrary<bool>());
}

fuzztest::Domain<Scalars> AnyScalars() {
  return StructOf<Scalars>(
      Arbitrary<bool>(),
      // Zigzag varint boundaries, int/long boundaries, and the extremes.
      OneOf(ElementOf<int64_t>({0, 1, -1, 63, 64, -64, -65, 8191, 8192,
                                std::numeric_limits<int32_t>::min(),
                                std::numeric_limits<int32_t>::max(),
                                int64_t{std::numeric_limits<int32_t>::max()} + 1,
                                int64_t{std::numeric_limits<int32_t>::min()} - 1,
                                std::numeric_limits<int64_t>::min(),
                                std::numeric_limits<int64_t>::max()}),
            Arbitrary<int64_t>()),
      // Arbitrary<float>/<double> mutate at bit level, which is what reaches
      // non-canonical NaN payloads and negative zero.
      Arbitrary<float>(),
      Arbitrary<double>(),
      AnyPayload(),
      OneOf(ElementOf<uint32_t>({0, 1, 0xFFFFFFFF}), Arbitrary<uint32_t>()),
      OneOf(ElementOf<uint32_t>({0, 1, 0xFFFFFFFF}), Arbitrary<uint32_t>()),
      OneOf(ElementOf<uint32_t>({0, 1, 0xFFFFFFFF}), Arbitrary<uint32_t>()));
}

fuzztest::Domain<Selectors> AnySelectors() {
  return StructOf<Selectors>(
      // kJustPast and kFarPast are deliberately out of range. Normalize forces
      // kRawMod in value-bearing mode, so only the property that tests
      // out-of-range indices ever sees them.
      ElementOf<IndexMode>({IndexMode::kFirst, IndexMode::kLast,
                            IndexMode::kMiddle, IndexMode::kJustPast,
                            IndexMode::kFarPast, IndexMode::kRawMod,
                            IndexMode::kRawMod}),
      Arbitrary<uint32_t>(),
      OneOf(ElementOf<uint8_t>({0, 1, 12, 16, 255}), InRange<uint8_t>(1, 64)),
      OneOf(ElementOf<uint8_t>({0, 1, 18, 19, 29, 38, 255}),
            InRange<uint8_t>(1, 40)),
      ElementOf<ScaleMode>({ScaleMode::kZero, ScaleMode::kOne,
                            ScaleMode::kEqPrecision, ScaleMode::kPrecisionPlus1,
                            ScaleMode::kNegative, ScaleMode::kRaw}),
      Arbitrary<int8_t>());
}

// Builds one Node layer. `children` supplies the recursive position; passing a
// domain that always yields an empty vector makes a leaf layer.
fuzztest::Domain<Node> NodeLayer(fuzztest::Domain<Kind> kinds,
                                 fuzztest::Domain<std::vector<Node>> children) {
  return StructOf<Node>(std::move(kinds), AnyNaming(), AnyScalars(),
                        AnySelectors(), std::move(children),
                        VectorOf(AnyName()).WithMaxSize(kMaxBreadth + 1),
                        VectorOf(AnyMapKey()).WithMaxSize(kMaxBreadth + 1));
}

fuzztest::Domain<Node> LeafLayer() {
  return NodeLayer(ElementOf<Kind>(LeafKinds()), Just(std::vector<Node>{}));
}

}  // namespace

fuzztest::Domain<std::string> AnyName() {
  static const std::vector<std::string>* pool = [] {
    auto* names = new std::vector<std::string>();
    for (int i = 0; i < kNamePoolSize; ++i) names->push_back(kNamePool[i]);
    // Repeat the pool so it outweighs the freely generated names below.
    for (int i = 0; i < kNamePoolSize; ++i) names->push_back(kNamePool[i]);
    for (int i = 0; i < kReservedWordsSize; ++i) {
      names->push_back(kReservedWords[i]);
    }
    return names;
  }();
  return OneOf(ElementOf<std::string>(*pool),
               fuzztest::InRegexp("[A-Za-z_][A-Za-z0-9_]{0,3}"));
}

fuzztest::Domain<std::string> AnyMapKey() {
  static const std::vector<std::string>* keys =
      new std::vector<std::string>{"",  "a",  "A",  "b",
                                   "aa", "ab", std::string("\0", 1), "a\xff"};
  return OneOf(ElementOf<std::string>(*keys), AnyPayload());
}

fuzztest::Domain<std::string> AnyPayload() {
  static const std::vector<std::string>* blobs = new std::vector<std::string>{
      "",
      std::string("\0", 1),          // embedded NUL
      std::string("a\0b", 3),        // NUL in the middle
      "\xff",                        // bare continuation-less byte
      "\xfe\xff",
      "\x80",
      "\xc2",                        // truncated two-byte lead
      "\xc0\x80",                    // overlong NUL
      "\xc0\xaf",                    // overlong '/'
      "\xe0\x80\x80",                // overlong
      "\xed\xa0\x80",                // lone high surrogate
      "\xed\xb0\x80",                // lone low surrogate
      "\xed\xa0\x80\xed\xb0\x80",    // CESU-8 surrogate pair
      "\xf4\x90\x80\x80",            // beyond U+10FFFF
      "\xef\xbb\xbf",                // BOM
      "\xef\xbf\xbd",                // U+FFFD
      "\xef\xbf\xbe",                // noncharacter
      "caf\xc3\xa9",                 // valid multibyte
      "\xf0\x9f\x92\xa9",            // valid astral
      // uuid shapes, for Kind::kUuid
      "0f9a1c8e-1111-4222-8333-444455556666",
      "0F9A1C8E-1111-4222-8333-444455556666",
      "urn:uuid:0f9a1c8e-1111-4222-8333-444455556666",
      "{0f9a1c8e-1111-4222-8333-444455556666}",
      "0f9a1c8e11114222833344445555666",
      "not-a-uuid",
  };
  return OneOf(ElementOf<std::string>(*blobs),
               Arbitrary<std::string>().WithMaxSize(16));
}

fuzztest::Domain<Node> AnyLeaf() { return LeafLayer(); }

fuzztest::Domain<Node> AnyTree(int max_depth) {
  // Built bottom-up so depth is a hard bound. Each layer is constructed once
  // and shared by the layer above, so the domain graph is O(depth) objects
  // rather than O(breadth^depth).
  fuzztest::Domain<Node> layer = LeafLayer();
  for (int i = 0; i < max_depth; ++i) {
    fuzztest::Domain<Node> composite =
        NodeLayer(ElementOf<Kind>(CompositeKinds()),
                  VectorOf(layer).WithMinSize(1).WithMaxSize(kMaxBreadth));
    layer = OneOf(LeafLayer(), std::move(composite));
  }
  return layer;
}

fuzztest::Domain<std::string> AnySchemaText() {
  // Shapes a structure-aware generator cannot reach. Empty containers and
  // truncations are the point; the well-formed entries are here so a mutation
  // starts from something a parser gets past its first character.
  static const std::vector<std::string>* seeds = new std::vector<std::string>{
      "[]",                                  // empty union
      "[[]]",                                // union of empty union
      R"({"type":"enum","name":"E","symbols":[]})",       // empty enum
      R"({"type":"record","name":"R","fields":[]})",      // legal: empty record
      R"({"type":"record","name":"R","fields":[{"name":"a","type":[]}]})",
      R"({"type":"array"})",                 // missing items
      R"({"type":"map"})",                   // missing values
      R"({"type":"fixed","name":"F"})",      // missing size
      R"({"type":"fixed","name":"F","size":-1})",
      R"({"type":"fixed","name":"F","size":99999999999999999999})",
      R"({"type":)",                         // truncated
      R"({"type":"record","name":"R","fields":[)",
      "{}",
      "null",
      "0",
      R"("")",
      R"("int")",
      R"(["int","int"])",                    // duplicate union branch type
      R"({"type":"int","logicalType":"unknown-logical-type"})",
      R"({"type":"bytes","logicalType":"decimal","precision":0,"scale":1})",
      R"({"type":"record","name":"R","fields":[{"name":"a","type":"R"}]})",
      R"({"type":"\uD800"})",                // lone surrogate in an escape
      "\xef\xbb\xbf\"int\"",                 // byte-order mark before a schema
      R"({"type":"string","default":})",
  };
  return OneOf(ElementOf<std::string>(*seeds),
               // Every schema the tree generator can build, as text. Gives the
               // mutator a valid starting point to corrupt.
               fuzztest::Map(
                   [](const Node& node) {
                     return ToSchemaJson(Normalize(node, NormalizeOptions{}));
                   },
                   AnyTree()),
               Arbitrary<std::string>().WithMaxSize(64));
}

fuzztest::Domain<Node> AnyDeepChain(int max_depth) {
  fuzztest::Domain<Node> layer = LeafLayer();
  for (int i = 0; i < max_depth; ++i) {
    // Single-branch: an array of exactly one item type. Deep, but linear.
    layer = NodeLayer(ElementOf<Kind>({Kind::kArray, Kind::kMap}),
                      VectorOf(layer).WithSize(1));
  }
  return layer;
}

}  // namespace security::avro_fuzz
