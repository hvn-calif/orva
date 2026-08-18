#include "fuzz/lower_bridge.h"

#include <cstdint>
#include <set>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "avro_bridge.h"
#include "fuzz/ir.h"
#include "fuzz/suppress.h"

namespace security::avro_fuzz {
namespace {

using ::security::avro::AvroValue;

absl::StatusOr<AvroValue> Lower(const Node& node, absl::string_view path,
                                FindingLog* log);

absl::StatusOr<AvroValue> LowerRecord(const Node& node, absl::string_view path,
                                      FindingLog* log) {
  AvroValue record = AvroValue::CreateRecord();
  for (size_t i = 0; i < node.children.size(); ++i) {
    const std::string name = LabelAt(node, i);
    auto field =
        Lower(node.children[i], absl::StrCat(path, ".", name), log);
    if (!field.ok()) return field.status();
    if (absl::Status status = record.RecordPut(name, *field); !status.ok()) {
      return status;
    }
  }
  return record;
}

absl::StatusOr<AvroValue> LowerArray(const Node& node, absl::string_view path,
                                     FindingLog* log) {
  AvroValue array = AvroValue::CreateArray();
  // The schema takes its item type from children[0]; every element must
  // therefore inhabit that same type, so all elements are built from it.
  if (node.children.empty()) return array;
  for (size_t i = 0; i < node.children.size(); ++i) {
    auto item =
        Lower(node.children[0], absl::StrCat(path, "[", i, "]"), log);
    if (!item.ok()) return item.status();
    if (absl::Status status = array.ArrayPush(*item); !status.ok()) {
      return status;
    }
  }
  return array;
}

absl::StatusOr<AvroValue> LowerMap(const Node& node, absl::string_view path,
                                   FindingLog* log) {
  AvroValue map = AvroValue::CreateMap();
  if (node.children.empty()) return map;

  std::set<std::string> seen;
  for (size_t i = 0; i < node.children.size(); ++i) {
    const std::string key = KeyAt(node, i);
    const std::string here =
        absl::StrCat(path, "{\"", absl::BytesToHexString(key), "\"}");

    if (!seen.insert(key).second) {
      // D2. avro-cpp's GenericMap is a vector of pairs and keeps both entries;
      // the bridge's HashMap collapses them and silently loses one.
      log->Report(DivergenceId::kD2DuplicateMapKey, here,
                  absl::StrCat("duplicate map key ",
                               absl::BytesToHexString(key),
                               ": the bridge collapses it (last write wins), "
                               "avro-cpp keeps both entries"),
                  "duplicate map key");
      return absl::FailedPreconditionError("D2");
    }
    auto value = Lower(node.children[0], here, log);
    if (!value.ok()) return value.status();
    if (absl::Status status = map.MapPut(key, *value); !status.ok()) {
      return status;
    }
  }
  return map;
}

absl::StatusOr<AvroValue> LowerUnion(const Node& node, absl::string_view path,
                                     FindingLog* log) {
  if (node.children.empty()) {
    return absl::FailedPreconditionError("union with no branches");
  }
  const uint32_t branch = ResolveIndex(node.selectors, node.children.size());
  if (branch >= node.children.size()) {
    return absl::OutOfRangeError("union branch out of range");
  }
  auto value = Lower(node.children[branch],
                     absl::StrCat(path, "<branch ", branch, ">"), log);
  if (!value.ok()) return value.status();
  return AvroValue::CreateUnion(branch, *value);
}

absl::StatusOr<AvroValue> Lower(const Node& node, absl::string_view path,
                                FindingLog* log) {
  const int64_t integer = node.scalars.integer;
  const auto narrow = [integer] { return static_cast<int32_t>(integer); };

  switch (node.kind) {
    case Kind::kNull: return AvroValue::CreateNull();
    case Kind::kBoolean: return AvroValue::CreateBoolean(node.scalars.boolean);
    case Kind::kInt: return AvroValue::CreateInt(narrow());
    case Kind::kLong: return AvroValue::CreateLong(integer);
    case Kind::kFloat: return AvroValue::CreateFloat(node.scalars.f32);
    case Kind::kDouble: return AvroValue::CreateDouble(node.scalars.f64);
    case Kind::kBytes: return AvroValue::CreateBytes(node.scalars.blob);

    case Kind::kString: {
      auto value = AvroValue::CreateString(node.scalars.blob);
      if (value.ok()) return value;
      // D1. avro-cpp stores whatever bytes it is given in a std::string; the
      // bridge validates UTF-8 and refuses. This is the write side of the
      // divergence -- the read side surfaces when avro-cpp encodes such a
      // string and the bridge decodes it.
      log->Report(DivergenceId::kD1StringNotUtf8, path,
                  absl::StrCat("the bridge rejected string bytes ",
                               absl::BytesToHexString(node.scalars.blob), " (",
                               value.status().message(),
                               "); avro-cpp stores them verbatim"),
                  "non-utf8 string");
      return value.status();
    }

    case Kind::kUuid: {
      auto value = AvroValue::CreateUuid(node.scalars.blob);
      if (value.ok()) return value;
      log->Report(DivergenceId::kUuidInvalidRejected, path,
                  absl::StrCat("the bridge rejected uuid text ",
                               absl::BytesToHexString(node.scalars.blob),
                               "; avro-cpp keeps the string as written"),
                  "invalid uuid");
      return value.status();
    }

    case Kind::kRecord: return LowerRecord(node, path, log);
    case Kind::kArray: return LowerArray(node, path, log);
    case Kind::kMap: return LowerMap(node, path, log);
    case Kind::kUnion: return LowerUnion(node, path, log);

    case Kind::kEnum: {
      if (node.labels.empty()) {
        return absl::FailedPreconditionError("enum with no symbols");
      }
      const uint32_t position = ResolveIndex(node.selectors, node.labels.size());
      if (position >= node.labels.size()) {
        return absl::OutOfRangeError("enum position out of range");
      }
      return AvroValue::CreateEnum(position, node.labels[position]);
    }

    case Kind::kFixed: return AvroValue::CreateFixed(node.scalars.blob);

    case Kind::kDecimalBytes:
    case Kind::kDecimalFixed:
      return AvroValue::CreateDecimal(node.scalars.blob);

    case Kind::kDate: return AvroValue::CreateDate(narrow());
    case Kind::kTimeMillis: return AvroValue::CreateTimeMillis(narrow());
    case Kind::kTimeMicros: return AvroValue::CreateTimeMicros(integer);
    case Kind::kTimestampMillis:
      return AvroValue::CreateTimestampMillis(integer);
    case Kind::kTimestampMicros:
      return AvroValue::CreateTimestampMicros(integer);
    case Kind::kTimestampNanos:
      return AvroValue::CreateTimestampNanos(integer);
    case Kind::kLocalTimestampMillis:
      return AvroValue::CreateLocalTimestampMillis(integer);
    case Kind::kLocalTimestampMicros:
      return AvroValue::CreateLocalTimestampMicros(integer);
    case Kind::kLocalTimestampNanos:
      return AvroValue::CreateLocalTimestampNanos(integer);

    case Kind::kDuration:
      return AvroValue::CreateDuration(node.scalars.months, node.scalars.days,
                                       node.scalars.millis);

    case Kind::kMaxKind:
      break;
  }
  return absl::InternalError("unhandled Kind in ToBridgeValue");
}

}  // namespace

absl::StatusOr<security::avro::AvroValue> ToBridgeValue(const Node& node,
                                                        FindingLog* log) {
  return Lower(node, "$", log);
}

}  // namespace security::avro_fuzz
