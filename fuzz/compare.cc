#include "fuzz/compare.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/escaping.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "avro/GenericDatum.hh"
#include "avro/LogicalType.hh"
#include "avro/Types.hh"
#include "fuzz/suppress.h"

namespace security::avro_fuzz {
namespace {

using ::security::avro::AvroValue;

// Drops redundant sign bytes from a two's-complement big-endian integer, so two
// encodings of the same number compare equal on value even when their lengths
// differ.
std::string MinimalTwosComplement(absl::string_view raw) {
  size_t start = 0;
  while (start + 1 < raw.size()) {
    const unsigned char lead = static_cast<unsigned char>(raw[start]);
    const unsigned char next = static_cast<unsigned char>(raw[start + 1]);
    const bool redundant_zero = lead == 0x00 && (next & 0x80) == 0;
    const bool redundant_ones = lead == 0xFF && (next & 0x80) != 0;
    if (!redundant_zero && !redundant_ones) break;
    ++start;
  }
  return std::string(raw.substr(start));
}

// Lowercases and strips the decorations avro-cpp preserves verbatim but the
// bridge normalises away.
std::string CanonicalUuid(absl::string_view raw) {
  absl::ConsumePrefix(&raw, "urn:uuid:");
  if (raw.size() >= 2 && absl::StartsWith(raw, "{") && absl::EndsWith(raw, "}")) {
    raw = raw.substr(1, raw.size() - 2);
  }
  return absl::AsciiStrToLower(raw);
}

template <typename T, typename Bits>
bool SameBits(T a, T b) {
  Bits left;
  Bits right;
  std::memcpy(&left, &a, sizeof(Bits));
  std::memcpy(&right, &b, sizeof(Bits));
  return left == right;
}

struct Comparer {
  FindingLog* log;
  CompareOptions options;
  bool clean = true;

  void Report(DivergenceId id, absl::string_view path, absl::string_view detail,
              absl::string_view evidence = "") {
    if (log->Report(id, path, detail, evidence)) clean = false;
  }

  // Reads a bridge string-or-bytes payload without caring which tag it carries.
  // Used only after the tags have already been compared.
  std::string TextOf(const AvroValue& value) {
    if (value.IsString()) return value.GetString().value_or("");
    if (value.IsBytes()) return value.GetBytes().value_or("");
    if (value.IsFixed()) return value.GetFixedBytes().value_or("");
    if (value.IsUuid()) return value.GetUuid().value_or("");
    if (value.IsDecimal()) return value.GetDecimalBytes().value_or("");
    return "";
  }

  void Compare(const AvroValue& bridge, const ::avro::GenericDatum& cpp,
               absl::string_view path);

