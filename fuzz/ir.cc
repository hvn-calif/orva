#include "fuzz/ir.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "fuzz/suppress.h"

namespace security::avro_fuzz {
namespace {

struct KindInfo {
  const char* name;
  bool named;
  bool complex;
};

// Indexed by Kind. The `named` column drives scope resolution; `complex`
// distinguishes nodes that own children from leaves.
constexpr KindInfo kKindInfo[] = {
    {"null", false, false},
    {"boolean", false, false},
    {"int", false, false},
    {"long", false, false},
    {"float", false, false},
    {"double", false, false},
    {"bytes", false, false},
    {"string", false, false},
    {"record", true, true},
    {"enum", true, false},
    {"array", false, true},
    {"map", false, true},
    {"union", false, true},
    {"fixed", true, false},
    {"decimal", false, false},
    {"decimal", true, false},
    {"uuid", false, false},
    {"date", false, false},
    {"time-millis", false, false},
    {"time-micros", false, false},
    {"timestamp-millis", false, false},
    {"timestamp-micros", false, false},
    {"timestamp-nanos", false, false},
    {"local-timestamp-millis", false, false},
    {"local-timestamp-micros", false, false},
    {"local-timestamp-nanos", false, false},
    {"duration", true, false},
};

static_assert(sizeof(kKindInfo) / sizeof(kKindInfo[0]) ==
                  static_cast<size_t>(Kind::kMaxKind),
              "kKindInfo must have one entry per Kind");

const KindInfo& Info(Kind kind) {
  size_t index = static_cast<size_t>(kind);
  if (index >= static_cast<size_t>(Kind::kMaxKind)) index = 0;
  return kKindInfo[index];
}

// Replaces every byte sequence that is not valid UTF-8 with U+FFFD.
//
// Used only when D1 is suppressed, and applied before either lowering, so the
// bridge and avrocpp provably receive the same bytes.
std::string SanitizeUtf8(const std::string& input) {
  std::string out;
  out.reserve(input.size());
  size_t i = 0;
  while (i < input.size()) {
    unsigned char lead = static_cast<unsigned char>(input[i]);
    int length = 0;
    unsigned int code = 0;
    if (lead < 0x80) {
      length = 1;
      code = lead;
    } else if ((lead & 0xE0) == 0xC0) {
      length = 2;
      code = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
      length = 3;
      code = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
      length = 4;
      code = lead & 0x07;
    }

    bool valid = length > 0 && i + length <= input.size();
    for (int k = 1; valid && k < length; ++k) {
      unsigned char cont = static_cast<unsigned char>(input[i + k]);
      if ((cont & 0xC0) != 0x80) {
        valid = false;
      } else {
        code = (code << 6) | (cont & 0x3F);
      }
    }
    // Reject overlong encodings, surrogates and out-of-range code points, so
    // the sanitised string is what both engines would call well-formed.
    if (valid) {
      if (length == 2 && code < 0x80) valid = false;
      if (length == 3 && code < 0x800) valid = false;
      if (length == 4 && code < 0x10000) valid = false;
      if (code >= 0xD800 && code <= 0xDFFF) valid = false;
      if (code > 0x10FFFF) valid = false;
    }

    if (valid) {
      out.append(input, i, length);
      i += length;
    } else {
      out += "\xef\xbf\xbd";  // U+FFFD
      i += 1;
    }
  }
  return out;
}

// A branch of a union is distinguished by its type, not its position: Avro
// forbids two branches of the same unnamed type, and two named branches
// sharing a full name.
std::string BranchTypeKey(const Node& node) {
  const KindInfo& info = Info(node.kind);
  if (!info.named) return info.name;
  // Names are not resolved until schema lowering, so key a named branch on the
  // naming *intent*. This over-approximates -- two kFresh records key alike
  // here but would in fact get distinct names -- which is the safe direction:
  // it drops a branch that would have been legal rather than emitting a schema
  // both engines reject.
  return std::string(info.name) + ":" +
         std::to_string(static_cast<int>(node.naming.strategy)) + ":" +
         std::to_string(node.naming.name_id) + ":" +
         std::to_string(node.naming.namespace_id);
}

std::string Escape(const std::string& raw) {
  std::string out;
  out.reserve(raw.size() + 2);
  for (unsigned char c : raw) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      default:
        if (c < 0x20 || c >= 0x7F) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\x%02X", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

}  // namespace

bool IsNamedType(Kind kind) { return Info(kind).named; }
bool IsComplex(Kind kind) { return Info(kind).complex; }
const char* KindName(Kind kind) { return Info(kind).name; }

const char* const kNamePool[] = {"A", "B", "_", "x0", "Foo", "foo"};
const int kNamePoolSize = 6;

const char* const kReservedWords[] = {
    "null",  "boolean", "int",   "long",  "float", "double",  "bytes", "string",
    "record", "enum",   "array", "map",   "union", "fixed",   "error", "request"};
const int kReservedWordsSize = 16;

const char* const kNamespacePool[] = {
    "", "ns", "ns.sub", "a.b.c", "A", "_",
    "ns..bad",  // invalid: empty component
    "1bad"};    // invalid: leading digit
const int kNamespacePoolSize = 8;

std::string LabelAt(const Node& node, size_t i) {
  if (i < node.labels.size() && !node.labels[i].empty()) return node.labels[i];
  return "f" + std::to_string(i);
}

std::string KeyAt(const Node& node, size_t i) {
  if (i < node.keys.size()) return node.keys[i];
  return "k" + std::to_string(i);
}

uint32_t ResolveIndex(const Selectors& selectors, size_t size) {
  const uint32_t n = static_cast<uint32_t>(size);
  switch (selectors.index_mode) {
    case IndexMode::kFirst:
      return 0;
    case IndexMode::kLast:
      return n == 0 ? 0 : n - 1;
    case IndexMode::kMiddle:
      return n / 2;
    case IndexMode::kJustPast:
      return n;
    case IndexMode::kFarPast:
      return 0xFFFFFFFFu;
    case IndexMode::kRawMod:
    default:
      return n == 0 ? 0 : selectors.index_raw % n;
  }
}

int32_t ResolveScale(const Selectors& selectors) {
  const int32_t precision = static_cast<int32_t>(selectors.precision);
  switch (selectors.scale_mode) {
    case ScaleMode::kZero:
      return 0;
    case ScaleMode::kOne:
      return 1;
    case ScaleMode::kEqPrecision:
      return precision;
    case ScaleMode::kPrecisionPlus1:
      return precision + 1;
    case ScaleMode::kNegative:
      return -1;
    case ScaleMode::kRaw:
    default:
      return selectors.scale_raw;
  }
}

namespace {

// Carries the state Normalize threads through the tree.
struct NormalizeState {
  const NormalizeOptions* options;
  int nodes_used = 0;
  int serial = 0;
  std::set<std::string> defined_names;

