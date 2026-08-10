#ifndef SECURITY_AVRO_AVRO_COMPARE_H_
#define SECURITY_AVRO_AVRO_COMPARE_H_

#include <cstddef>
#include <string>

#include "absl/types/span.h"
#include "avro_bridge.h"

#include <avro/DataFile.hh>
#include <avro/ValidSchema.hh>

// Cross-implementation comparison, shared so the benchmark's validation
// circle, the dual-run compat shim and the differential fuzzers cannot drift
// apart on what "the same value" means. See doc/specs/AvrocppMigration.md.
namespace security::avro_compare {

// Deep value equality, recursing into records, arrays, maps and unions. A
// mismatched shape returns false rather than failing.
//
// Looser than the bridge's operator== in one respect: Value::String and
// Value::Bytes holding the same bytes are equal, because D1 decodes an
// invalid-UTF-8 `string` to Bytes and a correct decode of such a field would
// otherwise read as a divergence. See doc/specs/AvroStringPolicy.md.
bool ValuesEqual(const security::avro::AvroValue& a,
                 const security::avro::AvroValue& b);

// Names what diverged, so a shim or fuzzer can log a reproducer rather than
// abort the way the benchmark does.
struct CompareResult {
  // Empty when every check below passed. Otherwise names the first point
  // of disagreement, e.g. "value 42 differs after the cross-read circle"
  // or "avrocpp read a different value count from our file".
  std::string diverged_at;

  bool ok() const { return diverged_at.empty(); }

  static CompareResult Ok() { return CompareResult{}; }
  static CompareResult Diverged(std::string where) {
    return CompareResult{std::move(where)};
  }
};

// The cross-read circle: our writer produces a container file from
// `values` (validated against `ours_schema`, written with `ours_codec`);
// avrocpp reads that file and re-writes it against `cpp_schema` with
// `cpp_codec`; we then read the result back two ways -- once whole-buffer,
// once through a stream fed in odd-sized pieces, to hit arbitrary split
// points the way a transport would -- and compare every value read
// back against `values`. Returns the first divergence found rather than
// continuing past it, so a caller gets one reproducible complaint instead
// of a pile of them.
//
// Never aborts: everything this checks failing is data for the caller to
// act on (a benchmark aborts on it, a dual-run shim logs it and falls back
// to avrocpp's answer), not a bug in this function.
CompareResult CrossReadCircle(
    const security::avro::AvroSchema& ours_schema,
    const ::avro::ValidSchema& cpp_schema,
    absl::Span<const security::avro::AvroValue> values,
    security::avro::Codec ours_codec, ::avro::Codec cpp_codec);

}  // namespace security::avro_compare

#endif  // SECURITY_AVRO_AVRO_COMPARE_H_
