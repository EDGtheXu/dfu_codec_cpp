# Layer 2 -- smoke tests.
#
# One executable covering the public API end to end: the reference
# risk-definition document, one document that uses every codec kind, malformed
# input handling and the compressed/plain JsonOps agreement.  Deliberately small
# and fast -- it answers "does the port work at all?" rather than "is every edge
# case correct?" (that is test/unit's job).
add_executable(codec_smoke_tests
  library_smoke_test.cpp
  risk_def_test.cpp
)

codec_configure_test(codec_smoke_tests)

gtest_discover_tests(codec_smoke_tests
  TEST_PREFIX "smoke."
  DISCOVERY_TIMEOUT 60
)
