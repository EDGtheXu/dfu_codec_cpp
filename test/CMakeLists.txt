# ---------------------------------------------------------------------------
# Test layers
#
#   test/unit/   codec_unit_tests   -- fine grained, one suite per component
#   test/smoke/  codec_smoke_tests  -- small, fast end-to-end checks of the API
#   test/perf/   codec_perf_tests   -- codec vs nlohmann/json measurements
#
# Every layer is a separate executable and registers its cases with CTest, so an
# IDE (CLion) shows them as run configurations and "All CTest" runs everything
# with one click.  `cmake --build . --target check` does the same from the command
# line.
# ---------------------------------------------------------------------------

# Shared compile settings for every test executable.
function(codec_configure_test target)
  target_link_libraries(${target} PRIVATE codec GTest::gtest_main)
  target_include_directories(${target} PRIVATE
    "${CMAKE_SOURCE_DIR}/test/support"
    "${CMAKE_SOURCE_DIR}/models"
  )
  if(MSVC)
    target_compile_options(${target} PRIVATE /utf-8 /W4 /permissive- /bigobj)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra)
  endif()
endfunction()

include(GoogleTest)

# JSON layers: unit, smoke, perf.  They need the JSON entry header (and therefore
# nlohmann/json); with -DCODEC_BUILD_JSON=OFF they are not built at all.
set(CODEC_JSON_TARGETS "")
if(CODEC_BUILD_JSON)
  add_subdirectory(unit)
  add_subdirectory(smoke)
  add_subdirectory(perf)
  set(CODEC_JSON_TARGETS codec_unit_tests codec_smoke_tests codec_perf_tests)
endif()

# Optional TOML layer (include/codec_toml.hpp).  Separate executables so the
# core layers stay free of the tinytoml dependency.
#
# `test/unit/toml_ops_test.cpp` and `test/perf/toml_perf_test.cpp` are JSON-free on
# purpose: they must build and run with -DCODEC_BUILD_JSON=OFF, so that a config
# without any JSON library still has real coverage.  The JSON-dependent assertions
# inside them are wrapped in CODEC_TEST_WITH_JSON, defined below only when the JSON
# layer is built.
set(CODEC_TOML_TARGETS "")
if(CODEC_BUILD_TOML)
  add_executable(codec_toml_unit_tests unit/toml_ops_test.cpp)
  codec_configure_test(codec_toml_unit_tests)
  target_link_libraries(codec_toml_unit_tests PRIVATE codec_toml)
  gtest_discover_tests(codec_toml_unit_tests TEST_PREFIX "toml_unit." DISCOVERY_TIMEOUT 60)

  # The TOML perf layer gets its own executable rather than a fourth case in
  # codec_perf_tests: this keeps the core perf binaries free of the tinytoml
  # dependency and, more importantly, keeps the recorded JSON numbers comparable
  # (adding code to that binary shifts its code layout, which is worth ~3%).
  add_executable(codec_toml_perf_tests perf/toml_perf_test.cpp)
  codec_configure_test(codec_toml_perf_tests)
  target_link_libraries(codec_toml_perf_tests PRIVATE codec_toml)
  gtest_discover_tests(codec_toml_perf_tests TEST_PREFIX "toml_perf." DISCOVERY_TIMEOUT 60)

  set(CODEC_TOML_TARGETS codec_toml_unit_tests codec_toml_perf_tests)

  if(CODEC_BUILD_JSON)
    # Cross-format smoke tests: the same codec reading JSON and TOML, so they need
    # both entry headers (and are therefore only built when both layers are on).
    add_executable(codec_toml_smoke_tests smoke/toml_risk_def_test.cpp)
    codec_configure_test(codec_toml_smoke_tests)
    target_link_libraries(codec_toml_smoke_tests PRIVATE codec_toml codec_json)
    gtest_discover_tests(codec_toml_smoke_tests TEST_PREFIX "toml_smoke." DISCOVERY_TIMEOUT 60)
    list(APPEND CODEC_TOML_TARGETS codec_toml_smoke_tests)
  endif()
endif()

# The JSON-dependent halves of the otherwise JSON-free TOML tests.
if(CODEC_BUILD_JSON)
  foreach(target IN LISTS CODEC_TOML_TARGETS)
    target_compile_definitions(${target} PRIVATE CODEC_TEST_WITH_JSON)
    target_link_libraries(${target} PRIVATE codec_json)
  endforeach()
endif()

# One-click "run every layer" target for the command line and for IDEs.
add_custom_target(check
  COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
  DEPENDS ${CODEC_JSON_TARGETS} ${CODEC_TOML_TARGETS}
  USES_TERMINAL
  COMMENT "Running every configured test layer"
)
