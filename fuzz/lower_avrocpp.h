#ifndef SECURITY_AVRO_FUZZ_LOWER_AVROCPP_H_
#define SECURITY_AVRO_FUZZ_LOWER_AVROCPP_H_

#include <string>

#include "avro/GenericDatum.hh"
#include "avro/Node.hh"
#include "avro/ValidSchema.hh"
#include "fuzz/ir.h"

// Builds an avro-cpp `GenericDatum` from an IR tree.
//
// Depends on avro-cpp and never on avro_bridge.h, the mirror of lower_bridge.
namespace security::avro_fuzz {

// How an avro-cpp call ended. avro-cpp signals failure by throwing, so every
// call into it is funnelled through CallAvrocpp and turned into one of these:
// a thrown exception is a *verdict* to compare against the bridge's status, not
// a crash for the fuzzer to report.
enum class CppVerdict {
  kOk,
  kAvroException,      // avro::Exception
  kOutOfRange,         // std::out_of_range -- D5's distinguishing signal
  kOtherStdException,
};

struct CppOutcome {
  CppVerdict verdict = CppVerdict::kOk;
  std::string what;
  bool ok() const { return verdict == CppVerdict::kOk; }
};

// Runs `body`, converting avro-cpp's documented failure modes into a verdict.
// `site` names the call for the message, e.g. "compileJsonSchemaFromMemory".
//
// std::out_of_range is caught separately from std::exception even though it
// derives from it: avro-cpp throws it (not avro::Exception) for an
// out-of-range union branch, and one property asserts on that exact type.
//
// There is deliberately no catch(...): a forced unwind, or a Rust panic
// crossing this frame, must terminate rather than be laundered into a verdict.
template <typename F>
CppOutcome CallAvrocpp(const char* site, F&& body);

// Builds the datum for `node` against `schema`.
//
// Takes a NodePtr rather than a ValidSchema because the recursion has to
// descend into subschemas, and a ValidSchema exists only at the root.
CppOutcome ToAvrocppDatum(const Node& node, const ::avro::NodePtr& schema,
                          ::avro::GenericDatum* out);

CppOutcome ToAvrocppDatum(const Node& node, const ::avro::ValidSchema& schema,
                          ::avro::GenericDatum* out);

}  // namespace security::avro_fuzz

// ---------------------------------------------------------------------------

#include "avro/Exception.hh"

namespace security::avro_fuzz {

template <typename F>
CppOutcome CallAvrocpp(const char* site, F&& body) {
  try {
    body();
    return {};
  } catch (const ::avro::Exception& e) {
    return {CppVerdict::kAvroException, std::string(site) + ": " + e.what()};
  } catch (const std::out_of_range& e) {
    return {CppVerdict::kOutOfRange, std::string(site) + ": " + e.what()};
  } catch (const std::exception& e) {
    return {CppVerdict::kOtherStdException, std::string(site) + ": " + e.what()};
  }
}

}  // namespace security::avro_fuzz

#endif  // SECURITY_AVRO_FUZZ_LOWER_AVROCPP_H_
