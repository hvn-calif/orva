#ifndef SECURITY_AVRO_FUZZ_SUPPRESS_H_
#define SECURITY_AVRO_FUZZ_SUPPRESS_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "absl/strings/string_view.h"

// The divergence registry and the suppression mechanism.
//
// Every difference the harness can report has a stable ID declared here, so a
// typo in a suppression file is caught at startup rather than silently muting
// nothing. Suppression is opt-in and empty by default: at commit 84ce4af, D1
// and D2 are open bugs, and the harness proving itself means finding them from
// an empty corpus without being told to look.
namespace security::avro_fuzz {

// X(enumerator, wire name, one-line meaning)
//
// Wire names starting with "D" match the IDs in doc/AvrocppDivergences.md so a
// suppression entry can cite the register. The rest are findings the register
// has no name for yet.
#define AVRO_FUZZ_DIVERGENCES(X)                                              \
  X(kD1StringNotUtf8, "D1",                                                   \
    "non-UTF-8 bytes in a string: avrocpp accepts, the bridge rejects")       \
  X(kD2DuplicateMapKey, "D2",                                                 \
    "duplicate map keys: avrocpp keeps both, the bridge collapses")           \
  X(kD5OutOfRangeIndex, "D5",                                                 \
    "out-of-range union branch: avrocpp throws std::out_of_range")            \
  X(kD6RangeCheckSite, "D6",                                                  \
    "enum or union range check happens at a different point")                 \
  X(kD7ArrayContinuation, "D7",                                               \
    "negative-count array continuation block: the bridge accepts, avrocpp "   \
    "fails")                                                                  \
  X(kD8ZstdCodec, "D8", "zstandard container codec: avrocpp has no such codec") \
  X(kD9AllocationCeiling, "D9",                                               \
    "the bridge's allocation ceiling fired; avrocpp has none, so the two are " \
    "not comparable on this input")                                           \
  X(kLogicalTypeAbsentIn1114, "LOGICAL_TYPE_ABSENT_IN_1114",                  \
    "timestamp-nanos and the local-timestamp family do not exist in avrocpp " \
    "1.11.4, which falls back to the underlying long")                        \
  X(kBridgeCannotRepresent, "BRIDGE_CANNOT_REPRESENT",                        \
    "the bridge's value constructor rejected a value avrocpp accepts")        \
  X(kSchemaParseVerdict, "SCHEMA_PARSE_VERDICT",                              \
    "the two parsers disagree on whether a schema is legal")                  \
  X(kCrossParseRoundTrip, "CROSS_PARSE_ROUND_TRIP",                           \
    "a schema rendered by one engine does not reparse in the other")          \
  X(kSchemaResolutionVerdict, "SCHEMA_RESOLUTION_VERDICT",                    \
    "the two engines disagree on reader/writer compatibility")               \
  X(kResolvedValue, "RESOLVED_VALUE",                                         \
    "resolution succeeded on both sides but produced different values")       \
  X(kDecodeVerdictBridgeLenient, "DECODE_VERDICT_BRIDGE_LENIENT",             \
    "the bridge accepted bytes avrocpp rejected")                             \
  X(kDecodeVerdictAvrocppLenient, "DECODE_VERDICT_AVROCPP_LENIENT",           \
    "avrocpp accepted bytes the bridge rejected")                             \
  X(kValueTypeMismatch, "VALUE_TYPE_MISMATCH",                                \
    "the two engines decoded the same bytes to different types")              \
  X(kStringBytesTypeMismatch, "STRING_BYTES_TYPE_MISMATCH",                   \
    "one engine produced a string where the other produced bytes")            \
  X(kRecordFieldNames, "RECORD_FIELD_NAMES", "record field names differ")     \
  X(kRecordArity, "RECORD_ARITY", "record field counts differ")               \
  X(kArrayLen, "ARRAY_LEN", "array lengths differ")                           \
  X(kArrayItemFabricatedByAvrocpp, "ARRAY_ITEM_FABRICATED",                    \
    "avrocpp returned a null datum where the item schema is not null")         \
  X(kMapKeySet, "MAP_KEY_SET", "map key sets differ")                         \
  X(kMapArity, "MAP_ARITY", "map entry counts differ")                        \
  X(kUnionBranch, "UNION_BRANCH", "union branch indices differ")              \
  X(kEnumPosition, "ENUM_POSITION", "enum positions differ")                  \
  X(kEnumSymbol, "ENUM_SYMBOL", "enum symbols differ for the same position")  \
  X(kScalarValue, "SCALAR_VALUE", "scalar payloads differ")                   \
  X(kFloatNanPayload, "FLOAT_NAN_PAYLOAD",                                    \
    "both sides are NaN but the payload bits differ")                         \
  X(kFloatSignedZero, "FLOAT_SIGNED_ZERO", "-0.0 on one side, +0.0 on the other") \
  X(kDecimalValue, "DECIMAL_VALUE", "decimal values differ numerically")      \
  X(kDecimalSignPadding, "DECIMAL_SIGN_PADDING",                              \
    "decimals are numerically equal but encoded with different sign padding") \
  X(kDurationFields, "DURATION_FIELDS", "duration month/day/millis differ")   \
  X(kUuidTextNotPreserved, "UUID_TEXT_NOT_PRESERVED",                         \
    "uuid canonicalises to the same value but the bridge did not preserve "   \
    "the original text")                                                      \
  X(kUuidInvalidRejected, "UUID_INVALID_REJECTED",                            \
    "the bridge rejected a uuid string avrocpp accepted verbatim")            \
  X(kRustPanicCaught, "RUST_PANIC_CAUGHT",                                    \
    "the bridge returned a clean status whose text says apache-avro panicked") \
  X(kOcfValueCount, "OCF_VALUE_COUNT",                                        \
    "the two engines read a different number of values from one container")   \
  X(kOcfReadRejected, "OCF_READ_REJECTED",                                    \
    "one engine rejected a container file the other wrote")                   \
  X(kStreamingDiverges, "STREAMING_DIVERGES",                                 \
    "the streaming reader disagrees with the whole-buffer reader")            \
  X(kReencodeNotByteIdentical, "REENCODE_NOT_BYTE_IDENTICAL",                 \
    "decode-then-reencode did not reproduce the original bytes")               \
  X(kConsumptionDiffers, "CONSUMPTION_DIFFERS",                                \
    "both engines decoded the input but read a different number of bytes, so " \
    "the values they returned came from different offsets and comparing them " \
    "says nothing about either engine")

enum class DivergenceId : uint16_t {
  kNone = 0,
#define AVRO_FUZZ_DECLARE_ID(name, wire, doc) name,
  AVRO_FUZZ_DIVERGENCES(AVRO_FUZZ_DECLARE_ID)
#undef AVRO_FUZZ_DECLARE_ID
};

// Returns the stable wire name, e.g. "D1". Returns "NONE" for kNone.
const char* DivergenceName(DivergenceId id);

// Returns the one-line meaning, for the startup banner and failure messages.
const char* DivergenceDoc(DivergenceId id);

// Parses a wire name. Returns kNone when the name is not registered, which
// callers treat as a fatal configuration error rather than a no-op.
DivergenceId LookupDivergence(absl::string_view wire_name);

// A set of muted divergences.
//
// Two mechanisms share this one configuration source. Divergences that are a
// property of the *input* (D1, D2) are suppressed by Normalize removing the
// triggering input class before either lowering runs, so both engines provably
// see identical input and a suppression can never manufacture a false
// agreement. Divergences found at comparison time are recorded and skipped.
class SuppressionSet {
 public:
  // `evidence` narrows a coarse ID to one substring of the detail text. An
  // entry with no evidence strings suppresses its ID unconditionally.
  bool Contains(DivergenceId id, absl::string_view evidence = "") const;

