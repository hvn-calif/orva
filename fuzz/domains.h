#ifndef SECURITY_AVRO_FUZZ_DOMAINS_H_
#define SECURITY_AVRO_FUZZ_DOMAINS_H_

#include <string>

#include "fuzz/ir.h"
#include "fuzztest/fuzztest.h"

namespace security::avro_fuzz {

// Depth and breadth caps. Depth is a hard structural bound: AnyNode stacks
// this many finite domain layers rather than recursing, so the generator
// cannot produce a tree deeper than this no matter what the mutator does.
//
// FuzzTest's DomainBuilder would be the obvious tool, but its own reference
// says "Recursion limit for recursive domains is not implemented yet". With an
// unconditionally-recursive children domain the branching factor exceeds one
// and the expected tree size is unbounded, so a deep draw blows the stack
// while *constructing the input*, before any Avro code runs -- which looks
// like a crash in the harness rather than a finding.
inline constexpr int kMaxDepth = 5;
inline constexpr int kMaxBreadth = 3;

// A valid Avro name: [A-Za-z_][A-Za-z0-9_]*.
//
// Weighted toward a tiny pool because collisions are the interesting case and
// a freely generated identifier collides essentially never. Reserved words are
// included because using them as user-defined names is a leniency question the
// divergence register lists as uninvestigated.
fuzztest::Domain<std::string> AnyName();

// Map keys, over a deliberately small alphabet so duplicate keys -- D2 -- show
// up on most maps with more than one entry rather than never.
fuzztest::Domain<std::string> AnyMapKey();

// bytes / string / fixed / decimal / uuid payloads, biased toward invalid
// UTF-8, lone surrogates, embedded NUL and overlong encodings, which is where
// D1 lives.
fuzztest::Domain<std::string> AnyPayload();

// A whole tree, bounded to `max_depth` layers.
fuzztest::Domain<Node> AnyTree(int max_depth = kMaxDepth);

// A tree of leaves only. Useful for properties that want to isolate scalar and
// logical-type behaviour from container framing.
fuzztest::Domain<Node> AnyLeaf();

// A deliberately deep, single-branch chain, for the property that checks both
// engines terminate on deep nesting rather than overflowing the stack. Breadth
// is pinned to one because that is the only shape where deep recursion has
// bounded size.
fuzztest::Domain<Node> AnyDeepChain(int max_depth = 24);

}  // namespace security::avro_fuzz

#endif  // SECURITY_AVRO_FUZZ_DOMAINS_H_
