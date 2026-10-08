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
    target_compile_options(${target} PRIVATE /utf-8 /W4 /permissive-)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra)
  endif()
endfunction()

include(GoogleTest)

add_subdirectory(unit)
add_subdirectory(smoke)
add_subdirectory(perf)

# One-click "run every layer" target for the command line and for IDEs.
add_custom_target(check
  COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
  DEPENDS codec_unit_tests codec_smoke_tests codec_perf_tests
  USES_TERMINAL
  COMMENT "Running the unit, smoke and perf test layers"
)
