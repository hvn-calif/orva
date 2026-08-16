#ifndef SECURITY_AVRO_FUZZ_IR_H_
#define SECURITY_AVRO_FUZZ_IR_H_

#include <cstdint>
#include <string>
#include <vector>

// The intermediate representation the differential fuzzer generates.
//
// A single `Node` tree carries both an Avro schema and a value inhabiting it.
// Three lowerings turn one tree into a schema JSON string, a bridge
// `AvroValue`, and an avrocpp `GenericDatum`, so "the value matches the
// schema" holds by construction and the mutator has exactly one structure to
// work on.
//
// The alternative -- generate a schema, then build a value domain for it at
// run time -- would reconstruct a type-erased domain graph on every execution,
// discard value-side corpus progress whenever the schema mutated, and shrink
// incoherently because shrinking a schema orphans its value.
namespace security::avro_fuzz {

enum class Kind : uint8_t {
  // Primitives.
  kNull,
  kBoolean,
  kInt,
  kLong,
  kFloat,
  kDouble,
  kBytes,
  kString,

  // Complex.
  kRecord,
  kEnum,
  kArray,
  kMap,
  kUnion,
  kFixed,

  // Logical. Decimal appears twice because avrocpp models the two backings as
  // different datum types: AVRO_BYTES versus AVRO_FIXED.
  kDecimalBytes,
  kDecimalFixed,
  kUuid,
  kDate,
  kTimeMillis,
  kTimeMicros,
  kTimestampMillis,
  kTimestampMicros,
  kTimestampNanos,
  kLocalTimestampMillis,
  kLocalTimestampMicros,
  kLocalTimestampNanos,
  kDuration,

  kMaxKind,
};

// Whether a Kind names a type whose schema JSON is a definition with a name,
// and which therefore participates in scope resolution.
bool IsNamedType(Kind kind);

// Whether a Kind has children in the schema sense (as opposed to a leaf).
bool IsComplex(Kind kind);

// Returns the Avro type name, e.g. "record", "timestamp-millis".
const char* KindName(Kind kind);

// How an index into a container is derived from that container's real size.
//
// Storing the *intent* rather than a raw number is what keeps every generated
// value meaningful for every tree: the index is resolved against the actual
// child count at lowering time, so no input is ever discarded as malformed and
// no `fuzztest::Filter` is needed. It also keeps deliberately out-of-range
// indices reachable, which is the point of kJustPast and kFarPast.
enum class IndexMode : uint8_t {
  kFirst,     // 0
  kLast,      // size - 1, or 0 when empty
  kMiddle,    // size / 2
  kJustPast,  // size -- out of range on purpose
  kFarPast,   // 0xFFFFFFFF -- out of range on purpose
  kRawMod,    // raw % size -- always in range
  kMaxIndexMode,
};

// How a decimal's scale is derived from its precision. Avro requires
// 0 <= scale <= precision; the modes that violate that are deliberate.
enum class ScaleMode : uint8_t {
  kZero,
  kOne,
  kEqPrecision,
  kPrecisionPlus1,  // illegal on purpose
  kNegative,        // illegal on purpose
  kRaw,
  kMaxScaleMode,
};

// How a named type's name is chosen, resolved against the enclosing scope.
//
// Avro name collisions are where parsers disagree, and a freely generated
// identifier collides essentially never. Naming a *strategy* instead makes
// shadowing, redefinition and recursive references routine rather than
// astronomically unlikely.
enum class NameStrategy : uint8_t {
  kFresh,            // never collides
  kPool,             // drawn from a tiny pool, so collisions are common
  kReuseAncestor,    // shadows the nearest enclosing named type
  kReuseAnyInScope,  // redefinition, or a reference to an existing definition
  kReserved,         // an Avro reserved word used as a user-defined name
  kMaxNameStrategy,
};

// Naming pools. Small on purpose: six names collide by the birthday bound
// within a handful of fields.
extern const char* const kNamePool[];
extern const int kNamePoolSize;
extern const char* const kReservedWords[];
extern const int kReservedWordsSize;
extern const char* const kNamespacePool[];
extern const int kNamespacePoolSize;

struct Naming {
  NameStrategy strategy = NameStrategy::kFresh;
  uint8_t name_id = 0;       // indexes kNamePool or kReservedWords
  uint8_t namespace_id = 0;  // indexes kNamespacePool; 0 means no namespace
  // A named type may be emitted as a bare reference to a name already in
  // scope instead of as a fresh definition. This is how recursive named types
  // arise.
  bool emit_as_reference = false;
  bool emit_aliases = false;
  bool emit_doc = false;
};

struct Scalars {
  bool boolean = false;
  // Serves int/long and every date, time and timestamp logical type. Narrowed
  // to 32 bits by the lowering where the Kind requires it.
  int64_t integer = 0;
  float f32 = 0.0f;
  double f64 = 0.0;
  // bytes, string, fixed body, decimal unscaled bytes, uuid text. Arbitrary
  // bytes on purpose: invalid UTF-8 here is what reaches D1.
  std::string blob;
  uint32_t months = 0;
  uint32_t days = 0;
  uint32_t millis = 0;
};

struct Selectors {
  IndexMode index_mode = IndexMode::kRawMod;
  uint32_t index_raw = 0;  // enum position, union branch
  uint8_t fixed_size = 1;
  uint8_t precision = 1;
  ScaleMode scale_mode = ScaleMode::kZero;
  int8_t scale_raw = 0;  // signed, so a negative scale is reachable
};

struct Node {
  Kind kind = Kind::kNull;
  Naming naming;
  Scalars scalars;
  Selectors selectors;

