#include "fuzz/lower_schema.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"

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

absl::string_view SimplePartOf(absl::string_view full) {
  const size_t dot = full.rfind('.');
  return dot == absl::string_view::npos ? full : full.substr(dot + 1);
}

absl::string_view NamespacePartOf(absl::string_view full) {
  const size_t dot = full.rfind('.');
  return dot == absl::string_view::npos ? absl::string_view()
                                        : full.substr(0, dot);
}

std::string ResolveSimpleName(const Naming& naming, ScopeState& state) {
  switch (naming.strategy) {
    case NameStrategy::kPool:
      return kNamePool[naming.name_id % kNamePoolSize];
    case NameStrategy::kReserved:
      return kReservedWords[naming.name_id % kReservedWordsSize];
    case NameStrategy::kReuseAncestor:
      // The enclosing entry is a full name; shadowing reuses the simple part.
      if (!state.enclosing.empty()) {
        return std::string(SimplePartOf(state.enclosing.back()));
      }
      break;
    case NameStrategy::kReuseAnyInScope:
      if (!state.defined.empty()) {
        return std::string(SimplePartOf(
            state.defined[naming.name_id % state.defined.size()]));
      }
      break;
    case NameStrategy::kFresh:
    default:
      break;
  }
  return absl::StrCat("N", state.serial++);
}

std::string JoinName(absl::string_view name_space, absl::string_view simple) {
  return name_space.empty() ? std::string(simple)
                            : absl::StrCat(name_space, ".", simple);
}

struct ResolvedName {
  std::string simple;
  std::string name_space;
  std::string full;
  bool is_reference = false;
};

