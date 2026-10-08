# Layer 1 -- unit tests.
#
# One executable covering the individual components of the port (JSON DOM,
# DataResult/Lifecycle, DynamicOps/JsonOps, codec combinators, record codecs,
# dispatch).  Fast, exhaustive on edge cases, no end-to-end scenarios.
add_executable(codec_unit_tests
  data_result_test.cpp
  primitives_test.cpp
  json_test.cpp
  nlohmann_interop_test.cpp
  dynamic_ops_test.cpp
  codec_combinators_test.cpp
  record_codec_test.cpp
  dispatch_test.cpp
  string_and_enum_test.cpp         # JSON scalar <-> enum / number / string cookbook
  dynamic_test.cpp                 # Dynamic (value + its ops): Passthrough carrier
  error_path_test.cpp              # error locations: risks[3].condition.or[0].op
  stacktrace_test.cpp              # optional std::stacktrace layer (CODEC_RECORD_STACKTRACE)
  header_self_contained_test.cpp   # includes codec_json.hpp before anything else
  odr_test.cpp
  odr_probe.cpp          # second TU: guards the header-only property
)

codec_configure_test(codec_unit_tests)
target_link_libraries(codec_unit_tests PRIVATE codec_json)

gtest_discover_tests(codec_unit_tests
  TEST_PREFIX "unit."
  DISCOVERY_TIMEOUT 60
)
