#ifndef SECURITY_AVRO_FUZZ_DUMP_H_
#define SECURITY_AVRO_FUZZ_DUMP_H_

#include <string>

#include "avro/GenericDatum.hh"
#include "avro_bridge.h"

// Renders a decoded value as canonical text, one function per engine.
//
// This is the second of two value oracles. `CompareValues` in compare.h walks
// the two values in lockstep, which gives a precise per-node message but drives
// the traversal from avro-cpp's type switch: a bridge value that is unreadable
// through its own accessor, or that carries structure avro-cpp does not have,
// can pass that walk. Each function here knows one engine only and reports what
// it actually read, including read failures, so that class of difference shows
// up as a mismatch instead of as silence.
//
// The two renderings are compared as strings, so every formatting rule is a
// normalisation decision. The rules, and why each one is what it is:
//
//   - Every byte payload prints as hex. A string holding invalid UTF-8 and a
//     map key holding a newline both have to survive being put in a message.
//   - Floats print their raw bits. Avro binary carries IEEE-754 bits, so a NaN
//     payload and the sign of zero are differences worth seeing, and decimal
//     text formatting would hide both.
//   - Map entries print in key order. Avro fixes no order for them and the two
//     engines choose differently; the bridge already returns keys sorted, so
//     only the avro-cpp side needs sorting.
//   - Logical types print the payload underneath them and no annotation.
//     avro-cpp 1.11.4 has no TIMESTAMP_NANOS and no LOCAL_TIMESTAMP_*, so
//     printing the annotation would report a version gap as a value
//     difference. Whether the two engines agree on the *type* is the walker's
//     job, and it has an ID for the version gap already.
//   - decimal prints as `dec:` on both sides rather than as its backing bytes
//     or fixed, because the bridge exposes a decimal through one accessor
//     whichever of the two backs it, so the backing type is not observable
//     there.
//   - duration prints as three integers. The bridge exposes months, days and
//     millis; avro-cpp exposes the twelve fixed bytes they were read from.
//
// A payload the bridge reports as present but will not return prints
// `<unreadable>`, which is the point of having this oracle at all: a value a
// caller can neither read nor forward is a defect even when both engines agree
// that the bytes were decodable.
namespace security::avro_fuzz {

// Depth at which both functions stop and print `<depth>`. Recursive schemas
// decode to finite values, so this is a guard against a stack overflow under
// AddressSanitizer rather than something the fixed schema list can reach.
constexpr int kMaxDumpDepth = 64;

struct DumpOptions {
  // Render `string` and `bytes` under one tag, so a payload that arrives as
  // bytes on one side and as a string on the other is not a difference.
  //
  // Needed by any binary that enables SetNonUtf8StringAsBytes: there a string
  // whose wire bytes are not valid UTF-8 decodes to bytes on the bridge side by
  // design, and avro-cpp still calls it a string. It is the same knob as
  // CompareOptions::allow_string_as_bytes and has to be set the same way, or
  // the two oracles are answering different questions and cannot be compared.
  //
  // The cost is that a real string-versus-bytes divergence becomes invisible
  // here. That is the configuration's choice, not this function's: the
  // distinction the patch erases cannot be observed by either oracle.
  bool string_as_bytes = false;
};

std::string DumpBridgeValue(const security::avro::AvroValue& value,
                            const DumpOptions& options = {});
std::string DumpAvrocppDatum(const ::avro::GenericDatum& datum,
                             const DumpOptions& options = {});

}  // namespace security::avro_fuzz

#endif  // SECURITY_AVRO_FUZZ_DUMP_H_
