cmake_minimum_required(VERSION 3.22)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)

project(avro
  VERSION 0.1.0
  LANGUAGES CXX)

include(FetchContent)

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

FetchContent_Declare(
  absl
  GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
  FIND_PACKAGE_ARGS NAMES absl
)
FetchContent_MakeAvailable(absl)

add_library(avro_bridge avro_bridge.cc avro_bridge.h)
# Crubit's runtime mutable-aliasing guards call a function defined in a .cc
# that its generated INTERFACE support library never compiles; build it into
# the bridge so the generated thunks link.
FetchContent_GetProperties(Crubit)
target_sources(avro_bridge PRIVATE
  ${crubit_SOURCE_DIR}/support/internal/check_no_mutable_aliasing.cc)
target_link_libraries(avro_bridge PUBLIC
  absl::status
  absl::statusor
  absl::strings
  absl::span
)
target_link_libraries(avro_bridge PRIVATE
  rust
)

add_executable(avro_example main.cc)
target_link_libraries(avro_example PRIVATE
  avro_bridge
  rust
)

# Tests.
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
  FIND_PACKAGE_ARGS NAMES GTest
)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_executable(avro_bridge_test avro_bridge_test.cc)
target_link_libraries(avro_bridge_test PRIVATE
  avro_bridge
  rust
  GTest::gmock
  GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(avro_bridge_test)

# Benchmark comparing this binding with Apache avro-cpp. Opt-in because it
# fetches and builds avro-cpp (which needs Boost) and google-benchmark; see
# doc/specs/AvroBenchmark.md and benchmarks/README.md.
option(AVRO_BUILD_BENCHMARKS "Build the avrocpp comparison benchmark" OFF)
if (AVRO_BUILD_BENCHMARKS)
  FetchContent_Declare(
    googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.1
    GIT_SHALLOW TRUE
  )
  set(BENCHMARK_ENABLE_TESTING OFF)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF)
  FetchContent_MakeAvailable(googlebenchmark)

  FetchContent_Declare(
    avrocpp_upstream
    GIT_REPOSITORY https://github.com/apache/avro.git
    GIT_TAG release-1.11.4
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR lang/c++
  )
  # avro-cpp's find_package(Snappy) depends on its own FindSnappy.cmake
  # located via CMAKE_SOURCE_DIR, which under FetchContent is this repo,
  # so detection silently fails and the snappy codec is dropped. Preset
  # the result variables its CMakeLists checks (same probes FindSnappy
  # would run) before it configures.
  find_path(SNAPPY_INCLUDE_DIR NAMES snappy.h)
  find_library(SNAPPY_LIBRARIES NAMES snappy)
  if (SNAPPY_INCLUDE_DIR AND SNAPPY_LIBRARIES)
    set(SNAPPY_FOUND TRUE)
  else()
    message(FATAL_ERROR "snappy-devel is required for the benchmark's "
            "snappy rows; see benchmarks/README.md")
  endif()
  FetchContent_MakeAvailable(avrocpp_upstream)

  # avro-cpp's build tree keeps its public headers in api/ and only maps
  # them to the installed <avro/...> layout at install time; recreate that
  # layout in the build dir so the benchmark includes <avro/DataFile.hh>
  # exactly like an external consumer would.
  file(COPY ${avrocpp_upstream_SOURCE_DIR}/lang/c++/api/
       DESTINATION ${CMAKE_BINARY_DIR}/avrocpp_include/avro)

  # avrocpp_s does not propagate its own link interface; supply the Boost
  # pieces and snappy it depends on. Needed by avro_compare below and by
  # avro_benchmark itself.
  find_package(Boost REQUIRED COMPONENTS iostreams)

  # Cross-implementation comparison shared by the benchmark's validation
  # circle below and, per doc/specs/AvrocppMigration.md, by the future
  # dual-run compat shim and differential fuzzers once those land. It needs
  # avrocpp, so for now it lives inside this option alongside the benchmark;
  # the migration spec makes avrocpp a non-optional dependency of the shim
  # later, at which point this target can move out from under
  # AVRO_BUILD_BENCHMARKS.
  add_library(avro_compare avro_compare.cc avro_compare.h)
  target_include_directories(avro_compare PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/avrocpp_include)
  target_link_libraries(avro_compare PUBLIC
    avro_bridge
    rust
    avrocpp_s
    Boost::iostreams
    ${SNAPPY_LIBRARIES}
  )

  # avro_compare's normalization branch (String vs Bytes) is only exercised
  # by these tests: none of the benchmark datasets contain invalid UTF-8, so
  # the validation circle above never takes that path. Lives here rather
  # than in avro_bridge_test because it links avrocpp transitively through
  # avro_compare.
  add_executable(avro_compare_test avro_compare_test.cc)
  target_link_libraries(avro_compare_test PRIVATE
    avro_compare
    avro_bridge
    rust
    GTest::gmock
    GTest::gtest_main
  )
  gtest_discover_tests(avro_compare_test)

  add_executable(avro_benchmark benchmarks/avro_benchmark.cc)
  target_include_directories(avro_benchmark PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/avrocpp_include)
  # avro-cpp hides the SNAPPY_CODEC enum value behind this define; the
  # library itself is built with snappy (snappy-devel is a documented
  # prerequisite), consumers must opt in. Cross-validation at benchmark
  # startup catches a snappy-less avrocpp build with a clear error.
  target_compile_definitions(avro_benchmark PRIVATE SNAPPY_CODEC_AVAILABLE)
  target_link_libraries(avro_benchmark PRIVATE
    avro_bridge
    avro_compare
    rust
    avrocpp_s
    Boost::iostreams
    ${SNAPPY_LIBRARIES}
    benchmark::benchmark
  )

  # Probes answering narrower questions than the avro_benchmark matrix:
  #   block_probe   container block-framing cost vs per-value decode cost
  #   avro_sweep    how the read gap scales with payload size and field count
  #   append_probe  what the copy inside Append costs, and what moving saves
  #   access_probe  what reading a decoded value back out costs
  #   manifest_probe where the time goes on a real Iceberg v2 manifest shape
  foreach(probe block_probe avro_sweep append_probe access_probe manifest_probe)
    add_executable(${probe} benchmarks/${probe}.cc)
    target_include_directories(${probe} PRIVATE
      ${CMAKE_SOURCE_DIR}
      ${CMAKE_BINARY_DIR}/avrocpp_include)
    target_link_libraries(${probe} PRIVATE
      avro_bridge
      rust
      avrocpp_s
      Boost::iostreams
      ${SNAPPY_LIBRARIES}
      benchmark::benchmark
    )
  endforeach()
endif()
