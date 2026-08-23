#include "fuzz/dump.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "avro/GenericDatum.hh"
#include "avro/LogicalType.hh"
#include "avro/Types.hh"
#include "avro_bridge.h"

namespace security::avro_fuzz {
namespace {

using ::security::avro::AvroValue;

std::string Text(absl::string_view tag, absl::string_view payload) {
  return absl::StrCat(tag, ":", absl::BytesToHexString(payload));
}

template <typename T>
std::string Number(absl::string_view tag, const absl::StatusOr<T>& value) {
  if (!value.ok()) return absl::StrCat(tag, ":<unreadable>");
  return absl::StrCat(tag, ":", *value);
}

std::string Bytes(absl::string_view tag,
                  const absl::StatusOr<std::string>& value) {
  if (!value.ok()) return absl::StrCat(tag, ":<unreadable>");
  return Text(tag, *value);
}

std::string FloatBits(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  return absl::StrFormat("f32:%08x", bits);
}

std::string DoubleBits(double value) {
  uint64_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  return absl::StrFormat("f64:%016x", bits);
}

std::string Duration(uint32_t months, uint32_t days, uint32_t millis) {
  return absl::StrCat("dur:", months, ",", days, ",", millis);
}

// Reads the three little-endian 32-bit fields Avro packs into a 12-byte
// duration, so the avro-cpp side prints what the bridge's accessors return.
std::string DurationFromFixed(const std::vector<uint8_t>& raw) {
  if (raw.size() != 12) return Text("dur-not-12-bytes", std::string(raw.begin(), raw.end()));
  const auto field = [&raw](int index) {
    return static_cast<uint32_t>(raw[index * 4]) |
           (static_cast<uint32_t>(raw[index * 4 + 1]) << 8) |
           (static_cast<uint32_t>(raw[index * 4 + 2]) << 16) |
           (static_cast<uint32_t>(raw[index * 4 + 3]) << 24);
  };
  return Duration(field(0), field(1), field(2));
}

struct BridgeDumper {
  DumpOptions options;

  absl::string_view StringTag() const {
    return options.string_as_bytes ? "text" : "str";
  }
  absl::string_view BytesTag() const {
    return options.string_as_bytes ? "text" : "bytes";
  }

