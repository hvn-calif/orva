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