  bool empty() const { return entries_.empty(); }

  // Sorted and comma-joined. Included in every failure message, because a
  // reproducer captured under one suppression set is uninterpretable under
  // another.
  std::string Render() const;

  void Add(DivergenceId id, absl::string_view evidence = "");

 private:
  std::map<DivergenceId, std::vector<std::string>> entries_;
};

// Parses the suppression configuration once per process, from the file named
// by AVRO_FUZZ_SUPPRESS_FILE (default "fuzz/suppressions.txt") and then from
// the comma-separated AVRO_FUZZ_SUPPRESS environment variable.
//
// Environment rather than a command-line flag because FuzzTest binaries share
// argv with the engine, and env vars survive corpus replay and single-input
// reproduction identically.
const SuppressionSet& Suppressions();

// Parses one suppression file body. Exposed for testing. Returns false and
// fills `error` when a line names an unregistered ID.
bool ParseSuppressions(absl::string_view text, SuppressionSet* out,
                       std::string* error);

// One reported difference.
struct Finding {
  DivergenceId id = DivergenceId::kNone;
  // Where in the value tree, e.g. `$.user<branch 1>.tags[2]{"61ff"}`. Map keys
  // are hex-escaped so a non-UTF-8 key cannot corrupt the message.
  std::string path;
  // Human-readable detail. Must contain no addresses or timings: FuzzTest
  // deduplicates failures by message, and an address makes every failure look
  // unique.
  std::string detail;
  // Normalised text that a suppression's `:substring` qualifier matches
  // against. Kept separate from `detail` so the qualifier does not depend on
  // incidental phrasing.
  std::string evidence;

  std::string Render() const;
};

// Collects findings in a fixed traversal order, so which one is reported first
// is reproducible.
class FindingLog {
 public:
  explicit FindingLog(const SuppressionSet* suppressions)
      : suppressions_(suppressions) {}

  // Records a finding, or counts it as suppressed. Returns true when it was
  // recorded, i.e. when the caller should treat it as a real difference.
  bool Report(DivergenceId id, absl::string_view path, absl::string_view detail,
              absl::string_view evidence = "");

  bool empty() const { return findings_.empty(); }
  size_t suppressed_count() const { return suppressed_count_; }
  const std::vector<Finding>& findings() const { return findings_; }

  // Full failure text: every finding, then the active suppression set.
  std::string Render() const;

 private:
  const SuppressionSet* suppressions_;
  std::vector<Finding> findings_;
  size_t suppressed_count_ = 0;
};

}  // namespace security::avro_fuzz

#endif  // SECURITY_AVRO_FUZZ_SUPPRESS_H_