  std::string Dump(const AvroValue& value, int depth) const;
  std::string DumpUnion(const AvroValue& value, int depth) const;
  std::string DumpRecord(const AvroValue& value, int depth) const;
  std::string DumpArray(const AvroValue& value, int depth) const;
  std::string DumpMap(const AvroValue& value, int depth) const;
};

std::string BridgeDumper::DumpUnion(const AvroValue& value, int depth) const {
  auto branch = value.GetUnionBranch();
  auto inner = value.GetUnionValue();
  if (!branch.ok() || !inner.ok()) return "union:<unreadable>";
  return absl::StrCat("union", *branch, "(", Dump(*inner, depth + 1), ")");
}

std::string BridgeDumper::DumpRecord(const AvroValue& value, int depth) const {
  auto names = value.GetRecordFieldNames();
  if (!names.ok()) return "rec:<unreadable>";
  std::string out = "rec{";
  for (size_t i = 0; i < names->size(); ++i) {
    if (i != 0) out.push_back(',');
    const std::string& name = (*names)[i];
    auto field = value.GetRecordField(name);
    absl::StrAppend(&out, name, "=",
                    field.ok() ? Dump(*field, depth + 1)
                               : std::string("<unreadable>"));
  }
  out.push_back('}');
  return out;
}

std::string BridgeDumper::DumpArray(const AvroValue& value, int depth) const {
  auto length = value.GetArrayLen();
  if (!length.ok()) return "arr:<unreadable>";
  std::string out = "arr[";
  for (size_t i = 0; i < *length; ++i) {
    if (i != 0) out.push_back(',');
    auto item = value.GetArrayItem(i);
    absl::StrAppend(&out, item.ok() ? Dump(*item, depth + 1)
                                    : std::string("<unreadable>"));
  }
  out.push_back(']');
  return out;
}

std::string BridgeDumper::DumpMap(const AvroValue& value, int depth) const {
  auto keys = value.GetMapKeys();
  if (!keys.ok()) return "map:<unreadable>";
  std::string out = "map{";
  for (size_t i = 0; i < keys->size(); ++i) {
    if (i != 0) out.push_back(',');
    const std::string& key = (*keys)[i];
    auto entry = value.GetMapValue(key);
    absl::StrAppend(&out, absl::BytesToHexString(key), "=",
                    entry.ok() ? Dump(*entry, depth + 1)
                               : std::string("<unreadable>"));
  }
  out.push_back('}');
  return out;
}

std::string BridgeDumper::Dump(const AvroValue& value, int depth) const {
  if (depth >= kMaxDumpDepth) return "<depth>";
  if (value.IsUnion()) return DumpUnion(value, depth);

  // Logical types first: a value that carries one answers both its own
  // predicate and, for some of them, the predicate of the type underneath.
  if (value.IsDecimal()) return Bytes("dec", value.GetDecimalBytes());
  if (value.IsUuid()) return Bytes(StringTag(), value.GetUuid());
  if (value.IsDate()) return Number("int", value.GetDate());
  if (value.IsTimeMillis()) return Number("int", value.GetTimeMillis());
  if (value.IsTimeMicros()) return Number("long", value.GetTimeMicros());
  if (value.IsTimestampMillis()) return Number("long", value.GetTimestampMillis());
  if (value.IsTimestampMicros()) return Number("long", value.GetTimestampMicros());
  if (value.IsTimestampNanos()) return Number("long", value.GetTimestampNanos());
  if (value.IsLocalTimestampMillis())
    return Number("long", value.GetLocalTimestampMillis());
  if (value.IsLocalTimestampMicros())
    return Number("long", value.GetLocalTimestampMicros());
  if (value.IsLocalTimestampNanos())
    return Number("long", value.GetLocalTimestampNanos());
  if (value.IsDuration()) {
    auto months = value.GetDurationMonths();
    auto days = value.GetDurationDays();
    auto millis = value.GetDurationMillis();
    if (!months.ok() || !days.ok() || !millis.ok()) return "dur:<unreadable>";
    return Duration(*months, *days, *millis);
  }

  if (value.IsNull()) return "null";
  if (value.IsBoolean()) return Number("bool", value.GetBoolean());
  if (value.IsInt()) return Number("int", value.GetInt());
  if (value.IsLong()) return Number("long", value.GetLong());
  if (value.IsFloat()) {
    auto number = value.GetFloat();
    return number.ok() ? FloatBits(*number) : std::string("f32:<unreadable>");
  }
  if (value.IsDouble()) {
    auto number = value.GetDouble();
    return number.ok() ? DoubleBits(*number) : std::string("f64:<unreadable>");
  }
  if (value.IsString()) return Bytes(StringTag(), value.GetString());
  if (value.IsBytes()) return Bytes(BytesTag(), value.GetBytes());
  if (value.IsFixed()) return Bytes("fixed", value.GetFixedBytes());
  if (value.IsEnum()) {
    auto position = value.GetEnumPosition();
    auto symbol = value.GetEnumSymbol();
    if (!position.ok() || !symbol.ok()) return "enum:<unreadable>";
    return absl::StrCat("enum:", *position, ":",
                        absl::BytesToHexString(*symbol));
  }
  if (value.IsRecord()) return DumpRecord(value, depth);
  if (value.IsArray()) return DumpArray(value, depth);
  if (value.IsMap()) return DumpMap(value, depth);
  return absl::StrCat("<unhandled:", value.TypeName(), ">");
}

// `inside_union` is what stops the union from being printed twice: avro-cpp's
// GenericDatum is transparent through a union, so type() and value<T>() already
// answer for the selected branch and there is no separate datum to descend to.
struct CppDumper {
  DumpOptions options;

  absl::string_view StringTag() const {
    return options.string_as_bytes ? "text" : "str";
  }
  absl::string_view BytesTag() const {
    return options.string_as_bytes ? "text" : "bytes";
  }

