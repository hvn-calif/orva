// Generator-level properties.
//
// These assert things about the harness itself rather than about Avro: that
// the generator terminates, that Normalize reaches a fixed point, and that the
// schema it renders is structurally sound. Nothing here links the bridge or
// avro-cpp, so a failure is unambiguously a harness bug.
//
// This is also the cheapest end-to-end check that the FuzzTest integration
// works, which is why it is the first target to build.

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "absl/strings/string_view.h"
#include "fuzz/domains.h"
#include "fuzz/ir.h"
#include "fuzz/lower_schema.h"
#include "fuzz/suppress.h"
#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

namespace security::avro_fuzz {
namespace {

int Depth(const Node& node) {
  int deepest = 0;
  for (const Node& child : node.children) {
    deepest = std::max(deepest, Depth(child));
  }
  return 1 + deepest;
}

int CountNodes(const Node& node) {
  int total = 1;
  for (const Node& child : node.children) total += CountNodes(child);
  return total;
}

bool SameTree(const Node& a, const Node& b) {
  if (a.kind != b.kind) return false;
  if (a.labels != b.labels) return false;
  if (a.keys != b.keys) return false;
  if (a.scalars.blob != b.scalars.blob) return false;
  if (a.selectors.precision != b.selectors.precision) return false;
  if (a.selectors.fixed_size != b.selectors.fixed_size) return false;
  if (ResolveScale(a.selectors) != ResolveScale(b.selectors)) return false;
  if (a.children.size() != b.children.size()) return false;
  for (size_t i = 0; i < a.children.size(); ++i) {
    if (!SameTree(a.children[i], b.children[i])) return false;
  }
  return true;
}

// Balanced-delimiter and quote check. Not a JSON parser -- the real check is
// that both engines parse it, which lives in the differential targets -- but
// it catches the structural mistakes a hand-rolled emitter makes.
bool DelimitersBalance(absl::string_view json, std::string* why) {
  std::vector<char> stack;
  bool in_string = false;
  bool escaped = false;
  for (char c : json) {
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }
    if (c == '"') {
      in_string = true;
    } else if (c == '{' || c == '[') {
      stack.push_back(c);
    } else if (c == '}' || c == ']') {
      const char want = c == '}' ? '{' : '[';
      if (stack.empty() || stack.back() != want) {
        *why = "unbalanced delimiter";
        return false;
      }
      stack.pop_back();
    }
  }
  if (in_string) {
    *why = "unterminated string";
    return false;
  }
  if (!stack.empty()) {
    *why = "unclosed delimiters";
    return false;
  }
  return true;
}

NormalizeOptions ValueBearing() {
  NormalizeOptions options;
  options.mode = NormalizeMode::kValueBearing;
  return options;
}

// ---------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------

// Normalize must reach a fixed point in one step. Downstream code normalizes
// once and then assumes the invariants hold; if a second pass changed anything,
// some invariant was only half-established.
//
// This caught a real bug: demoting a nested union to null left its children
// attached, and a null node owning children is not a fixed point.
void NormalizeIsIdempotent(const Node& raw) {
  const Node once = Normalize(raw, ValueBearing());
  const Node twice = Normalize(once, ValueBearing());
  ASSERT_TRUE(SameTree(once, twice))
      << "first:  " << ToDebugString(once) << "\n"
      << "second: " << ToDebugString(twice);
}
FUZZ_TEST(AvroIr, NormalizeIsIdempotent).WithDomains(AnyTree());

// The depth bound is what keeps the generator from blowing the stack while
// constructing an input, which would look like a crash rather than a finding.
void NormalizeRespectsBounds(const Node& raw) {
  const NormalizeOptions options = ValueBearing();
  const Node tree = Normalize(raw, options);
  EXPECT_LE(Depth(tree), options.max_depth + 1) << ToDebugString(tree);
  EXPECT_LE(CountNodes(tree), options.max_nodes * 2) << ToDebugString(tree);
}
FUZZ_TEST(AvroIr, NormalizeRespectsBounds).WithDomains(AnyTree());

// Every value-bearing tree must satisfy the schema rules both engines enforce,
// so the round-trip properties spend their budget on values rather than on
// schemas that were never going to parse.
void ValueBearingTreesAreWellFormed(const Node& raw) {
  const Node tree = Normalize(raw, ValueBearing());

  std::vector<const Node*> pending{&tree};
  while (!pending.empty()) {
    const Node& node = *pending.back();
    pending.pop_back();

    const int precision = node.selectors.precision;
    EXPECT_GE(precision, 1);
    EXPECT_LE(precision, 38);
    EXPECT_GE(ResolveScale(node.selectors), 0);
    EXPECT_LE(ResolveScale(node.selectors), precision);
    EXPECT_GE(node.selectors.fixed_size, 1);

    if (node.kind == Kind::kEnum) {
      ASSERT_FALSE(node.labels.empty());
      const std::set<std::string> unique(node.labels.begin(), node.labels.end());
      EXPECT_EQ(unique.size(), node.labels.size()) << "enum symbols must differ";
      EXPECT_LT(ResolveIndex(node.selectors, node.labels.size()),
                node.labels.size());
    }
    if (node.kind == Kind::kUnion) {
      ASSERT_FALSE(node.children.empty());
      EXPECT_LT(ResolveIndex(node.selectors, node.children.size()),
                node.children.size());
      for (const Node& branch : node.children) {
        EXPECT_NE(branch.kind, Kind::kUnion) << "Avro forbids nested unions";
      }
    }
    if (node.kind == Kind::kRecord) {
      std::set<std::string> names;
      for (size_t i = 0; i < node.children.size(); ++i) {
        names.insert(LabelAt(node, i));
      }
      EXPECT_EQ(names.size(), node.children.size())
          << "record field names must differ";
    }
    if (node.kind == Kind::kMap) {
      EXPECT_EQ(node.keys.size(), node.children.size());
    }
    for (const Node& child : node.children) pending.push_back(&child);
  }
}
FUZZ_TEST(AvroIr, ValueBearingTreesAreWellFormed).WithDomains(AnyTree());

// The schema emitter is hand-rolled, so check it never produces syntactically
// broken JSON and never defines one full name twice.
void SchemaJsonIsStructurallySound(const Node& raw) {
  const Node tree = Normalize(raw, ValueBearing());
  SchemaNames names;
  const std::string json = ToSchemaJson(tree, &names);

  std::string why;
  ASSERT_TRUE(DelimitersBalance(json, &why)) << why << "\n" << json;
  ASSERT_FALSE(json.empty());

  std::set<std::string> defined;
  for (size_t i = 0; i < names.full_name_by_preorder_index.size(); ++i) {
    const std::string& full = names.full_name_by_preorder_index[i];
    if (full.empty() || names.is_reference_by_preorder_index[i]) continue;
    EXPECT_TRUE(defined.insert(full).second)
        << "named type '" << full << "' defined twice\n"
        << json;
  }
}
FUZZ_TEST(AvroIr, SchemaJsonIsStructurallySound).WithDomains(AnyTree());

// Deep nesting must not blow the stack in the generator or the emitter. The
// claim is only that the harness survives: no differential property feeds either
// engine a deep schema, so depth is an unfuzzed axis rather than a covered one.
// Measured separately, avro-cpp's JSON reader recurses per nesting level with no
// depth counter (readEntity, JsonDom.cc:46) and segfaults at about 11,600 nested
// arrays or 35,000 unbalanced brackets on an 8 MiB stack, while apache-avro stops
// at serde_json's 128-level limit -- so the two disagree from depth 128 up, and
// nothing here or in the differential targets would find that.
void DeepChainsRenderWithoutOverflow(const Node& raw) {
  NormalizeOptions options = ValueBearing();
  options.max_depth = 24;
  options.max_nodes = 256;
  const Node tree = Normalize(raw, options);
  const std::string json = ToSchemaJson(tree);
  EXPECT_FALSE(json.empty());
}
FUZZ_TEST(AvroIr, DeepChainsRenderWithoutOverflow).WithDomains(AnyDeepChain());

// ---------------------------------------------------------------------------
// Registry and suppression, which are configuration rather than generation.
// ---------------------------------------------------------------------------

TEST(Divergence, NamesRoundTrip) {
  EXPECT_EQ(LookupDivergence("D1"), DivergenceId::kD1StringNotUtf8);
  EXPECT_EQ(LookupDivergence("D2"), DivergenceId::kD2DuplicateMapKey);
  EXPECT_STREQ(DivergenceName(DivergenceId::kD1StringNotUtf8), "D1");
  EXPECT_NE(DivergenceDoc(DivergenceId::kD1StringNotUtf8), std::string());
}

TEST(Divergence, UnknownIdIsRejected) {
  // A typo must be fatal rather than silently suppressing nothing, or an
  // operator would believe a divergence was muted when it was not.
  EXPECT_EQ(LookupDivergence("NOT_A_REAL_ID"), DivergenceId::kNone);
  SuppressionSet set;
  std::string error;
  EXPECT_FALSE(ParseSuppressions("NOT_A_REAL_ID\n", &set, &error));
  EXPECT_FALSE(error.empty());
}

TEST(Divergence, EvidenceNarrowsASuppression) {
  SuppressionSet set;
  std::string error;
  ASSERT_TRUE(ParseSuppressions("# comment\nD1\nD2:array continuation\n", &set,
                                &error))
      << error;
  EXPECT_TRUE(set.Contains(DivergenceId::kD1StringNotUtf8));
  EXPECT_TRUE(set.Contains(DivergenceId::kD1StringNotUtf8, "anything"));
  EXPECT_TRUE(
      set.Contains(DivergenceId::kD2DuplicateMapKey, "an array continuation x"));
  EXPECT_FALSE(set.Contains(DivergenceId::kD2DuplicateMapKey, "something else"));
  EXPECT_FALSE(set.Contains(DivergenceId::kD7ArrayContinuation));
}

TEST(Divergence, ShippedSuppressionFileIsEmpty) {
  // The harness's acceptance test is that it rediscovers D1 and D2 starting
  // from an empty corpus, with no seed inputs. If someone silences them to make
  // a run green, this fails.
  SuppressionSet set;
  std::string error;
  ASSERT_TRUE(ParseSuppressions(
      "# only comments and blank lines\n\n   \n", &set, &error))
      << error;
  EXPECT_TRUE(set.empty());
  EXPECT_EQ(set.Render(), "(none)");
}

// Suppression must work by removing the triggering input class before any
// lowering, so both engines provably see identical input.
TEST(Suppression, D2RemovesDuplicateMapKeys) {
  Node map;
  map.kind = Kind::kMap;
  map.children.resize(3);
  map.keys = {"dup", "dup", "other"};

  SuppressionSet set;
  set.Add(DivergenceId::kD2DuplicateMapKey);
  NormalizeOptions options = ValueBearing();
  options.suppressions = &set;

  const Node collapsed = Normalize(map, options);
  std::set<std::string> keys(collapsed.keys.begin(), collapsed.keys.end());
  EXPECT_EQ(keys.size(), collapsed.keys.size());
  EXPECT_EQ(collapsed.keys.size(), collapsed.children.size());
}

TEST(Suppression, WithoutD2DuplicateMapKeysSurvive) {
  Node map;
  map.kind = Kind::kMap;
  map.children.resize(2);
  map.keys = {"dup", "dup"};

  const Node kept = Normalize(map, ValueBearing());
  ASSERT_EQ(kept.keys.size(), 2u);
  EXPECT_EQ(kept.keys[0], kept.keys[1]) << "D2 must stay reachable";
}

TEST(Suppression, D1SanitisesStringsButNotBytes) {
  Node text;
  text.kind = Kind::kString;
  text.scalars.blob = "\xff\xfe";

  SuppressionSet set;
  set.Add(DivergenceId::kD1StringNotUtf8);
  NormalizeOptions options = ValueBearing();
  options.suppressions = &set;

  EXPECT_NE(Normalize(text, options).scalars.blob, text.scalars.blob);
  // Without the suppression the invalid bytes must survive, or the fuzzer
  // could never find D1.
  EXPECT_EQ(Normalize(text, ValueBearing()).scalars.blob, text.scalars.blob);

  // bytes is not a string; it must never be rewritten.
  Node raw_bytes;
  raw_bytes.kind = Kind::kBytes;
  raw_bytes.scalars.blob = "\xff\xfe";
  EXPECT_EQ(Normalize(raw_bytes, options).scalars.blob, raw_bytes.scalars.blob);
}

}  // namespace
}  // namespace security::avro_fuzz
