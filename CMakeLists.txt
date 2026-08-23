cmake_minimum_required(VERSION 3.22)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)

project(avro
  VERSION 0.1.0
  LANGUAGES CXX)

# This tree previously configured with no build type, which on Make plus clang
# means neither -O nor -g. Fuzzing wants -O for throughput and -g for
# symbolised reports.
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(AVRO_BUILD_BENCHMARKS "Build the avrocpp comparison benchmark" OFF)
option(AVRO_BUILD_FUZZERS "Build the FuzzTest differential fuzz targets" OFF)
# Declared here as well as by FuzzTest itself, because the fuzzing flags below
# have to be set before any FetchContent_MakeAvailable and FuzzTest has not
# been fetched yet at that point. A second option() with the same name is a
# no-op.
option(FUZZTEST_FUZZING_MODE "Build fuzz targets with coverage instrumentation" OFF)

set(AVRO_CPP_TAG "release-1.11.4" CACHE STRING
    "Apache avro-cpp git tag to compare against")

# Fuzzing-mode flags, split deliberately.
#
# The AddressSanitizer half must be global and must be set before anything is
# fetched: abseil's InlinedVector, FixedArray and Cord poison container tails
# under ABSL_HAVE_ADDRESS_SANITIZER, so translation units that disagree about
# whether ASan is on violate the ODR and produce bogus container-overflow
# reports. add_subdirectory snapshots CMAKE_CXX_FLAGS at call time, which is
# why this cannot wait for fuzztest_setup_fuzzing_flags().
#
# The coverage half is applied per target instead, so the counter map covers
# avro_bridge and the harnesses rather than all of abseil and GoogleTest.
# FuzzTest already compiles itself with -fsanitize-coverage=0.
if (AVRO_BUILD_FUZZERS AND FUZZTEST_FUZZING_MODE)
  add_compile_options(-g -DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION -UNDEBUG
                      -fsanitize=address -DADDRESS_SANITIZER
                      -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
  set(AVRO_FUZZ_COVERAGE_FLAGS
      -fsanitize-coverage=inline-8bit-counters
      -fsanitize-coverage=trace-cmp)
endif()

include(FetchContent)

# ---- Abseil ---------------------------------------------------------------
# Three consumers want abseil under two different FetchContent names: FuzzTest
# declares `abseil-cpp`, while Crubit (pulled in by corrosion_import_crate)
# declares `absl`. FetchContent de-duplicates by name, so two names mean two
# clones and two add_subdirectory calls, which fails on duplicate absl_* targets.
# No declaration order fixes that.
#
# So own the `abseil-cpp` name here, pinned to what FuzzTest pins
# (cmake/BuildDependencies.cmake), and answer Crubit's find_package(absl)
# through the find_package redirect directory rather than a second clone.
FetchContent_Declare(
  abseil-cpp
  GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
  GIT_TAG 20260526.0
  GIT_SHALLOW TRUE
)
set(ABSL_PROPAGATE_CXX_STD ON)
set(ABSL_ENABLE_INSTALL ON)
FetchContent_MakeAvailable(abseil-cpp)

# Crubit's declaration carries FIND_PACKAGE_ARGS NAMES absl, so an empty config
# package here makes its find_package(absl) succeed against the targets added
# above. CMAKE_FIND_PACKAGE_REDIRECTS_DIR is wiped and recreated on every
# configure, so this must be rewritten each run.
file(WRITE "${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/absl-config.cmake" "")
file(WRITE "${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/absl-config-version.cmake"
  "set(PACKAGE_VERSION 20260526.0)\nset(PACKAGE_VERSION_COMPATIBLE TRUE)\nset(PACKAGE_VERSION_EXACT TRUE)\n")

# ---- Crubit ---------------------------------------------------------------
# Corrosion declares Crubit with GIT_TAG latest, a moving target. It moved on
# 2026-08-16 and broke this build outright, so pin it and bump deliberately.
FetchContent_Declare(
  Crubit
  GIT_REPOSITORY https://github.com/google/crubit.git
  GIT_TAG 0e0a26dc95c4f2b190831dcecf950e480d5413f9
)

FetchContent_Declare(
  Corrosion
  # NOTE: Replace with https://github.com/corrosion-rs/corrosion once
  # http://github.com/corrosion-rs/corrosion/pull/666 is merged.
  GIT_REPOSITORY https://github.com/thunderseethe/corrosion
  GIT_TAG master
)

FetchContent_MakeAvailable(Corrosion)

corrosion_experimental_crubit(rust)
corrosion_import_crate(MANIFEST_PATH rust/Cargo.toml)

add_library(avro_bridge avro_bridge.cc avro_bridge.h)
target_link_libraries(avro_bridge PUBLIC
  absl::status
  absl::statusor
  absl::strings
  absl::span
)
target_link_libraries(avro_bridge PRIVATE
  rust
)
if (AVRO_FUZZ_COVERAGE_FLAGS)
  target_compile_options(avro_bridge PRIVATE ${AVRO_FUZZ_COVERAGE_FLAGS})
endif()

add_executable(avro_example main.cc)
target_link_libraries(avro_example PRIVATE
  avro_bridge
  rust
)

# Tests. Pinned to the version FuzzTest is tested against; this declaration
# wins over FuzzTest's identically-named one, so keeping the two in sync is on
# us. FIND_PACKAGE_ARGS is deliberately absent: a system GTest older than 1.17
# would break FuzzTest.
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(googletest)

enable_testing()
include(GoogleTest)

# Every setting avro_bridge.h exposes is a process-global that only the first
# call sets, so one process can observe only one value of each. The same source
# is therefore built twice: once at the bridge's avrocpp-compatible defaults,
# and once with every setting flipped to its strict reading. Without the second
# binary the strict half of each setting is untested, which is why it used to
# need a Rust integration test of its own.
add_executable(avro_bridge_test avro_bridge_test.cc)
add_executable(avro_bridge_strict_test avro_bridge_test.cc)
target_compile_definitions(avro_bridge_strict_test PRIVATE
  AVRO_BRIDGE_TEST_STRICT_SETTINGS)

foreach(target avro_bridge_test avro_bridge_strict_test)
  target_link_libraries(${target} PRIVATE
    avro_bridge
    rust
    GTest::gmock
    GTest::gtest_main
  )
endforeach()

gtest_discover_tests(avro_bridge_test)
# Both binaries register the same gtest names, so the ctest names would collide
# without a prefix.
gtest_discover_tests(avro_bridge_strict_test TEST_PREFIX strict.)

# ---- Apache avro-cpp ------------------------------------------------------
# The benchmark and the differential fuzzer both need it, so acquisition is
# shared rather than living under the benchmark option alone.
if (AVRO_BUILD_BENCHMARKS OR AVRO_BUILD_FUZZERS)
  # EXCLUDE_FROM_ALL because avro-cpp 1.11.4 defines its own test executables
  # unconditionally -- there is no BUILD_TESTING guard -- and one of them,
  # AvrogencppTestReservedWords, does not compile under clang 21: avrogencpp
  # emits a generated header that uses `constinit` where clang rejects it. We
  # only ever want the library, and our targets link avrocpp_s, so it is still
  # built on demand.
  FetchContent_Declare(
    avrocpp_upstream
    GIT_REPOSITORY https://github.com/apache/avro.git
    GIT_TAG ${AVRO_CPP_TAG}
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR lang/c++
    EXCLUDE_FROM_ALL
  )
  # avro-cpp's find_package(Snappy) locates its own FindSnappy.cmake through
  # CMAKE_SOURCE_DIR, which under FetchContent is this repo, so detection
  # silently fails and the snappy codec is dropped. Preset the variables its
  # CMakeLists checks.
  find_path(SNAPPY_INCLUDE_DIR NAMES snappy.h)
  find_library(SNAPPY_LIBRARIES NAMES snappy)
  if (SNAPPY_INCLUDE_DIR AND SNAPPY_LIBRARIES)
    set(SNAPPY_FOUND TRUE)
  else()
    message(FATAL_ERROR "snappy-devel is required to build against avro-cpp")
  endif()
  FetchContent_MakeAvailable(avrocpp_upstream)

  # avro-cpp keeps its public headers in api/ and only maps them to the
  # installed <avro/...> layout at install time; recreate that layout so
  # consumers include <avro/DataFile.hh> exactly as an external consumer would.
  file(COPY ${avrocpp_upstream_SOURCE_DIR}/lang/c++/api/
       DESTINATION ${CMAKE_BINARY_DIR}/avrocpp_include/avro)

  find_package(Boost REQUIRED COMPONENTS iostreams)

  # One place for everything an avrocpp consumer needs. avrocpp_s does not
  # propagate its own link interface, so Boost and snappy are named here rather
  # than repeated at each consumer.
  add_library(avrocpp_iface INTERFACE)
  target_include_directories(avrocpp_iface INTERFACE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/avrocpp_include)
  target_link_libraries(avrocpp_iface INTERFACE
    avrocpp_s
    Boost::iostreams
    ${SNAPPY_LIBRARIES})
endif()

# ---- FuzzTest -------------------------------------------------------------
if (AVRO_BUILD_FUZZERS)
  # Pinned rather than tracking main: FuzzTest pins abseil, GoogleTest and RE2,
  # and the pins above have to stay in sync with it. Bump the three together.
  FetchContent_Declare(
    fuzztest
    GIT_REPOSITORY https://github.com/google/fuzztest.git
    GIT_TAG 810d993fa2c8fe02650d8e2388246a5b67341022
  )
  set(FUZZTEST_BUILD_TESTING OFF)
  set(FUZZTEST_BUILD_FLATBUFFERS OFF)
  FetchContent_MakeAvailable(fuzztest)

  # FuzzTest's CMakeLists hard-sets CMAKE_CXX_STANDARD 17 in its own directory
  # scope. This project is C++20 and both sides include the same abseil
  # headers, whose contents depend on the language standard, so pull its
  # targets up to one standard for the whole program.
  get_property(_ft_dirs DIRECTORY "${fuzztest_SOURCE_DIR}" PROPERTY SUBDIRECTORIES)
  list(APPEND _ft_dirs "${fuzztest_SOURCE_DIR}")
  foreach(_dir IN LISTS _ft_dirs)
    get_property(_targets DIRECTORY "${_dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_target IN LISTS _targets)
      get_target_property(_type ${_target} TYPE)
      if (NOT _type STREQUAL "INTERFACE_LIBRARY")
        set_property(TARGET ${_target} PROPERTY CXX_STANDARD 20)
      endif()
    endforeach()
  endforeach()

  # fuzz/ first: the byte-oriented fuzzer below links avro_fuzz_compare, which
  # is defined there.
  add_subdirectory(fuzz)

  # Byte-oriented differential fuzzer. Links the two engines and, for the value
  # comparison, the two oracles in fuzz/.
  if (TARGET avrocpp_iface)
    add_executable(avro_bytes_fuzz_test avro_bytes_fuzz_test.cc)
    target_include_directories(avro_bytes_fuzz_test PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(avro_bytes_fuzz_test PRIVATE
      avro_bridge rust avrocpp_iface avro_fuzz_compare)
    link_fuzztest(avro_bytes_fuzz_test)
    target_compile_options(avro_bytes_fuzz_test PRIVATE
      ${AVRO_FUZZ_COVERAGE_FLAGS})
    if (FUZZTEST_FUZZING_MODE)
      target_compile_options(avro_bytes_fuzz_test PRIVATE
        -fsanitize=undefined -fno-sanitize-recover=undefined -fno-sanitize=vptr)
      target_link_options(avro_bytes_fuzz_test PRIVATE -fsanitize=undefined)
    endif()
    gtest_discover_tests(avro_bytes_fuzz_test
      DISCOVERY_MODE PRE_TEST
      PROPERTIES TIMEOUT 300)
  endif()
endif()
