// Tests for the primitive codecs (Codec.BOOL / INT / LONG / FLOAT / DOUBLE /
// STRING / PASSTHROUGH) and for DynamicOps primitive coercion rules.
#include "test_support.hpp"

namespace {

using codec::DataResult;
using codec::JsonOps;
using codec::JsonValue;
using codec::Value;
using codec::codecs::Bool;
using codec::codecs::Byte;
using codec::codecs::Double;
using codec::codecs::Float;
using codec::codecs::Int;
using codec::codecs::Long;
using codec::codecs::Passthrough;
using codec::codecs::Short;
using codec::codecs::String;
using codec::Dynamic;
using codec::testing::decode;
using codec::testing::decodeError;
using codec::testing::encode;
using codec::testing::dynamicJson;
using codec::testing::dumpJson;
using codec::testing::json;

TEST(PrimitiveCodecTest, Bool) {
  EXPECT_TRUE(decode(Bool, "true"));
  EXPECT_FALSE(decode(Bool, "false"));
  // JsonOps.getBooleanValue also accepts numbers.
  EXPECT_TRUE(decode(Bool, "1"));
  EXPECT_FALSE(decode(Bool, "0"));
  EXPECT_EQ(encode(Bool, true), "true");
  EXPECT_EQ(encode(Bool, false), "false");

  const std::string error = decodeError<bool>(Bool, "\"true\"");
  EXPECT_EQ(error.find("Not a boolean"), 0u) << error;
}

TEST(PrimitiveCodecTest, Int) {
  EXPECT_EQ(decode(Int, "42"), 42);
  EXPECT_EQ(decode(Int, "-7"), -7);
  // Number.intValue() truncates.
  EXPECT_EQ(decode(Int, "1.9"), 1);
  EXPECT_EQ(decode(Int, "-1.9"), -1);
  EXPECT_EQ(encode(Int, 42), "42");

  EXPECT_EQ(decodeError<int32_t>(Int, "\"42\"").find("Not a number"), 0u);
}

TEST(PrimitiveCodecTest, NumericWideningAndNarrowing) {
  EXPECT_EQ(decode(Long, "9007199254740993"), 9007199254740993LL);
  EXPECT_EQ(encode(Long, 9007199254740993LL), "9007199254740993");

  EXPECT_EQ(decode(Byte, "300"), 44);     // (byte) 300
  EXPECT_EQ(decode(Short, "65535"), -1);  // (short) 65535

  EXPECT_FLOAT_EQ(decode(Float, "1.5"), 1.5f);
  EXPECT_DOUBLE_EQ(decode(Double, "1.5"), 1.5);
  EXPECT_EQ(encode(Double, 1.5), "1.5");
  EXPECT_EQ(encode(Float, 1.5f), "1.5");
}

TEST(PrimitiveCodecTest, String) {
  EXPECT_EQ(decode(String, "\"hello\""), "hello");
  EXPECT_EQ(encode(String, "hello"), "\"hello\"");
  EXPECT_EQ(decodeError<std::string>(String, "1").find("Not a string"), 0u);
  EXPECT_EQ(decodeError<std::string>(String, "true").find("Not a string"), 0u);
}

TEST(PrimitiveCodecTest, StringAcceptsNumbersOnlyInCompressedMode) {
  const std::string error = decodeError<std::string>(String, "12");
  EXPECT_EQ(error.find("Not a string"), 0u);
  EXPECT_EQ(decode(String, "12", JsonOps::COMPRESSED), "12");
  EXPECT_EQ(decode(Int, "\"12\"", JsonOps::COMPRESSED), 12);
}

TEST(PrimitiveCodecTest, EncodeUsesMergeToPrimitive) {
  // A fresh prefix (ops.empty()) accepts the primitive.
  const DataResult<Value> fresh = Int.encodeStart(JsonOps::INSTANCE, 5);
  ASSERT_TRUE(fresh.result().has_value());
  EXPECT_EQ(jsonView(*fresh.result()).dump(), "5");

  // A non-empty prefix cannot absorb a primitive -- DFU's mergeToPrimitive.
  const DataResult<Value> merged = Int.encode(5, JsonOps::INSTANCE, json(R"({"a":1})"));
  ASSERT_TRUE(merged.isError());
  EXPECT_EQ(merged.message(),
            "Do not know how to append a primitive value 5 to {\"a\":1}");
  // The primitive itself is offered as the partial result.
  EXPECT_EQ(jsonView(*merged.valueOrPartial()).dump(), "5");
}

TEST(PrimitiveCodecTest, PassthroughKeepsArbitraryJson) {
  EXPECT_EQ(encode(Passthrough, dynamicJson(R"({"a":[1,2]})")), R"({"a":[1,2]})");
  EXPECT_EQ(encode(Passthrough, dynamicJson("\"text\"")), "\"text\"");
  EXPECT_EQ(encode(Passthrough, dynamicJson("null")), "null");

  const Dynamic decoded = decode(Passthrough, R"({"nested":{"x":true}})");
  EXPECT_EQ(dumpJson(decoded.value()), R"({"nested":{"x":true}})");
}

TEST(PrimitiveCodecTest, PassthroughMergesIntoAMapPrefix) {
  const DataResult<Value> merged =
      Passthrough.encode(dynamicJson(R"({"b":2})"), JsonOps::INSTANCE, json(R"({"a":1})"));
  ASSERT_TRUE(merged.result().has_value());
  EXPECT_EQ(jsonView(*merged.result()).dump(), R"({"a":1,"b":2})");
}

}  // namespace
