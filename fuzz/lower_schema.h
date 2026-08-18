#ifndef SECURITY_AVRO_FUZZ_LOWER_SCHEMA_H_
#define SECURITY_AVRO_FUZZ_LOWER_SCHEMA_H_

#include <string>
#include <vector>

#include "absl/strings/string_view.h"
#include "fuzz/ir.h"

// Renders an IR tree as Avro schema JSON.
//
// This translation unit depends on `ir.h` and nothing else -- not the bridge,
// not avrocpp. Keeping the three lowerings on disjoint dependency edges is
// what lets a bridge-only fuzz target exist that never links avrocpp, so a
// sanitizer report from that binary is unambiguously about the bridge.
namespace security::avro_fuzz {

// The names a lowering assigned, in the order the tree was walked.
//
// The value lowerings need these: an avrocpp `GenericEnum` is constructed from
// its schema node, and a record field is looked up by the same name the schema
// declared. Recomputing the names independently in each lowering would let
// them drift.
struct SchemaNames {
  // Full name assigned to each named-type node, keyed by the node's index in
  // a pre-order walk of the tree.
  std::vector<std::string> full_name_by_preorder_index;
  // True where a named-type node was emitted as a bare reference rather than
  // as a definition. A reference has no value-side structure of its own; the
  // value lowering must resolve it against the definition.
  std::vector<bool> is_reference_by_preorder_index;
};

// Renders `node` as Avro schema JSON. Total: every tree has a rendering, though
// a tree normalized with kSchemaOnly may well render something both engines
// reject, which is the point of the parse-verdict property.
std::string ToSchemaJson(const Node& node);

// As above, and also reports the names assigned.
std::string ToSchemaJson(const Node& node, SchemaNames* names);

// Escapes `raw` as a JSON string literal, including the surrounding quotes.
// Exposed because the value lowerings need the same escaping for map keys.
std::string JsonQuote(absl::string_view raw);

}  // namespace security::avro_fuzz

#endif  // SECURITY_AVRO_FUZZ_LOWER_SCHEMA_H_
