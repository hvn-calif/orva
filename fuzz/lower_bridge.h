#ifndef SECURITY_AVRO_FUZZ_LOWER_BRIDGE_H_
#define SECURITY_AVRO_FUZZ_LOWER_BRIDGE_H_

#include <string>

#include "absl/status/statusor.h"
#include "avro_bridge.h"
#include "fuzz/ir.h"
#include "fuzz/suppress.h"

// Builds a bridge `AvroValue` from an IR tree.
//
// Depends on `avro_bridge.h` and never on avro-cpp, so a target linking only
// this half produces sanitizer reports that are unambiguously about the
// binding under test.
namespace security::avro_fuzz {

// Two of the bridge's value constructors reject inputs avro-cpp accepts, and
// that asymmetry is the finding, not an obstacle. `log` records it and the
// lowering fails; the caller asserts on the log rather than skipping the input.
//
//   D1  CreateString rejects non-UTF-8 bytes; avro-cpp stores them verbatim.
//   D2  a map collapses duplicate keys last-write-wins; avro-cpp keeps both.
absl::StatusOr<security::avro::AvroValue> ToBridgeValue(const Node& node,
                                                        FindingLog* log);

}  // namespace security::avro_fuzz

#endif  // SECURITY_AVRO_FUZZ_LOWER_BRIDGE_H_
