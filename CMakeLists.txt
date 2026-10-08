cmake_minimum_required(VERSION 3.16)
project(dfu_codec_cpp VERSION 1.0.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# A C++17 port of Mojang's DataFixerUpper `com.mojang.serialization` Codec API.
# Header only library; the only build dependency is GoogleTest (tests).
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(CODEC_BUILD_TESTS "Build the GoogleTest unit test suite" ON)
option(CODEC_BUILD_EXAMPLES "Build the example programs" ON)
option(CODEC_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(CODEC_RECORD_STACKTRACE
  "Capture a std::stacktrace for every error (needs C++23; file:line needs debug info)" OFF)
option(CODEC_BUILD_PROTOTYPE
  "Build the generic-ops design prototype (experiment, not part of the library)" OFF)
option(CODEC_BUILD_TOML
  "Build the optional TOML layer (include/codec_toml.hpp) and its tests; needs third_party/tinytoml" ON)

# ---------------------------------------------------------------------------
# Header-only interface library
# ---------------------------------------------------------------------------
add_library(codec INTERFACE)
add_library(codec::codec ALIAS codec)
target_include_directories(codec INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  # nlohmann/json single header (<nlohmann/json.hpp>)
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
)
target_compile_features(codec INTERFACE cxx_std_17)

# ---------------------------------------------------------------------------
# The optional TOML layer: include/codec_toml.hpp (+ tinytoml).
# The core (include/codec.hpp) never depends on tinytoml; this target only adds
# the include directory <toml/toml.h> needs.
# ---------------------------------------------------------------------------
if(CODEC_BUILD_TOML)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tinytoml/toml/toml.h")
    message(FATAL_ERROR
      "tinytoml not found at third_party/tinytoml/toml/toml.h.\n"
      "Download it, or configure with -DCODEC_BUILD_TOML=OFF.")
  endif()
  add_library(codec_toml INTERFACE)
  target_link_libraries(codec_toml INTERFACE codec)
  target_include_directories(codec_toml INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tinytoml")
endif()

# Optional: record a real C++ stack trace with the standard library's
# <stacktrace> (C++23).  Off by default because capturing costs on every error;
# the header still compiles as C++17 when it is off (see include/codec.hpp).
if(CODEC_RECORD_STACKTRACE)
  target_compile_definitions(codec INTERFACE CODEC_RECORD_STACKTRACE)
  if(MSVC)
    # /std:c++latest for <stacktrace>; /bigobj because the C++23 instantiation
    # count of this header exceeds the default section limit.
    target_compile_options(codec INTERFACE /std:c++latest /bigobj)
  else()
    target_compile_options(codec INTERFACE -std=c++23)
  endif()
endif()

if(MSVC)
  target_compile_options(codec INTERFACE /utf-8 /W4 /permissive- /Zc:__cplusplus)
  if(CODEC_WARNINGS_AS_ERRORS)
    target_compile_options(codec INTERFACE /WX)
  endif()
else()
  target_compile_options(codec INTERFACE -Wall -Wextra -Wpedantic)
  if(CODEC_WARNINGS_AS_ERRORS)
    target_compile_options(codec INTERFACE -Werror)
  endif()
endif()

# ---------------------------------------------------------------------------
# Third-party dependencies
#   * nlohmann/json (header only) -- the JSON parser/serializer/DOM backing
#     JsonValue; see scripts/fetch_deps.ps1
#   * GoogleTest -- only needed for the unit tests
# ---------------------------------------------------------------------------
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann/json.hpp")
  message(FATAL_ERROR
    "nlohmann/json not found at third_party/nlohmann/json.hpp.\n"
    "Download it with: powershell -File scripts/fetch_deps.ps1")
endif()

# ---------------------------------------------------------------------------
# Tests -- layered: test/unit (one executable) and test/smoke (one executable),
# both registered with CTest.
# ---------------------------------------------------------------------------
if(CODEC_BUILD_TESTS)
  set(CODEC_GTEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/googletest-1.17.0")
  if(NOT EXISTS "${CODEC_GTEST_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
      "GoogleTest not found at ${CODEC_GTEST_DIR}.\n"
      "Download it with: pwsh -File scripts/fetch_deps.ps1")
  endif()

  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  add_subdirectory("${CODEC_GTEST_DIR}" "${CMAKE_BINARY_DIR}/googletest" EXCLUDE_FROM_ALL)

  enable_testing()
  add_subdirectory(test)
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(CODEC_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# ---------------------------------------------------------------------------
# Design prototypes (off by default; they are experiments, not the library).
#   prototype/ -- generic-ops (DynamicOps<T>) feasibility + microbenchmarks
# ---------------------------------------------------------------------------
if(CODEC_BUILD_PROTOTYPE)
  add_subdirectory(prototype)
endif()