  void CompareRecord(const AvroValue& bridge, const ::avro::GenericDatum& cpp,
                     absl::string_view path);
  void CompareArray(const AvroValue& bridge, const ::avro::GenericDatum& cpp,
                    absl::string_view path);
  void CompareMap(const AvroValue& bridge, const ::avro::GenericDatum& cpp,
                  absl::string_view path);
};

void Comparer::CompareRecord(const AvroValue& bridge,
                             const ::avro::GenericDatum& cpp,
                             absl::string_view path) {
  const auto& record = cpp.value<::avro::GenericRecord>();
  auto names = bridge.GetRecordFieldNames();
  if (!names.ok()) {
    Report(DivergenceId::kValueTypeMismatch, path,
           "the bridge would not enumerate record fields");
    return;
  }
  if (names->size() != record.fieldCount()) {
    Report(DivergenceId::kRecordArity, path,
           absl::StrCat("field count differs: bridge ", names->size(),
                        ", avro-cpp ", record.fieldCount()));
    return;
  }
  for (size_t i = 0; i < names->size(); ++i) {
    const std::string& name = (*names)[i];
    const std::string here = absl::StrCat(path, ".", name);
    // Field order is significant on both sides, so compare positionally and
    // check the names agree at each position.
    if (record.schema()->nameAt(i) != name) {
      Report(DivergenceId::kRecordFieldNames, here,
             absl::StrCat("field ", i, " is named '", name,
                          "' in the bridge and '", record.schema()->nameAt(i),
                          "' in avro-cpp"));
      continue;
    }
    auto field = bridge.GetRecordField(name);
    if (!field.ok()) {
      Report(DivergenceId::kRecordFieldNames, here,
             absl::StrCat("the bridge has no field '", name, "'"));
      continue;
    }
    Compare(*field, record.fieldAt(i), here);
  }
}

void Comparer::CompareArray(const AvroValue& bridge,
                            const ::avro::GenericDatum& cpp,
                            absl::string_view path) {
  const auto& items = cpp.value<::avro::GenericArray>().value();
  auto length = bridge.GetArrayLen();
  if (!length.ok()) {
    Report(DivergenceId::kValueTypeMismatch, path,
           "the bridge would not report an array length");
    return;
  }
  if (*length != items.size()) {
    Report(DivergenceId::kArrayLen, path,
           absl::StrCat("length differs: bridge ", *length, ", avro-cpp ",
                        items.size()));
    return;
  }
  for (size_t i = 0; i < items.size(); ++i) {
    const std::string here = absl::StrCat(path, "[", i, "]");
    auto item = bridge.GetArrayItem(i);
    if (!item.ok()) {
      Report(DivergenceId::kArrayLen, here,
             "the bridge would not read this element");
      continue;
    }
    Compare(*item, items[i], here);
  }
}

void Comparer::CompareMap(const AvroValue& bridge,
                          const ::avro::GenericDatum& cpp,
                          absl::string_view path) {
  const auto& entries = cpp.value<::avro::GenericMap>().value();

  // avro-cpp keeps duplicate keys; the bridge's HashMap collapses them. Detect
  // that here rather than letting it surface as a confusing size mismatch.
  std::vector<std::string> cpp_keys;
  cpp_keys.reserve(entries.size());
  for (const auto& entry : entries) cpp_keys.push_back(entry.first);
  std::vector<std::string> unique_keys = cpp_keys;
  std::sort(unique_keys.begin(), unique_keys.end());
  const size_t distinct =
      std::unique(unique_keys.begin(), unique_keys.end()) - unique_keys.begin();
  unique_keys.resize(distinct);

  auto bridge_keys = bridge.GetMapKeys();
  if (!bridge_keys.ok()) {
    Report(DivergenceId::kValueTypeMismatch, path,
           "the bridge would not enumerate map keys");
    return;
  }

  if (distinct != cpp_keys.size()) {
    Report(DivergenceId::kD2DuplicateMapKey, path,
           absl::StrCat("avro-cpp holds ", cpp_keys.size(),
                        " entries with only ", distinct,
                        " distinct keys; the bridge holds ",
                        bridge_keys->size(), ", so a duplicate key lost data"),
           "duplicate map key");
    return;
  }

  // Wire order is never compared: the bridge returns keys sorted, and Rust's
  // HashMap iteration order makes avro-cpp's order vary between runs (D3).
  if (bridge_keys->size() != unique_keys.size()) {
    Report(DivergenceId::kMapArity, path,
           absl::StrCat("entry count differs: bridge ", bridge_keys->size(),
                        ", avro-cpp ", unique_keys.size()));
    return;
  }
  if (*bridge_keys != unique_keys) {
    Report(DivergenceId::kMapKeySet, path, "key sets differ");
    return;
  }
  for (const auto& entry : entries) {
    const std::string here =
        absl::StrCat(path, "{\"", absl::BytesToHexString(entry.first), "\"}");
    auto value = bridge.GetMapValue(entry.first);
    if (!value.ok()) {
      Report(DivergenceId::kMapKeySet, here, "the bridge has no such key");
      continue;
    }
    Compare(*value, entry.second, here);
  }
}

void Comparer::Compare(const AvroValue& bridge_in,
                       const ::avro::GenericDatum& cpp,
                       absl::string_view path) {
  // Unions: avro-cpp's type(), logicalType() and value<T>() are all
  // transparent through a union, forwarding to the selected branch. Only
  // isUnion() and unionBranch() are not. So compare the branch index, then
  // advance the *bridge* side into its branch while keeping the *same* datum,
  // and re-dispatch. Avro forbids nested unions, so this runs at most once --
  // a loop, not recursion.
  const AvroValue* bridge = &bridge_in;
  AvroValue branch_storage = bridge_in;
  std::string here(path);

  if (bridge->IsUnion()) {
    if (!cpp.isUnion()) {
      Report(DivergenceId::kValueTypeMismatch, here,
             "the bridge holds a union, avro-cpp does not");
      return;
    }
    auto bridge_branch = bridge->GetUnionBranch();
    if (!bridge_branch.ok()) {
      Report(DivergenceId::kUnionBranch, here,
             "the bridge would not report a union branch");
      return;
    }
    if (*bridge_branch != cpp.unionBranch()) {
      Report(DivergenceId::kUnionBranch, here,
             absl::StrCat("branch differs: bridge ", *bridge_branch,
                          ", avro-cpp ", cpp.unionBranch()));
      return;
    }
    auto inner = bridge->GetUnionValue();
    if (!inner.ok()) {
      Report(DivergenceId::kUnionBranch, here,
             "the bridge would not read its union branch");
      return;
    }
    branch_storage = *inner;
    bridge = &branch_storage;
    absl::StrAppend(&here, "<branch ", *bridge_branch, ">");
  } else if (cpp.isUnion()) {
    Report(DivergenceId::kValueTypeMismatch, here,
           "avro-cpp holds a union, the bridge does not");
    return;
  }

  const ::avro::Type cpp_type = cpp.type();
  const ::avro::LogicalType::Type cpp_logical = cpp.logicalType().type();

  switch (cpp_type) {
    case ::avro::AVRO_NULL:
      if (!bridge->IsNull()) {
        Report(DivergenceId::kValueTypeMismatch, here,
               absl::StrCat("avro-cpp holds null, the bridge holds ",
                            bridge->TypeName()));
      }
      return;

    case ::avro::AVRO_BOOL: {
      auto value = bridge->GetBoolean();
      if (!value.ok()) {
        Report(DivergenceId::kValueTypeMismatch, here,
               absl::StrCat("avro-cpp holds boolean, the bridge holds ",
                            bridge->TypeName()));
      } else if (*value != cpp.value<bool>()) {
        Report(DivergenceId::kScalarValue, here, "boolean values differ");
      }
      return;
    }

    case ::avro::AVRO_INT: {
      const int32_t theirs = cpp.value<int32_t>();
      // date and time-millis ride on int; the bridge exposes them through
      // their own accessors.
      auto ours = bridge->IsDate()         ? bridge->GetDate()
                  : bridge->IsTimeMillis() ? bridge->GetTimeMillis()
                                           : bridge->GetInt();
      if (!ours.ok()) {
        Report(DivergenceId::kValueTypeMismatch, here,
               absl::StrCat("avro-cpp holds int, the bridge holds ",
                            bridge->TypeName()));
      } else if (*ours != theirs) {
        Report(DivergenceId::kScalarValue, here,
               absl::StrCat("int values differ: bridge ", *ours, ", avro-cpp ",
                            theirs));
      }
      return;
    }

    case ::avro::AVRO_LONG: {
      const int64_t theirs = cpp.value<int64_t>();
      absl::StatusOr<int64_t> ours = bridge->GetLong();
      if (bridge->IsTimeMicros()) ours = bridge->GetTimeMicros();
      else if (bridge->IsTimestampMillis()) ours = bridge->GetTimestampMillis();
      else if (bridge->IsTimestampMicros()) ours = bridge->GetTimestampMicros();
      else if (bridge->IsTimestampNanos()) ours = bridge->GetTimestampNanos();
      else if (bridge->IsLocalTimestampMillis())
        ours = bridge->GetLocalTimestampMillis();
      else if (bridge->IsLocalTimestampMicros())
        ours = bridge->GetLocalTimestampMicros();
      else if (bridge->IsLocalTimestampNanos())
        ours = bridge->GetLocalTimestampNanos();

      const bool cpp_dropped_annotation =
          cpp_logical == ::avro::LogicalType::NONE &&
          (bridge->IsTimestampNanos() || bridge->IsLocalTimestampMillis() ||
           bridge->IsLocalTimestampMicros() || bridge->IsLocalTimestampNanos());
      if (cpp_dropped_annotation && !options.allow_missing_logical_types) {
        Report(DivergenceId::kLogicalTypeAbsentIn1114, here,
               absl::StrCat("the bridge kept the logical type ",
                            bridge->TypeName(),
                            " but avro-cpp 1.11.4 has no such LogicalType and "
                            "fell back to a plain long"),
               "logical type absent");
      }
      if (!ours.ok()) {
        Report(DivergenceId::kValueTypeMismatch, here,
               absl::StrCat("avro-cpp holds long, the bridge holds ",
                            bridge->TypeName()));
      } else if (*ours != theirs) {
        Report(DivergenceId::kScalarValue, here,
               absl::StrCat("long values differ: bridge ", *ours, ", avro-cpp ",
                            theirs));
      }
      return;
    }

    case ::avro::AVRO_FLOAT: {
      auto ours = bridge->GetFloat();
      const float theirs = cpp.value<float>();
      if (!ours.ok()) {
        Report(DivergenceId::kValueTypeMismatch, here,
               absl::StrCat("avro-cpp holds float, the bridge holds ",
                            bridge->TypeName()));
        return;
      }
      if (options.strict_float_bits) {
        if (!SameBits<float, uint32_t>(*ours, theirs)) {
          const bool both_nan = std::isnan(*ours) && std::isnan(theirs);
          Report(both_nan ? DivergenceId::kFloatNanPayload
                          : (*ours == theirs ? DivergenceId::kFloatSignedZero
                                             : DivergenceId::kScalarValue),
                 here, "float bits differ");
        }
      } else if (*ours != theirs && !(std::isnan(*ours) && std::isnan(theirs))) {
        Report(DivergenceId::kScalarValue, here, "float values differ");
      }
      return;
    }

    case ::avro::AVRO_DOUBLE: {
      auto ours = bridge->GetDouble();
      const double theirs = cpp.value<double>();
      if (!ours.ok()) {
        Report(DivergenceId::kValueTypeMismatch, here,
               absl::StrCat("avro-cpp holds double, the bridge holds ",
                            bridge->TypeName()));
        return;
      }
      if (options.strict_float_bits) {
        if (!SameBits<double, uint64_t>(*ours, theirs)) {
          const bool both_nan = std::isnan(*ours) && std::isnan(theirs);
          Report(both_nan ? DivergenceId::kFloatNanPayload
                          : (*ours == theirs ? DivergenceId::kFloatSignedZero
                                             : DivergenceId::kScalarValue),
                 here, "double bits differ");
        }
      } else if (*ours != theirs && !(std::isnan(*ours) && std::isnan(theirs))) {
        Report(DivergenceId::kScalarValue, here, "double values differ");
      }
      return;
    }

    case ::avro::AVRO_STRING: {
      const std::string& theirs = cpp.value<std::string>();
      if (bridge->IsUuid() || cpp_logical == ::avro::LogicalType::UUID) {
        const std::string ours = TextOf(*bridge);
        if (CanonicalUuid(ours) != CanonicalUuid(theirs)) {
          Report(DivergenceId::kScalarValue, here,
                 absl::StrCat("uuid values differ: bridge '", ours,
                              "', avro-cpp '", theirs, "'"));
        } else if (ours != theirs) {
          // avro-cpp preserves the text as written; the bridge re-emits the
          // lowercase canonical form.
          Report(DivergenceId::kUuidTextNotPreserved, here,
                 absl::StrCat("uuid canonicalises the same but the text "
                              "differs: bridge '",
                              ours, "', avro-cpp '", theirs, "'"),
                 "uuid text");
        }
        return;
      }
      if (!bridge->IsString()) {
        // Strict on purpose. Treating String and Bytes as interchangeable is
        // exactly how D1 would be hidden.
        Report(DivergenceId::kStringBytesTypeMismatch, here,
               absl::StrCat("avro-cpp holds a string of ", theirs.size(),
                            " bytes (", absl::BytesToHexString(theirs),
                            ") but the bridge holds ", bridge->TypeName()),
               "string vs bytes");
        return;
      }
      const std::string ours = TextOf(*bridge);
      if (ours != theirs) {
        Report(DivergenceId::kScalarValue, here,
               absl::StrCat("string payloads differ: bridge ",
                            absl::BytesToHexString(ours), ", avro-cpp ",
                            absl::BytesToHexString(theirs)));
      }
      return;
    }

    case ::avro::AVRO_BYTES: {
      const auto& theirs = cpp.value<std::vector<uint8_t>>();
      const std::string theirs_text(theirs.begin(), theirs.end());
      if (cpp_logical == ::avro::LogicalType::DECIMAL || bridge->IsDecimal()) {
        const std::string ours = TextOf(*bridge);
        if (MinimalTwosComplement(ours) != MinimalTwosComplement(theirs_text)) {
          Report(DivergenceId::kDecimalValue, here,
                 absl::StrCat("decimal values differ: bridge ",
                              absl::BytesToHexString(ours), ", avro-cpp ",
                              absl::BytesToHexString(theirs_text)));
        } else if (ours != theirs_text) {
          Report(DivergenceId::kDecimalSignPadding, here,
                 absl::StrCat("decimals are numerically equal but padded "
                              "differently: bridge ",
                              absl::BytesToHexString(ours), ", avro-cpp ",
                              absl::BytesToHexString(theirs_text)),
                 "decimal padding");
        }
        return;
      }
      if (!bridge->IsBytes()) {
        Report(DivergenceId::kStringBytesTypeMismatch, here,
               absl::StrCat("avro-cpp holds bytes but the bridge holds ",
                            bridge->TypeName()),
               "string vs bytes");
        return;
      }
      const std::string ours = TextOf(*bridge);
      if (ours != theirs_text) {
        Report(DivergenceId::kScalarValue, here,
               absl::StrCat("bytes differ: bridge ",
                            absl::BytesToHexString(ours), ", avro-cpp ",
                            absl::BytesToHexString(theirs_text)));
      }
      return;
    }

    case ::avro::AVRO_FIXED: {
      const auto& theirs = cpp.value<::avro::GenericFixed>().value();
      const std::string theirs_text(theirs.begin(), theirs.end());
      const std::string ours = TextOf(*bridge);

      if (bridge->IsDuration() || cpp_logical == ::avro::LogicalType::DURATION) {
        if (theirs.size() != 12) {
          Report(DivergenceId::kDurationFields, here,
                 "duration is not 12 bytes on the avro-cpp side");
          return;
        }
        const auto le32 = [&theirs](int part) {
          return static_cast<uint32_t>(theirs[part * 4]) |
                 (static_cast<uint32_t>(theirs[part * 4 + 1]) << 8) |
                 (static_cast<uint32_t>(theirs[part * 4 + 2]) << 16) |
                 (static_cast<uint32_t>(theirs[part * 4 + 3]) << 24);
        };
        auto months = bridge->GetDurationMonths();
        auto days = bridge->GetDurationDays();
        auto millis = bridge->GetDurationMillis();
        if (!months.ok() || !days.ok() || !millis.ok()) {
          Report(DivergenceId::kValueTypeMismatch, here,
                 absl::StrCat("avro-cpp holds a duration, the bridge holds ",
                              bridge->TypeName()));
          return;
        }
        if (*months != le32(0) || *days != le32(1) || *millis != le32(2)) {
          Report(DivergenceId::kDurationFields, here,
                 absl::StrCat("duration fields differ: bridge (", *months, ",",
                              *days, ",", *millis, "), avro-cpp (", le32(0),
                              ",", le32(1), ",", le32(2), ")"));
        }
        return;
      }

      if (cpp_logical == ::avro::LogicalType::DECIMAL || bridge->IsDecimal()) {
        if (MinimalTwosComplement(ours) != MinimalTwosComplement(theirs_text)) {
          Report(DivergenceId::kDecimalValue, here,
                 absl::StrCat("fixed-backed decimal values differ: bridge ",
                              absl::BytesToHexString(ours), ", avro-cpp ",
                              absl::BytesToHexString(theirs_text)));
        } else if (ours.size() != theirs_text.size()) {
          Report(DivergenceId::kDecimalSignPadding, here,
                 absl::StrCat("fixed-backed decimal lengths differ: bridge ",
                              ours.size(), ", avro-cpp ", theirs_text.size()),
                 "decimal padding");
        }
        return;
      }

      if (!bridge->IsFixed()) {
        Report(DivergenceId::kValueTypeMismatch, here,
               absl::StrCat("avro-cpp holds fixed, the bridge holds ",
                            bridge->TypeName()));
        return;
      }
      if (ours != theirs_text) {
        Report(DivergenceId::kScalarValue, here,
               absl::StrCat("fixed payloads differ: bridge ",
                            absl::BytesToHexString(ours), ", avro-cpp ",
                            absl::BytesToHexString(theirs_text)));
      }
      return;
    }

    case ::avro::AVRO_ENUM: {
      const auto& theirs = cpp.value<::avro::GenericEnum>();
      auto position = bridge->GetEnumPosition();
      auto symbol = bridge->GetEnumSymbol();
      if (!position.ok() || !symbol.ok()) {
        Report(DivergenceId::kValueTypeMismatch, here,
               absl::StrCat("avro-cpp holds an enum, the bridge holds ",
                            bridge->TypeName()));
        return;
      }
      // Compare both: a position/symbol disagreement is precisely the D6 shape.
      if (*position != theirs.value()) {
        Report(DivergenceId::kEnumPosition, here,
               absl::StrCat("enum positions differ: bridge ", *position,
                            ", avro-cpp ", theirs.value()));
        return;
      }
      if (*symbol != theirs.symbol()) {
        Report(DivergenceId::kEnumSymbol, here,
               absl::StrCat("enum symbols differ at position ", *position,
                            ": bridge '", *symbol, "', avro-cpp '",
                            theirs.symbol(), "'"));
      }
      return;
    }

    case ::avro::AVRO_RECORD:
      if (!bridge->IsRecord()) {
        Report(DivergenceId::kValueTypeMismatch, here,
               absl::StrCat("avro-cpp holds a record, the bridge holds ",
                            bridge->TypeName()));
        return;
      }
      CompareRecord(*bridge, cpp, here);
      return;

    case ::avro::AVRO_ARRAY:
      if (!bridge->IsArray()) {
        Report(DivergenceId::kValueTypeMismatch, here,
               absl::StrCat("avro-cpp holds an array, the bridge holds ",
                            bridge->TypeName()));
        return;
      }
      CompareArray(*bridge, cpp, here);
      return;

    case ::avro::AVRO_MAP:
      if (!bridge->IsMap()) {
        Report(DivergenceId::kValueTypeMismatch, here,
               absl::StrCat("avro-cpp holds a map, the bridge holds ",
                            bridge->TypeName()));
        return;
      }
      CompareMap(*bridge, cpp, here);
      return;

    default:
      Report(DivergenceId::kValueTypeMismatch, here,
             absl::StrCat("unhandled avro-cpp type ",
                          static_cast<int>(cpp_type)));
      return;
  }
}

}  // namespace

bool CompareValues(const security::avro::AvroValue& bridge,
                   const ::avro::GenericDatum& cpp, FindingLog* log,
                   const CompareOptions& options) {
  Comparer comparer{log, options};
  comparer.Compare(bridge, cpp, "$");
  return comparer.clean;
}

}  // namespace security::avro_fuzz