  bool suppressed(DivergenceId id) const {
    return options->suppressions != nullptr &&
           options->suppressions->Contains(id);
  }
};

Node NormalizeNode(const Node& raw, NormalizeState& state, int depth);

// Rewrites the children of a complex node, clipping breadth and dropping
// children once the node budget is spent.
void NormalizeChildren(const Node& raw, Node& out, NormalizeState& state,
                       int depth, size_t min_children) {
  const size_t limit =
      std::min<size_t>(raw.children.size(),
                       static_cast<size_t>(state.options->max_breadth));
  for (size_t i = 0; i < limit; ++i) {
    if (state.nodes_used >= state.options->max_nodes &&
        out.children.size() >= min_children) {
      break;
    }
    out.children.push_back(NormalizeNode(raw.children[i], state, depth + 1));
  }
  // A union with no branches and an enum with no symbols are both illegal, and
  // an empty union has no inhabitant at all, so top them up rather than emit a
  // schema that can never carry a value.
  while (out.children.size() < min_children) {
    Node filler;
    filler.kind = Kind::kNull;
    out.children.push_back(filler);
    ++state.nodes_used;
  }
}

Node NormalizeNode(const Node& raw, NormalizeState& state, int depth) {
  ++state.nodes_used;

  Node out = raw;
  out.children.clear();
  out.labels.clear();
  out.keys.clear();

  const bool value_bearing =
      state.options->mode == NormalizeMode::kValueBearing;
  const bool out_of_budget = depth >= state.options->max_depth ||
                             state.nodes_used > state.options->max_nodes;

  // Past the depth or node budget, collapse to a leaf. Doing this by rewriting
  // the Kind rather than by refusing to recurse keeps the result a valid tree
  // for every input.
  if (out_of_budget && IsComplex(out.kind)) {
    out.kind = Kind::kNull;
    return out;
  }

  if (static_cast<size_t>(out.kind) >= static_cast<size_t>(Kind::kMaxKind)) {
    out.kind = Kind::kNull;
  }

  if (value_bearing) {
    // An in-range index everywhere; out-of-range indices are the exclusive
    // business of the property that tests for them.
    out.selectors.index_mode = IndexMode::kRawMod;

    // No bare name references in a value-bearing tree.
    //
    // A reference names a type defined elsewhere, so instantiating a value for
    // it means walking back to that definition -- and if the reference is
    // recursive, deciding where to stop. Both lowerings would have to make
    // that decision identically or the comparison is meaningless. Recursion is
    // still worth testing, but as a *schema* question (do the two parsers
    // agree it is legal?), which is what kSchemaOnly mode is for.
    out.naming.emit_as_reference = false;
    if (out.naming.strategy == NameStrategy::kReuseAncestor ||
        out.naming.strategy == NameStrategy::kReuseAnyInScope) {
      out.naming.strategy = NameStrategy::kPool;
    }

    out.selectors.fixed_size =
        static_cast<uint8_t>(std::clamp<int>(out.selectors.fixed_size, 1, 64));

    int precision = std::clamp<int>(out.selectors.precision, 1, 38);
    out.selectors.precision = static_cast<uint8_t>(precision);
    int scale = std::clamp<int>(ResolveScale(out.selectors), 0, precision);
    out.selectors.scale_mode = ScaleMode::kRaw;
    out.selectors.scale_raw = static_cast<int8_t>(scale);

    if (state.suppressed(DivergenceId::kD1StringNotUtf8) &&
        (out.kind == Kind::kString || out.kind == Kind::kUuid)) {
      out.scalars.blob = SanitizeUtf8(out.scalars.blob);
    }
  }

  switch (out.kind) {
    case Kind::kRecord: {
      NormalizeChildren(raw, out, state, depth, /*min_children=*/0);
      // Field names must be unique within a record; a duplicate is a schema
      // error on both engines rather than an interesting value divergence.
      std::set<std::string> used;
      for (size_t i = 0; i < out.children.size(); ++i) {
        std::string label = LabelAt(raw, i);
        if (value_bearing) {
          while (label.empty() || used.count(label) != 0) {
            label += "_" + std::to_string(state.serial++);
          }
          used.insert(label);
        }
        out.labels.push_back(label);
      }
      break;
    }

    case Kind::kEnum: {
      std::set<std::string> used;
      const size_t limit = std::min<size_t>(
          raw.labels.size(), static_cast<size_t>(state.options->max_breadth));
      for (size_t i = 0; i < limit; ++i) {
        std::string symbol = raw.labels[i];
        if (value_bearing) {
          while (symbol.empty() || used.count(symbol) != 0) {
            symbol += "_" + std::to_string(state.serial++);
          }
          used.insert(symbol);
        }
        out.labels.push_back(symbol);
      }
      if (out.labels.empty()) out.labels.push_back("S" + std::to_string(state.serial++));
      break;
    }

    case Kind::kArray:
      NormalizeChildren(raw, out, state, depth, /*min_children=*/1);
      break;

    case Kind::kMap: {
      NormalizeChildren(raw, out, state, depth, /*min_children=*/1);
      for (size_t i = 0; i < out.children.size(); ++i) {
        out.keys.push_back(KeyAt(raw, i));
      }
      // D2 suppressed: collapse duplicates last-write-wins, matching what the
      // bridge does internally, so avrocpp is handed the same entry list.
      if (state.suppressed(DivergenceId::kD2DuplicateMapKey)) {
        std::vector<Node> children;
        std::vector<std::string> keys;
        for (size_t i = 0; i < out.keys.size(); ++i) {
          auto existing = std::find(keys.begin(), keys.end(), out.keys[i]);
          if (existing == keys.end()) {
            keys.push_back(out.keys[i]);
            children.push_back(out.children[i]);
          } else {
            children[existing - keys.begin()] = out.children[i];
          }
        }
        out.keys = std::move(keys);
        out.children = std::move(children);
      }
      break;
    }

    case Kind::kUnion: {
      NormalizeChildren(raw, out, state, depth, /*min_children=*/1);
      if (value_bearing) {
        // Avro forbids two branches of the same type. Drop later duplicates
        // rather than rename them, since renaming would change the schema
        // shape the generator asked for.
        std::vector<Node> kept;
        std::set<std::string> seen;
        for (Node& branch : out.children) {
          // A union may not immediately contain another union. Demote it to a
          // leaf, and drop its children with it: a kNull node that still owns
          // children is not a fixed point, because re-normalising would clear
          // them and Normalize would stop being idempotent.
          if (branch.kind == Kind::kUnion) {
            branch.kind = Kind::kNull;
            branch.children.clear();
            branch.labels.clear();
            branch.keys.clear();
          }
          std::string key = BranchTypeKey(branch);
          if (seen.insert(key).second) kept.push_back(branch);
        }
        if (kept.empty()) {
          Node filler;
          filler.kind = Kind::kNull;
          kept.push_back(filler);
        }
        out.children = std::move(kept);
      }
      break;
    }

    case Kind::kFixed:
    case Kind::kDecimalFixed:
      if (value_bearing) {
        out.scalars.blob.resize(out.selectors.fixed_size, '\0');
      }
      break;

    default:
      break;
  }

  return out;
}

}  // namespace

Node Normalize(const Node& raw, const NormalizeOptions& options) {
  NormalizeState state;
  state.options = &options;
  return NormalizeNode(raw, state, 0);
}

std::string ToDebugString(const Node& node) {
  std::string out = KindName(node.kind);
  switch (node.kind) {
    case Kind::kRecord:
    case Kind::kUnion:
    case Kind::kArray:
    case Kind::kMap: {
      out += "(";
      for (size_t i = 0; i < node.children.size(); ++i) {
        if (i != 0) out += ", ";
        if (node.kind == Kind::kRecord) out += LabelAt(node, i) + ": ";
        if (node.kind == Kind::kMap) out += "\"" + Escape(KeyAt(node, i)) + "\": ";
        out += ToDebugString(node.children[i]);
      }
      out += ")";
      break;
    }
    case Kind::kEnum:
      out += "[" + std::to_string(node.labels.size()) + " symbols @" +
             std::to_string(ResolveIndex(node.selectors, node.labels.size())) + "]";
      break;
    case Kind::kString:
    case Kind::kBytes:
    case Kind::kFixed:
    case Kind::kUuid:
      out += "(\"" + Escape(node.scalars.blob) + "\")";
      break;
    default:
      break;
  }
  return out;
}

std::string ToCppLiteral(const Node& node) {
  std::string out = "MakeNode(Kind::k";
  const char* name = KindName(node.kind);
  out += name;
  out += ")";
  // A full aggregate dump is unreadable; the fields that matter for a
  // reproducer are the kind, the blob and the children.
  if (!node.scalars.blob.empty()) {
    out += ".WithBlob(\"" + Escape(node.scalars.blob) + "\")";
  }
  if (node.scalars.integer != 0) {
    out += ".WithInteger(" + std::to_string(node.scalars.integer) + "LL)";
  }
  for (size_t i = 0; i < node.children.size(); ++i) {
    out += "\n  .WithChild(";
    if (node.kind == Kind::kRecord) {
      out += "\"" + Escape(LabelAt(node, i)) + "\", ";
    } else if (node.kind == Kind::kMap) {
      out += "\"" + Escape(KeyAt(node, i)) + "\", ";
    }
    out += ToCppLiteral(node.children[i]) + ")";
  }
  return out;
}

}  // namespace security::avro_fuzz
