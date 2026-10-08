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

add_subdirectory(unit)
add_subdirectory(smoke)
add_subdirectory(perf)

# Optional TOML layer (include/codec_toml.hpp).  Separate executables so the
# core layers stay free of the tinytoml dependency.
set(CODEC_TOML_TARGETS "")
if(CODEC_BUILD_TOML)
  add_executable(codec_toml_unit_tests unit/toml_ops_test.cpp)
  codec_configure_test(codec_toml_unit_tests)
  target_link_libraries(codec_toml_unit_tests PRIVATE codec_toml)
  gtest_discover_tests(codec_toml_unit_tests TEST_PREFIX "toml_unit." DISCOVERY_TIMEOUT 60)

  add_executable(codec_toml_smoke_tests smoke/toml_risk_def_test.cpp)
  codec_configure_test(codec_toml_smoke_tests)
  target_link_libraries(codec_toml_smoke_tests PRIVATE codec_toml)
  gtest_discover_tests(codec_toml_smoke_tests TEST_PREFIX "toml_smoke." DISCOVERY_TIMEOUT 60)

  # The TOML perf layer gets its own executable rather than a fourth case in
  # codec_perf_tests: this keeps the core perf binaries free of the tinytoml
  # dependency and, more importantly, keeps the recorded JSON numbers comparable
  # (adding code to that binary shifts its code layout, which is worth ~3%).
  add_executable(codec_toml_perf_tests perf/toml_perf_test.cpp)
  codec_configure_test(codec_toml_perf_tests)
  target_link_libraries(codec_toml_perf_tests PRIVATE codec_toml)
  gtest_discover_tests(codec_toml_perf_tests TEST_PREFIX "toml_perf." DISCOVERY_TIMEOUT 60)

  set(CODEC_TOML_TARGETS codec_toml_unit_tests codec_toml_smoke_tests codec_toml_perf_tests)
endif()

# One-click "run every layer" target for the command line and for IDEs.
add_custom_target(check
  COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
  DEPENDS codec_unit_tests codec_smoke_tests codec_perf_tests ${CODEC_TOML_TARGETS}
  USES_TERMINAL
  COMMENT "Running the unit, smoke, perf (and TOML) test layers"
)
