#ifndef SECURITY_AVRO_FUZZ_COMPARE_H_
#define SECURITY_AVRO_FUZZ_COMPARE_H_

#include <string>

#include "avro/GenericDatum.hh"
#include "avro_bridge.h"
#include "fuzz/suppress.h"

// Compares a bridge value against an avro-cpp datum.
//
// Deliberately stricter than the `avro_compare::ValuesEqual` that lands on main
// twelve commits later. That one treats a String and a Bytes holding the same
// payload as equal, because by then the projected decoder returns Bytes for
// invalid UTF-8. Importing that leniency here would mask D1 -- the very bug
// this baseline exists to rediscover.
namespace security::avro_fuzz {

struct CompareOptions {
  // Compare float and double bit-for-bit, so a NaN payload is significant and
  // -0.0 differs from +0.0.
  //
  // This is the right oracle for a codec: Avro binary carries raw IEEE-754
  // bits, and canonicalisation by either engine is a real finding. Note the
  // bridge's own operator== cannot be used for floats at all -- it delegates
  // to Rust PartialEq, where NaN != NaN would report a spurious difference and
  // -0.0 == 0.0 would hide a real one.
  bool strict_float_bits = true;

  // avro-cpp 1.11.4's LogicalType enum has no TIMESTAMP_NANOS and no
  // LOCAL_TIMESTAMP_*, so it ignores those annotations and yields a plain
  // long. When true, that is accepted as a version gap and the underlying
  // integers are compared instead of reporting a type mismatch.
  bool allow_missing_logical_types = true;

  // Accept a bridge `bytes` where avro-cpp holds a `string`, comparing the
  // payloads instead of reporting a type mismatch.
  //
  // Off by default, and the default is the point: treating String and Bytes as
  // interchangeable is exactly how the non-UTF-8-string divergence would be
  // hidden, which is what this comparer exists to rediscover.
  //
  // Turn it on only in a binary that enables SetNonUtf8StringAsBytes, where a
  // string whose wire bytes are not valid UTF-8 decodes to bytes by design.
  // There, every such input would otherwise report a type mismatch and bury
  // everything else.
  bool allow_string_as_bytes = false;
};

// Walks both values in lockstep, reporting differences into `log`. Returns true
// when no unsuppressed difference was found.
//
// The traversal order is fixed, so which difference is reported first is
// reproducible -- a requirement for FuzzTest reproducers to mean anything.
//
// The walk stops at the first difference that proves the two decoders consumed a
// different number of input bytes: a collection whose sizes disagree, or a slot
// avro-cpp reserved and never read. Past that point the two sides are reading
// different offsets of the same buffer, so a difference is not attributable to
// either engine. Reporting them anyway buried one suppressed array-of-null length
// divergence under ten thousand reports about the field that followed it.
bool CompareValues(const security::avro::AvroValue& bridge,
                   const ::avro::GenericDatum& cpp, FindingLog* log,
                   const CompareOptions& options = {});

}  // namespace security::avro_fuzz

#endif  // SECURITY_AVRO_FUZZ_COMPARE_H_
