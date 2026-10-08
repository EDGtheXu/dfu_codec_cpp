// Dynamic —— DFU 的 com.mojang.serialization.Dynamic<T> 的移植（值 + 懂它的 ops）。
//
// Dynamic 是 `Codec.PASSTHROUGH` 的载体：原始动态值离开它所属的 ops 就无法
// 解释，因此两者绑在一起；它同时也是跨格式搬运（convertTo）的入口。
// 这里覆盖取值/取子项/改写/转换/decode 余值，以及"只比值不比 ops"的相等语义。
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "codec.hpp"
#include "test_support.hpp"
#include <gtest/gtest.h>

namespace {

using codec::Codec;
using codec::DataResult;
using codec::Dynamic;
using codec::JsonOps;
using codec::JsonValue;
using codec::Number;
using codec::Value;
using codec::codecs::Passthrough;
using codec::testing::dynamicJson;
using codec::testing::dumpJson;
using codec::testing::encode;

TEST(DynamicTest, ConstructsFromOpsAndValue) {
  // DFU: Dynamic(ops) 就是 Dynamic(ops, ops.empty())。
  const Dynamic empty(JsonOps::INSTANCE);
  EXPECT_EQ(&empty.ops(), &JsonOps::INSTANCE);  // Dynamic 记住的是 ops 本身
  EXPECT_TRUE(JsonOps::INSTANCE.valueEquals(empty.value(), JsonOps::INSTANCE.empty()));

  const Dynamic number(JsonOps::INSTANCE, Value(JsonValue::number(5)));
  ASSERT_TRUE(number.asNumber().result().has_value());
  EXPECT_EQ(number.asNumber().result()->intValue(), 5);
}

TEST(DynamicTest, ReadsScalarsThroughItsOps) {
  const Dynamic number = dynamicJson("5");
  EXPECT_EQ(number.asNumber().result()->intValue(), 5);
  // JsonOps 的 getStringValue 在非压缩模式下不接受数字。
  EXPECT_TRUE(number.asString().isError());
  EXPECT_TRUE(number.asString().message().find("Not a string") != std::string::npos);

  const Dynamic text = dynamicJson("\"hello\"");
  ASSERT_TRUE(text.asString().result().has_value());
  EXPECT_EQ(*text.asString().result(), "hello");
  EXPECT_TRUE(text.asNumber().isError());

  const Dynamic flag = dynamicJson("true");
  ASSERT_TRUE(flag.asBoolean().result().has_value());
  EXPECT_TRUE(*flag.asBoolean().result());
  // JsonOps 的布尔/数字互相强制：true 也能读成 1（与 Codec.BOOL/INT 一致）。
  EXPECT_EQ(flag.asNumber().result()->intValue(), 1);
}

TEST(DynamicTest, GetsMembersAndListElements) {
  const Dynamic object = dynamicJson(R"({"a":1,"b":[10,20]})");

  const std::optional<Dynamic> a = object.get("a");
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(a->asNumber().result()->intValue(), 1);

  EXPECT_FALSE(object.get("missing").has_value());

  const std::optional<Dynamic> b = object.get("b");
  ASSERT_TRUE(b.has_value());
  const std::optional<Dynamic> first = b->getElement(0);
  const std::optional<Dynamic> second = b->getElement(1);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->asNumber().result()->intValue(), 20);
  EXPECT_FALSE(b->getElement(2).has_value());   // 越界
  EXPECT_FALSE(b->getElement(-1).has_value());  // 负下标
  EXPECT_FALSE(a->getElement(0).has_value());   // 不是列表

  // 非对象取键、非列表取下标的路径都返回 nullopt（DFU 的 OptionalDynamic 语义）。
  EXPECT_FALSE(dynamicJson("[1,2]").get("a").has_value());
}

TEST(DynamicTest, SetsRemovesAndUpdatesMembers) {
  const Dynamic object = dynamicJson(R"({"a":1})");

  EXPECT_EQ(dumpJson(object.set("b", dynamicJson("2")).value()), R"({"a":1,"b":2})");
  EXPECT_EQ(dumpJson(object.set("a", dynamicJson("9")).value()), R"({"a":9})");
  EXPECT_EQ(dumpJson(object.remove("a").value()), R"({})");
  EXPECT_EQ(dumpJson(object.remove("missing").value()), R"({"a":1})");

  const Dynamic updated = object.update("a", [](const Dynamic& current) {
    return current.withValue(Value(JsonValue::number(current.asNumber().result()->intValue() + 10)));
  });
  EXPECT_EQ(dumpJson(updated.value()), R"({"a":11})");

  // 键不存在时 ops.update 原样返回（DFU 的 DynamicOps.update 语义）。
  const Dynamic untouched = object.update("missing", [](const Dynamic& current) { return current; });
  EXPECT_EQ(dumpJson(untouched.value()), R"({"a":1})");
}

TEST(DynamicTest, EqualityIsValueBasedAndSurvivesConvert) {
  // 数字相等沿用 Gson 的 JsonPrimitive 规则（1 == 1.0）。
  EXPECT_EQ(dynamicJson("1"), dynamicJson("1.0"));
  EXPECT_NE(dynamicJson("1"), dynamicJson("\"1\""));

  // convertTo 换一个 ops（JSON 侧目前是恒等），值不变。
  const Dynamic converted = dynamicJson(R"({"a":1})").convertTo(JsonOps::INSTANCE);
  EXPECT_EQ(dumpJson(converted.value()), R"({"a":1})");

  // 有意偏离：DFU 的 equals 先比 ops 身份，这里只比值，因此跨 ops 也能相等。
  const Dynamic viaCompressed(JsonOps::COMPRESSED, Value(JsonValue::number(7)));
  EXPECT_EQ(viaCompressed, dynamicJson("7"));
}

TEST(DynamicTest, DecodeReturnsTheRemainingValueAsDynamic) {
  const Dynamic number = dynamicJson("42");
  const DataResult<std::pair<int32_t, Dynamic>> decoded = number.decode(codec::codecs::Int.decoder());
  ASSERT_TRUE(decoded.result().has_value());
  EXPECT_EQ(decoded.result()->first, 42);
  // 余下的值由同一个 ops 包成 Dynamic（偏离：DFU 返回裸的 T）。
  EXPECT_EQ(decoded.result()->second, Dynamic(JsonOps::INSTANCE));
}

TEST(DynamicTest, PassthroughEncodesAValueFromAnotherOps) {
  // encode 时先 convertTo 到目标 ops —— 用 COMPRESSED 构造的 Dynamic 编码到
  // INSTANCE 会走那条分支（JSON 侧两个 ops 的 convertTo 目前都是恒等）。
  const Dynamic value(JsonOps::COMPRESSED, Value(JsonValue::string("x")));
  EXPECT_EQ(encode(Passthrough, value, JsonOps::INSTANCE), "\"x\"");

  // 空值编码为 prefix（DFU: input.getValue() == input.getOps().empty() 分支）。
  const DataResult<Value> encoded =
      Passthrough.encode(Dynamic(JsonOps::INSTANCE), JsonOps::INSTANCE,
                         Value(JsonValue::string("prefix")));
  ASSERT_TRUE(encoded.result().has_value());
  EXPECT_EQ(dumpJson(*encoded.result()), "\"prefix\"");
}

}  // namespace