  // The recursive position.
  //
  // `std::vector<Node>` with `Node` incomplete here is explicitly allowed
  // (C++17 [vector.overview]/4). `std::pair<std::string, Node>` with `Node`
  // incomplete is NOT -- it is undefined behaviour. So the names that go with
  // these children live in the parallel vectors below, zipped by index, never
  // in a vector of pairs.
  //
  // Holds record fields, array items, map values and union branches.
  std::vector<Node> children;

  // Record field names and enum symbols. A missing entry is synthesised as
  // "f<i>" during lowering, so a child is never silently dropped and any
  // duplicate that does appear is a deliberate one.
  std::vector<std::string> labels;

  // Map keys, synthesised as "k<i>" when missing. Duplicates here are how D2
  // (the bridge collapses, avrocpp keeps both) is reached.
  std::vector<std::string> keys;
};

// Returns the label for child `i`, synthesising one if `labels` is short.
std::string LabelAt(const Node& node, size_t i);

// Returns the map key for child `i`, synthesising one if `keys` is short.
std::string KeyAt(const Node& node, size_t i);

// Resolves an index against a real container size. Out-of-range results are
// intentional for kJustPast and kFarPast.
uint32_t ResolveIndex(const Selectors& selectors, size_t size);

// Resolves a decimal scale against a precision. May return an illegal value.
int32_t ResolveScale(const Selectors& selectors);

enum class NormalizeMode : uint8_t {
  // Leave illegality in place: duplicate enum symbols, scale > precision,
  // reserved words as names, redefinitions. Used by the properties that ask
  // "do the two parsers agree on whether this is legal?".
  kSchemaOnly,
  // Guarantee the tree parses on both engines and has a finite instance, so
  // the round-trip properties spend their time on values rather than on
  // rejected schemas.
  kValueBearing,
};

class SuppressionSet;

struct NormalizeOptions {
  NormalizeMode mode = NormalizeMode::kValueBearing;
  int max_depth = 6;
  int max_breadth = 4;
  int max_nodes = 64;
  // When a divergence is suppressed, Normalize removes the input class that
  // triggers it -- before either lowering runs, so both engines provably see
  // the same input and a suppression can never manufacture a false agreement.
  const SuppressionSet* suppressions = nullptr;
};

// Rewrites `raw` into a tree satisfying `options`. Total: succeeds for every
// input.
Node Normalize(const Node& raw, const NormalizeOptions& options);

// Emits a C++ aggregate initialiser for `node`, so a triager can paste a
// failing input straight into ir_test.cc as a regression seed.
std::string ToCppLiteral(const Node& node);

// Renders a compact one-line summary for failure messages.
std::string ToDebugString(const Node& node);

// Teaches FuzzTest (and abseil) how to print a Node.
//
// Without this, FuzzTest falls back to its generic aggregate printer, which
// walks every field of every node including the recursive vectors. On a tree
// of any size that becomes pathologically slow -- slow enough to look like a
// hang during shrinking, which is exactly how it first showed up.
template <typename Sink>
void AbslStringify(Sink& sink, const Node& node) {
  sink.Append(ToDebugString(node));
}

}  // namespace security::avro_fuzz

#endif  // SECURITY_AVRO_FUZZ_IR_H_
