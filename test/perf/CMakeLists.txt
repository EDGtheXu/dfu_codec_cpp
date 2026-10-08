# Layer 3 -- performance comparison.
#
# Measures the Codec layer against nlohmann/json used directly and prints a
# table.  The only assertions are equivalence ones (all strategies must agree),
# so the layer is safe to run in CI; read the numbers by running the executable
# directly or `ctest -R perf -V`.
add_executable(codec_perf_tests
  codec_perf_test.cpp
)

codec_configure_test(codec_perf_tests)

gtest_discover_tests(codec_perf_tests
  TEST_PREFIX "perf."
  DISCOVERY_TIMEOUT 60
)
