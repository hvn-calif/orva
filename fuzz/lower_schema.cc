#include "fuzz/lower_schema.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace security::avro_fuzz {
namespace {

// The state threaded through the schema walk.
struct ScopeState {
  SchemaNames* names;
  int preorder_index = 0;
  int serial = 0;

  // Full names of the definitions lexically enclosing the current node. The
  // target of NameStrategy::kReuseAncestor, and therefore the source of
  // shadowing.
  std::vector<std::string> enclosing;
  // Every full name defined so far, in definition order so the walk stays
  // reproducible.
  std::vector<std::string> defined;

  // How many ancestors sit in a position whose value can terminate without
  // instantiating this node: an array (which may be empty), a map (likewise),
  // or a union with an alternative branch.
  //
  // A recursive named type is only emitted as a reference when this is
  // non-zero. Otherwise a schema like {"type":"record","name":"R","fields":
  // [{"name":"f","type":"R"}]} has no finite instance at all, and every value
  // property would have nothing to generate.
  int termination_guards = 0;
};

std::string ResolveNamespace(const Naming& naming) {
  return kNamespacePool[naming.namespace_id % kNamespacePoolSize];
}

std::string ResolveSimpleName(const Naming& naming, ScopeState& state) {
  switch (naming.strategy) {
    case NameStrategy::kPool:
      return kNamePool[naming.name_id % kNamePoolSize];
    case NameStrategy::kReserved:
      return kReservedWords[naming.name_id % kReservedWordsSize];
    case NameStrategy::kReuseAncestor:
      if (!state.enclosing.empty()) {
        // The enclosing entry is a full name; shadowing reuses the simple part.
        const std::string& full = state.enclosing.back();
        size_t dot = full.rfind('.');
        return dot == std::string::npos ? full : full.substr(dot + 1);
      }
      break;
    case NameStrategy::kReuseAnyInScope:
      if (!state.defined.empty()) {
        const std::string& full =
            state.defined[naming.name_id % state.defined.size()];
        size_t dot = full.rfind('.');
        return dot == std::string::npos ? full : full.substr(dot + 1);
      }
      break;
    case NameStrategy::kFresh:
    default:
      break;
  }
  return "N" + std::to_string(state.serial++);
}

std::string JoinName(const std::string& name_space, const std::string& simple) {
  return name_space.empty() ? simple : name_space + "." + simple;
}

struct ResolvedName {
  std::string simple;
  std::string name_space;
  std::string full;
  bool is_reference = false;
};

// Splits a full name into namespace and simple parts.
void SplitName(const std::string& full, std::string* name_space,
               std::string* simple) {
  size_t dot = full.rfind('.');
  if (dot == std::string::npos) {
    name_space->clear();
    *simple = full;
  } else {
    *name_space = full.substr(0, dot);
    *simple = full.substr(dot + 1);
  }
}

ResolvedName ResolveName(const Naming& naming, ScopeState& state) {
  ResolvedName out;

  // The two reuse strategies must reproduce an existing *full* name, namespace
  // included. Taking only the simple name and then drawing a fresh namespace
  // would almost never land on a name already in scope, so references -- and
  // therefore recursive named types -- would be unreachable.
  const bool reuses_existing =
      naming.strategy == NameStrategy::kReuseAncestor ||
      naming.strategy == NameStrategy::kReuseAnyInScope;
  if (reuses_existing) {
    const std::string* source = nullptr;
    if (naming.strategy == NameStrategy::kReuseAncestor &&
        !state.enclosing.empty()) {
      source = &state.enclosing.back();
    } else if (naming.strategy == NameStrategy::kReuseAnyInScope &&
               !state.defined.empty()) {
      source = &state.defined[naming.name_id % state.defined.size()];
    }
    if (source != nullptr) {
      out.full = *source;
      SplitName(out.full, &out.name_space, &out.simple);
    }
  }

  if (out.full.empty()) {
    out.simple = ResolveSimpleName(naming, state);
    out.name_space = ResolveNamespace(naming);
    out.full = JoinName(out.name_space, out.simple);
  }

  const bool already_defined =
      std::find(state.defined.begin(), state.defined.end(), out.full) !=
      state.defined.end();
  const bool can_terminate = state.termination_guards > 0;

  if (already_defined && can_terminate &&
      (naming.emit_as_reference ||
       naming.strategy == NameStrategy::kReuseAncestor ||
       naming.strategy == NameStrategy::kReuseAnyInScope)) {
    // Emit a bare reference. This is the only way a recursive named type
    // arises, and the guard is what keeps it inhabitable.
    out.is_reference = true;
    return out;
  }

  // Two definitions may not share a full name. Uniquify rather than emit a
  // redefinition, which both engines reject and which would cost every value
  // property its input.
  while (std::find(state.defined.begin(), state.defined.end(), out.full) !=
         state.defined.end()) {
    out.simple += "_" + std::to_string(state.serial++);
    out.full = JoinName(out.name_space, out.simple);
  }
  return out;
}

// Emits the `logicalType` annotation shared by the simple logical kinds, or
// nothing when the Kind carries no annotation.
const char* LogicalAnnotation(Kind kind) {
  switch (kind) {
    case Kind::kUuid: return "uuid";
    case Kind::kDate: return "date";
    case Kind::kTimeMillis: return "time-millis";
    case Kind::kTimeMicros: return "time-micros";
    case Kind::kTimestampMillis: return "timestamp-millis";
    case Kind::kTimestampMicros: return "timestamp-micros";
    case Kind::kTimestampNanos: return "timestamp-nanos";
    case Kind::kLocalTimestampMillis: return "local-timestamp-millis";
    case Kind::kLocalTimestampMicros: return "local-timestamp-micros";
    case Kind::kLocalTimestampNanos: return "local-timestamp-nanos";
    default: return nullptr;
  }
}

// The primitive an annotated logical type is layered on.
const char* LogicalBase(Kind kind) {
  switch (kind) {
    case Kind::kUuid: return "string";
    case Kind::kDate:
    case Kind::kTimeMillis: return "int";
    case Kind::kTimeMicros:
    case Kind::kTimestampMillis:
    case Kind::kTimestampMicros:
    case Kind::kTimestampNanos:
    case Kind::kLocalTimestampMillis:
    case Kind::kLocalTimestampMicros:
    case Kind::kLocalTimestampNanos: return "long";
    default: return nullptr;
  }
}

void Emit(const Node& node, ScopeState& state, std::string* out);

// Records this node's slot in the per-node name tables, growing them so the
// preorder index is a valid subscript for every node.
int ReserveSlot(ScopeState& state) {
  const int index = state.preorder_index++;
  if (state.names != nullptr) {
    state.names->full_name_by_preorder_index.resize(index + 1);
    state.names->is_reference_by_preorder_index.resize(index + 1);
  }
  return index;
}

void RecordName(ScopeState& state, int index, const std::string& full,
                bool is_reference) {
  if (state.names == nullptr) return;
  state.names->full_name_by_preorder_index[index] = full;
  state.names->is_reference_by_preorder_index[index] = is_reference;
}

void EmitNamedHeader(const ResolvedName& name, const char* type,
                     std::string* out) {
  *out += "{\"type\":\"";
  *out += type;
  *out += "\",\"name\":";
  *out += JsonQuote(name.simple);
  if (!name.name_space.empty()) {
    *out += ",\"namespace\":" + JsonQuote(name.name_space);
  }
}

void EmitRecord(const Node& node, ScopeState& state, int index,
                std::string* out) {
  ResolvedName name = ResolveName(node.naming, state);
  RecordName(state, index, name.full, name.is_reference);
  if (name.is_reference) {
    *out += JsonQuote(name.full);
    return;
  }
  state.defined.push_back(name.full);
  EmitNamedHeader(name, "record", out);
  if (node.naming.emit_doc) *out += ",\"doc\":\"generated\"";

  state.enclosing.push_back(name.full);
  *out += ",\"fields\":[";
  for (size_t i = 0; i < node.children.size(); ++i) {
    if (i != 0) *out += ",";
    *out += "{\"name\":" + JsonQuote(LabelAt(node, i)) + ",\"type\":";
    Emit(node.children[i], state, out);
    *out += "}";
  }
  *out += "]}";
  state.enclosing.pop_back();
}

void EmitEnum(const Node& node, ScopeState& state, int index,
              std::string* out) {
  ResolvedName name = ResolveName(node.naming, state);
  RecordName(state, index, name.full, name.is_reference);
  if (name.is_reference) {
    *out += JsonQuote(name.full);
    return;
  }
  state.defined.push_back(name.full);
  EmitNamedHeader(name, "enum", out);
  *out += ",\"symbols\":[";
  for (size_t i = 0; i < node.labels.size(); ++i) {
    if (i != 0) *out += ",";
    *out += JsonQuote(node.labels[i]);
  }
  *out += "]}";
}

void EmitFixedLike(const Node& node, ScopeState& state, int index,
                   bool decimal, std::string* out) {
  ResolvedName name = ResolveName(node.naming, state);
  RecordName(state, index, name.full, name.is_reference);
  if (name.is_reference) {
    *out += JsonQuote(name.full);
    return;
  }
  state.defined.push_back(name.full);
  EmitNamedHeader(name, "fixed", out);
  const int size = node.kind == Kind::kDuration ? 12 : node.selectors.fixed_size;
  *out += ",\"size\":" + std::to_string(size);
  if (decimal) {
    *out += ",\"logicalType\":\"decimal\",\"precision\":" +
            std::to_string(node.selectors.precision) +
            ",\"scale\":" + std::to_string(ResolveScale(node.selectors));
  } else if (node.kind == Kind::kDuration) {
    *out += ",\"logicalType\":\"duration\"";
  }
  *out += "}";
}

void Emit(const Node& node, ScopeState& state, std::string* out) {
  const int index = ReserveSlot(state);

  switch (node.kind) {
    case Kind::kRecord:
      EmitRecord(node, state, index, out);
      return;
    case Kind::kEnum:
      EmitEnum(node, state, index, out);
      return;
    case Kind::kFixed:
      EmitFixedLike(node, state, index, /*decimal=*/false, out);
      return;
    case Kind::kDecimalFixed:
      EmitFixedLike(node, state, index, /*decimal=*/true, out);
      return;
    case Kind::kDuration:
      EmitFixedLike(node, state, index, /*decimal=*/false, out);
      return;

    case Kind::kArray:
      // An array may be empty, so anything below it can terminate.
      ++state.termination_guards;
      *out += "{\"type\":\"array\",\"items\":";
      Emit(node.children.empty() ? Node{} : node.children[0], state, out);
      *out += "}";
      --state.termination_guards;
      return;

    case Kind::kMap:
      ++state.termination_guards;
      *out += "{\"type\":\"map\",\"values\":";
      Emit(node.children.empty() ? Node{} : node.children[0], state, out);
      *out += "}";
      --state.termination_guards;
      return;

    case Kind::kUnion: {
      // A union with more than one branch can always pick a different branch,
      // so its subtrees can terminate.
      const bool has_alternative = node.children.size() > 1;
      if (has_alternative) ++state.termination_guards;
      *out += "[";
      for (size_t i = 0; i < node.children.size(); ++i) {
        if (i != 0) *out += ",";
        Emit(node.children[i], state, out);
      }
      *out += "]";
      if (has_alternative) --state.termination_guards;
      return;
    }

    case Kind::kDecimalBytes:
      *out += "{\"type\":\"bytes\",\"logicalType\":\"decimal\",\"precision\":" +
              std::to_string(node.selectors.precision) +
              ",\"scale\":" + std::to_string(ResolveScale(node.selectors)) + "}";
      return;

    default:
      break;
  }

  if (const char* annotation = LogicalAnnotation(node.kind)) {
    *out += "{\"type\":\"";
    *out += LogicalBase(node.kind);
    *out += "\",\"logicalType\":\"";
    *out += annotation;
    *out += "\"}";
    return;
  }

  // Plain primitive.
  *out += "\"";
  *out += KindName(node.kind);
  *out += "\"";
}

}  // namespace

std::string JsonQuote(const std::string& raw) {
  std::string out = "\"";
  for (unsigned char c : raw) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04X", c);
          out += buf;
        } else {
          // Bytes >= 0x80 are passed through. A name never contains them, and
          // a map key is not part of the schema, so this only matters if a
          // caller quotes arbitrary bytes -- in which case emitting them raw
          // preserves what both parsers will actually see.
          out += static_cast<char>(c);
        }
    }
  }
  out += "\"";
  return out;
}

std::string ToSchemaJson(const Node& node, SchemaNames* names) {
  ScopeState state;
  state.names = names;
  std::string out;
  Emit(node, state, &out);
  return out;
}

std::string ToSchemaJson(const Node& node) {
  return ToSchemaJson(node, nullptr);
}

}  // namespace security::avro_fuzz