void SplitName(absl::string_view full, std::string* name_space,
               std::string* simple) {
  const size_t dot = full.rfind('.');
  if (dot == absl::string_view::npos) {
    name_space->clear();
    simple->assign(full);
  } else {
    name_space->assign(full.substr(0, dot));
    simple->assign(full.substr(dot + 1));
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
    // Avro resolves a name carrying no namespace against the enclosing
    // definition's namespace, so the uniquifier below has to compare the names
    // the *parsers* will see rather than the ones the pool handed out. Without
    // this, an inner record named A under an outer ns.A was emitted as a second
    // definition, both engines resolved it to ns.A, and avro-cpp turned the
    // duplicate into a symbolic reference -- a record cycle whose GenericDatum
    // construction recurses until the stack is gone.
    if (out.name_space.empty() && !state.enclosing.empty()) {
      out.name_space = std::string(NamespacePartOf(state.enclosing.back()));
    }
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
    absl::StrAppend(&out.simple, "_", state.serial++);
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

void RecordName(ScopeState& state, int index, absl::string_view full,
                bool is_reference) {
  if (state.names == nullptr) return;
  state.names->full_name_by_preorder_index[index].assign(full);
  state.names->is_reference_by_preorder_index[index] = is_reference;
}

void EmitNamedHeader(const ResolvedName& name, absl::string_view type,
                     std::string* out) {
  absl::StrAppend(out, "{\"type\":\"", type, "\",\"name\":",
                  JsonQuote(name.simple));
  if (!name.name_space.empty()) {
    absl::StrAppend(out, ",\"namespace\":", JsonQuote(name.name_space));
  }
}

void EmitRecord(const Node& node, ScopeState& state, int index,
                std::string* out) {
  ResolvedName name = ResolveName(node.naming, state);
  RecordName(state, index, name.full, name.is_reference);
  if (name.is_reference) {
    absl::StrAppend(out, JsonQuote(name.full));
    return;
  }
  state.defined.push_back(name.full);
  EmitNamedHeader(name, "record", out);
  if (node.naming.emit_doc) absl::StrAppend(out, ",\"doc\":\"generated\"");

  state.enclosing.push_back(name.full);
  absl::StrAppend(out, ",\"fields\":[");
  for (size_t i = 0; i < node.children.size(); ++i) {
    if (i != 0) absl::StrAppend(out, ",");
    absl::StrAppend(out, "{\"name\":", JsonQuote(LabelAt(node, i)),
                    ",\"type\":");
    Emit(node.children[i], state, out);
    absl::StrAppend(out, "}");
  }
  absl::StrAppend(out, "]}");
  state.enclosing.pop_back();
}

void EmitEnum(const Node& node, ScopeState& state, int index,
              std::string* out) {
  ResolvedName name = ResolveName(node.naming, state);
  RecordName(state, index, name.full, name.is_reference);
  if (name.is_reference) {
    absl::StrAppend(out, JsonQuote(name.full));
    return;
  }
  state.defined.push_back(name.full);
  EmitNamedHeader(name, "enum", out);
  absl::StrAppend(out, ",\"symbols\":[");
  for (size_t i = 0; i < node.labels.size(); ++i) {
    if (i != 0) absl::StrAppend(out, ",");
    absl::StrAppend(out, JsonQuote(node.labels[i]));
  }
  absl::StrAppend(out, "]}");
}

void EmitFixedLike(const Node& node, ScopeState& state, int index,
                   bool decimal, std::string* out) {
  ResolvedName name = ResolveName(node.naming, state);
  RecordName(state, index, name.full, name.is_reference);
  if (name.is_reference) {
    absl::StrAppend(out, JsonQuote(name.full));
    return;
  }
  state.defined.push_back(name.full);
  EmitNamedHeader(name, "fixed", out);
  const int size = node.kind == Kind::kDuration ? 12 : node.selectors.fixed_size;
  absl::StrAppend(out, ",\"size\":", size);
  if (decimal) {
    absl::StrAppend(out, ",\"logicalType\":\"decimal\",\"precision\":",
                    node.selectors.precision,
                    ",\"scale\":", ResolveScale(node.selectors));
  } else if (node.kind == Kind::kDuration) {
    absl::StrAppend(out, ",\"logicalType\":\"duration\"");
  }
  absl::StrAppend(out, "}");
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
      absl::StrAppend(out, "{\"type\":\"array\",\"items\":");
      Emit(node.children.empty() ? Node{} : node.children[0], state, out);
      absl::StrAppend(out, "}");
      --state.termination_guards;
      return;

    case Kind::kMap:
      ++state.termination_guards;
      absl::StrAppend(out, "{\"type\":\"map\",\"values\":");
      Emit(node.children.empty() ? Node{} : node.children[0], state, out);
      absl::StrAppend(out, "}");
      --state.termination_guards;
      return;

    case Kind::kUnion: {
      // A union with more than one branch can always pick a different branch,
      // so its subtrees can terminate.
      const bool has_alternative = node.children.size() > 1;
      if (has_alternative) ++state.termination_guards;
      absl::StrAppend(out, "[");
      for (size_t i = 0; i < node.children.size(); ++i) {
        if (i != 0) absl::StrAppend(out, ",");
        Emit(node.children[i], state, out);
      }
      absl::StrAppend(out, "]");
      if (has_alternative) --state.termination_guards;
      return;
    }

    case Kind::kDecimalBytes:
      absl::StrAppend(out,
                      "{\"type\":\"bytes\",\"logicalType\":\"decimal\","
                      "\"precision\":",
                      node.selectors.precision,
                      ",\"scale\":", ResolveScale(node.selectors), "}");
      return;

    default:
      break;
  }

  if (const char* annotation = LogicalAnnotation(node.kind)) {
    absl::StrAppend(out, "{\"type\":\"", LogicalBase(node.kind),
                    "\",\"logicalType\":\"", annotation, "\"}");
    return;
  }

  absl::StrAppend(out, "\"", KindName(node.kind), "\"");
}

}  // namespace

std::string JsonQuote(absl::string_view raw) {
  std::string out = "\"";
  for (unsigned char c : raw) {
    switch (c) {
      case '"': absl::StrAppend(&out, "\\\""); break;
      case '\\': absl::StrAppend(&out, "\\\\"); break;
      case '\b': absl::StrAppend(&out, "\\b"); break;
      case '\f': absl::StrAppend(&out, "\\f"); break;
      case '\n': absl::StrAppend(&out, "\\n"); break;
      case '\r': absl::StrAppend(&out, "\\r"); break;
      case '\t': absl::StrAppend(&out, "\\t"); break;
      default:
        if (c < 0x20) {
          absl::StrAppendFormat(&out, "\\u%04X", c);
        } else {
          // Bytes >= 0x80 are passed through. A name never contains them, and
          // a map key is not part of the schema, so this only matters if a
          // caller quotes arbitrary bytes -- in which case emitting them raw
          // preserves what both parsers will actually see.
          out.push_back(static_cast<char>(c));
        }
    }
  }
  out.push_back('"');
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