  std::string Dump(const ::avro::GenericDatum& datum, int depth,
                   bool inside_union) const;
  std::string DumpRecord(const ::avro::GenericDatum& datum, int depth) const;
  std::string DumpArray(const ::avro::GenericDatum& datum, int depth) const;
  std::string DumpMap(const ::avro::GenericDatum& datum, int depth) const;
};

std::string CppDumper::DumpRecord(const ::avro::GenericDatum& datum,
                                  int depth) const {
  const auto& record = datum.value<::avro::GenericRecord>();
  std::string out = "rec{";
  for (size_t i = 0; i < record.fieldCount(); ++i) {
    if (i != 0) out.push_back(',');
    absl::StrAppend(&out, record.schema()->nameAt(i), "=",
                    Dump(record.fieldAt(i), depth + 1, false));
  }
  out.push_back('}');
  return out;
}

std::string CppDumper::DumpArray(const ::avro::GenericDatum& datum,
                                 int depth) const {
  const auto& items = datum.value<::avro::GenericArray>().value();
  std::string out = "arr[";
  for (size_t i = 0; i < items.size(); ++i) {
    if (i != 0) out.push_back(',');
    absl::StrAppend(&out, Dump(items[i], depth + 1, false));
  }
  out.push_back(']');
  return out;
}

std::string CppDumper::DumpMap(const ::avro::GenericDatum& datum,
                               int depth) const {
  const auto& entries = datum.value<::avro::GenericMap>().value();
  std::vector<std::pair<std::string, std::string>> rendered;
  rendered.reserve(entries.size());
  for (const auto& entry : entries) {
    rendered.emplace_back(absl::BytesToHexString(entry.first),
                          Dump(entry.second, depth + 1, false));
  }
  // Sorted by rendered key and then by rendered value, so the duplicate keys
  // avro-cpp keeps and the bridge collapses stay visible as two entries rather
  // than being merged here.
  //
  // Sorting the hex renderings rather than the raw keys still matches the order
  // the bridge returns its keys in, which is by raw byte: hex is fixed-width per
  // byte and its digits ascend in nibble order, so it preserves the ordering.
  std::sort(rendered.begin(), rendered.end());
  std::string out = "map{";
  for (size_t i = 0; i < rendered.size(); ++i) {
    if (i != 0) out.push_back(',');
    absl::StrAppend(&out, rendered[i].first, "=", rendered[i].second);
  }
  out.push_back('}');
  return out;
}

std::string CppDumper::Dump(const ::avro::GenericDatum& datum, int depth,
                            bool inside_union) const {
  if (depth >= kMaxDumpDepth) return "<depth>";
  if (datum.isUnion() && !inside_union) {
    return absl::StrCat("union", datum.unionBranch(), "(",
                        Dump(datum, depth + 1, true), ")");
  }

  const ::avro::LogicalType::Type logical = datum.logicalType().type();
  if (logical == ::avro::LogicalType::DECIMAL) {
    if (datum.type() == ::avro::AVRO_BYTES) {
      const auto& raw = datum.value<std::vector<uint8_t>>();
      return Text("dec", std::string(raw.begin(), raw.end()));
    }
    if (datum.type() == ::avro::AVRO_FIXED) {
      const auto& raw = datum.value<::avro::GenericFixed>().value();
      return Text("dec", std::string(raw.begin(), raw.end()));
    }
  }
  if (logical == ::avro::LogicalType::DURATION &&
      datum.type() == ::avro::AVRO_FIXED) {
    return DurationFromFixed(datum.value<::avro::GenericFixed>().value());
  }

  switch (datum.type()) {
    case ::avro::AVRO_NULL:
      return "null";
    case ::avro::AVRO_BOOL:
      return absl::StrCat("bool:", datum.value<bool>());
    case ::avro::AVRO_INT:
      return absl::StrCat("int:", datum.value<int32_t>());
    case ::avro::AVRO_LONG:
      return absl::StrCat("long:", datum.value<int64_t>());
    case ::avro::AVRO_FLOAT:
      return FloatBits(datum.value<float>());
    case ::avro::AVRO_DOUBLE:
      return DoubleBits(datum.value<double>());
    case ::avro::AVRO_STRING:
      return Text(StringTag(), datum.value<std::string>());
    case ::avro::AVRO_BYTES: {
      const auto& raw = datum.value<std::vector<uint8_t>>();
      return Text(BytesTag(), std::string(raw.begin(), raw.end()));
    }
    case ::avro::AVRO_FIXED: {
      const auto& raw = datum.value<::avro::GenericFixed>().value();
      return Text("fixed", std::string(raw.begin(), raw.end()));
    }
    case ::avro::AVRO_ENUM: {
      const auto& value = datum.value<::avro::GenericEnum>();
      return absl::StrCat("enum:", value.value(), ":",
                          absl::BytesToHexString(value.symbol()));
    }
    case ::avro::AVRO_RECORD:
      return DumpRecord(datum, depth);
    case ::avro::AVRO_ARRAY:
      return DumpArray(datum, depth);
    case ::avro::AVRO_MAP:
      return DumpMap(datum, depth);
    default:
      return absl::StrCat("<unhandled:", static_cast<int>(datum.type()), ">");
  }
}

}  // namespace

std::string DumpBridgeValue(const AvroValue& value,
                            const DumpOptions& options) {
  return BridgeDumper{options}.Dump(value, 0);
}

std::string DumpAvrocppDatum(const ::avro::GenericDatum& datum,
                             const DumpOptions& options) {
  // avro-cpp accessors throw: GenericEnum::symbol() on a position its schema
  // does not have, and value<T>() on a type mismatch. A datum whose own
  // accessors refuse it is a finding rather than a reason to abort the run, so
  // it is rendered as one.
  try {
    return CppDumper{options}.Dump(datum, 0, false);
  } catch (const std::exception& e) {
    return absl::StrCat("<threw: ", e.what(), ">");
  } catch (...) {
    return "<threw: unknown exception>";
  }
}

}  // namespace security::avro_fuzz
