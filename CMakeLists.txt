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
