#include "fuzz/lower_avrocpp.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "avro/GenericDatum.hh"
#include "avro/Node.hh"
#include "avro/Types.hh"
#include "avro/ValidSchema.hh"
#include "fuzz/ir.h"

namespace security::avro_fuzz {
namespace {

std::vector<uint8_t> ToBytes(const std::string& raw) {
  return std::vector<uint8_t>(raw.begin(), raw.end());
}

CppOutcome Failure(const std::string& what) {
  return {CppVerdict::kOtherStdException, what};
}

CppOutcome Fill(const Node& node, const ::avro::NodePtr& schema,
                ::avro::GenericDatum* out);

CppOutcome FillRecord(const Node& node, const ::avro::NodePtr& schema,
                      ::avro::GenericDatum* out) {
  auto& record = out->value<::avro::GenericRecord>();
  // fieldAt is an unchecked fields_[pos], so the bound comes from us. Walking
  // the schema's leaf count rather than the IR's child count keeps the two in
  // step even if a lowering ever drops a field.
  const size_t fields = record.fieldCount();
  for (size_t i = 0; i < fields && i < node.children.size(); ++i) {
    CppOutcome inner =
        Fill(node.children[i], schema->leafAt(i), &record.fieldAt(i));
    if (!inner.ok()) return inner;
  }
  return {};
}

CppOutcome FillArray(const Node& node, const ::avro::NodePtr& schema,
                     ::avro::GenericDatum* out) {
  auto& array = out->value<::avro::GenericArray>();
  array.value().clear();
  if (node.children.empty()) return {};

  const ::avro::NodePtr& item_schema = schema->leafAt(0);
  for (size_t i = 0; i < node.children.size(); ++i) {
    // Every element inhabits the single item type, so all of them are built
    // from children[0] -- the same rule the bridge lowering follows.
    ::avro::GenericDatum item(item_schema);
    CppOutcome inner = Fill(node.children[0], item_schema, &item);
    if (!inner.ok()) return inner;
    array.value().push_back(item);
  }
  return {};
}

CppOutcome FillMap(const Node& node, const ::avro::NodePtr& schema,
                   ::avro::GenericDatum* out) {
  auto& map = out->value<::avro::GenericMap>();
  map.value().clear();
  if (node.children.empty()) return {};

  const ::avro::NodePtr& value_schema = schema->leafAt(1);
  for (size_t i = 0; i < node.children.size(); ++i) {
    ::avro::GenericDatum value(value_schema);
    CppOutcome inner = Fill(node.children[0], value_schema, &value);
    if (!inner.ok()) return inner;
    // Duplicate keys are pushed verbatim. GenericMap is a vector of pairs, so
    // both entries survive -- which is exactly the D2 behaviour the bridge
    // does not reproduce, and the reason this must not deduplicate.
    map.value().emplace_back(KeyAt(node, i), value);
  }
  return {};
}

CppOutcome FillUnion(const Node& node, const ::avro::NodePtr& schema,
                     ::avro::GenericDatum* out) {
  if (node.children.empty()) return Failure("union with no branches");
  const uint32_t branch = ResolveIndex(node.selectors, node.children.size());
  if (branch >= schema->leaves()) {
    return Failure("union branch out of range for the schema");
  }
  // selectBranch on a non-union is a null dereference rather than a throw
  // (GenericDatum::selectBranch uses the pointer form of any_cast), so guard.
  if (!out->isUnion()) return Failure("expected a union datum");
  CppOutcome selected = CallAvrocpp("GenericDatum::selectBranch", [&] {
    out->selectBranch(branch);
  });
  if (!selected.ok()) return selected;

  // value<T>(), type() and logicalType() are all transparent through a union,
  // so filling the branch is just filling `out` against the branch schema.
  return Fill(node.children[branch], schema->leafAt(branch), out);
}

CppOutcome Fill(const Node& node, const ::avro::NodePtr& schema,
                ::avro::GenericDatum* out) {
  if (schema == nullptr) return Failure("null schema node");

  // A value-bearing tree never emits a bare reference (see Normalize), so a
  // symbolic node here means the two lowerings have drifted apart.
  if (schema->type() == ::avro::AVRO_SYMBOLIC) {
    return Failure("symbolic schema reference in a value-bearing tree");
  }

  const int64_t integer = node.scalars.integer;

  return CallAvrocpp("ToAvrocppDatum", [&]() -> void {
    switch (node.kind) {
      case Kind::kNull:
        break;
      case Kind::kBoolean:
        out->value<bool>() = node.scalars.boolean;
        break;
      case Kind::kInt:
      case Kind::kDate:
      case Kind::kTimeMillis:
        out->value<int32_t>() = static_cast<int32_t>(integer);
        break;
      case Kind::kLong:
      case Kind::kTimeMicros:
      case Kind::kTimestampMillis:
      case Kind::kTimestampMicros:
      case Kind::kTimestampNanos:
      case Kind::kLocalTimestampMillis:
      case Kind::kLocalTimestampMicros:
      case Kind::kLocalTimestampNanos:
        out->value<int64_t>() = integer;
        break;
      case Kind::kFloat:
        out->value<float>() = node.scalars.f32;
        break;
      case Kind::kDouble:
        out->value<double>() = node.scalars.f64;
        break;
      case Kind::kBytes:
      case Kind::kDecimalBytes:
        out->value<std::vector<uint8_t>>() = ToBytes(node.scalars.blob);
        break;
      case Kind::kString:
      case Kind::kUuid:
        // No validation: avro-cpp stores whatever bytes it is given. That is
        // the whole of D1 on this side.
        out->value<std::string>() = node.scalars.blob;
        break;
      case Kind::kEnum: {
        auto& symbol = out->value<::avro::GenericEnum>();
        const uint32_t position =
            ResolveIndex(node.selectors, node.labels.size());
        symbol.set(position);
        break;
      }
      case Kind::kFixed:
      case Kind::kDecimalFixed:
      case Kind::kDuration: {
        auto& fixed = out->value<::avro::GenericFixed>();
        std::vector<uint8_t> bytes = ToBytes(node.scalars.blob);
        // A fixed's payload length is fixed by its schema; pad or truncate so
        // both engines see the same bytes rather than one of them erroring.
        bytes.resize(fixed.value().size(), 0);
        if (node.kind == Kind::kDuration && bytes.size() == 12) {
          // duration is three little-endian uint32: months, days, millis.
          const uint32_t parts[3] = {node.scalars.months, node.scalars.days,
                                     node.scalars.millis};
          for (int part = 0; part < 3; ++part) {
            for (int byte = 0; byte < 4; ++byte) {
              bytes[part * 4 + byte] =
                  static_cast<uint8_t>((parts[part] >> (8 * byte)) & 0xFF);
            }
          }
        }
        fixed.value() = bytes;
        break;
      }
      default:
        break;
    }
  });
}

}  // namespace

CppOutcome ToAvrocppDatum(const Node& node, const ::avro::NodePtr& schema,
                          ::avro::GenericDatum* out) {
  if (out == nullptr) return Failure("null output datum");

  CppOutcome constructed = CallAvrocpp("GenericDatum(NodePtr)", [&] {
    *out = ::avro::GenericDatum(schema);
  });
  if (!constructed.ok()) return constructed;

  switch (node.kind) {
    case Kind::kRecord:
      return FillRecord(node, schema, out);
    case Kind::kArray:
      return FillArray(node, schema, out);
    case Kind::kMap:
      return FillMap(node, schema, out);
    case Kind::kUnion:
      return FillUnion(node, schema, out);
    default:
      return Fill(node, schema, out);
  }
}

CppOutcome ToAvrocppDatum(const Node& node, const ::avro::ValidSchema& schema,
                          ::avro::GenericDatum* out) {
  return ToAvrocppDatum(node, schema.root(), out);
}

}  // namespace security::avro_fuzz
