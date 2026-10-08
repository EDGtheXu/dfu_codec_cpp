// Proves the single header is self-contained.
//
// `codec.hpp` is included before every other header in this translation unit (the
// other unit suites pull in GoogleTest first, via test_support.hpp), so the header
// has to compile with no help from anything else.  It also deliberately avoids the
// test helpers, so nothing but codec.hpp is required.
#include "codec_json.hpp"

#include <string>

#include <gtest/gtest.h>

namespace {

struct Named {
  std::string name;
  int32_t count = 0;

  bool operator==(const Named& other) const {
    return name == other.name && count == other.count;
  }
};

codec::Codec<Named> namedCodec() {
  static const codec::Codec<Named> codec = codec::record<Named>(
      codec::fieldOf("name", &Named::name, codec::codecs::String),
      codec::fieldOf("count", &Named::count, codec::codecs::Int));
  return codec;
}

TEST(HeaderSelfContainedTest, CompilesAndWorksWithOnlyTheSingleHeader) {
  const codec::DataResult<Named> decoded = namedCodec().parse(
      codec::JsonOps::INSTANCE, codec::JsonValue::parse(R"({"name":"a","count":2})"));
  ASSERT_TRUE(decoded.result().has_value()) << decoded.message();
  EXPECT_EQ(*decoded.result(), (Named{"a", 2}));

  const codec::DataResult<codec::Value> encoded =
      namedCodec().encodeStart(codec::JsonOps::INSTANCE, *decoded.result());
  ASSERT_TRUE(encoded.result().has_value()) << encoded.message();
  EXPECT_EQ(jsonView(*encoded.result()).dump(), R"({"name":"a","count":2})");

  // The error path works too, without any test-helper machinery.
  const codec::DataResult<Named> failed = namedCodec().parse(
      codec::JsonOps::INSTANCE, codec::JsonValue::parse(R"({"name":1,"count":2})"));
  ASSERT_TRUE(failed.isError());
  EXPECT_EQ(failed.message(), "Not a string: 1");
}

}  // namespace
