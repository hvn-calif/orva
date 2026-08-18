# FuzzTest targets.
#
# In the default configuration these are ordinary gtest binaries: each
# FUZZ_TEST runs for about a second against random inputs, so ctest picks them
# up as regression tests at negligible cost. Configuring with
# -DFUZZTEST_FUZZING_MODE=ON rebuilds them with AddressSanitizer and coverage
# instrumentation for real fuzzing. See README.md.

# Harness code shared by every fuzz target. Deliberately split so that
# lower_bridge never sees avrocpp and lower_avrocpp never sees the bridge:
# that is what lets a bridge-only target exist whose sanitizer reports are
# unambiguously about the bridge rather than about the C++ library we compare
# against.
add_library(avro_fuzz_ir
  ir.cc ir.h
  suppress.cc suppress.h
  lower_schema.cc lower_schema.h
)
target_include_directories(avro_fuzz_ir PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(avro_fuzz_ir PUBLIC absl::strings absl::str_format)
if (AVRO_FUZZ_COVERAGE_FLAGS)
  target_compile_options(avro_fuzz_ir PRIVATE ${AVRO_FUZZ_COVERAGE_FLAGS})
endif()

# The FuzzTest domains. Separate from avro_fuzz_ir so that non-fuzz consumers
# (unit tests, future tooling) can use the IR without linking FuzzTest.
add_library(avro_fuzz_domains domains.cc domains.h)
target_link_libraries(avro_fuzz_domains PUBLIC avro_fuzz_ir)
link_fuzztest(avro_fuzz_domains)
if (AVRO_FUZZ_COVERAGE_FLAGS)
  target_compile_options(avro_fuzz_domains PRIVATE ${AVRO_FUZZ_COVERAGE_FLAGS})
endif()

# Defines one fuzz-test binary.
#   AVROCPP  also link Apache avro-cpp
function(avro_add_fuzz_test name)
  cmake_parse_arguments(FT "AVROCPP" "" "SRCS" ${ARGN})
  add_executable(${name} ${FT_SRCS})
  target_link_libraries(${name} PRIVATE avro_fuzz_ir avro_fuzz_domains)
  if (FT_AVROCPP)
    target_link_libraries(${name} PRIVATE avrocpp_iface)
  endif()
  # link_fuzztest brings in fuzztest_gtest_main, which supplies main(), so
  # GTest::gtest_main must not also be linked.
  link_fuzztest(${name})
  target_compile_options(${name} PRIVATE ${AVRO_FUZZ_COVERAGE_FLAGS})
  # UndefinedBehaviorSanitizer per target rather than globally: avro-cpp 1.11.4
  # and Boost 1.75 headers trip it for reasons unrelated to this binding, and
  # unlike ASan a per-translation-unit UBSan setting is ODR-safe. vptr needs
  # whole-program instrumented RTTI, which this build does not have.
  if (FUZZTEST_FUZZING_MODE)
    target_compile_options(${name} PRIVATE
      -fsanitize=undefined -fno-sanitize-recover=undefined -fno-sanitize=vptr)
    target_link_options(${name} PRIVATE -fsanitize=undefined)
  endif()
  # PRE_TEST discovery: the default POST_BUILD mode runs the freshly linked
  # binary during the build, which under ASan would need ASAN_OPTIONS plumbed
  # into the build environment.
  gtest_discover_tests(${name}
    DISCOVERY_MODE PRE_TEST
    PROPERTIES
      ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0:allocator_may_return_null=1"
      TIMEOUT 300)
endfunction()

# Generator-level properties. Links neither the bridge nor avrocpp, so it is
# the cheapest possible check that the FuzzTest integration works at all.
avro_add_fuzz_test(avro_ir_fuzz_test SRCS ir_test.cc)

# The differential harness. Only this target sees both engines.
if (TARGET avrocpp_iface)
  add_library(avro_fuzz_lower
    lower_bridge.cc lower_bridge.h
    lower_avrocpp.cc lower_avrocpp.h
    compare.cc compare.h
  )
  target_link_libraries(avro_fuzz_lower PUBLIC
    avro_fuzz_ir avro_bridge rust avrocpp_iface)
  if (AVRO_FUZZ_COVERAGE_FLAGS)
    target_compile_options(avro_fuzz_lower PRIVATE ${AVRO_FUZZ_COVERAGE_FLAGS})
  endif()

  avro_add_fuzz_test(avro_differential_fuzz_test AVROCPP
    SRCS differential_test.cc)
  target_link_libraries(avro_differential_fuzz_test PRIVATE avro_fuzz_lower)
endif()
