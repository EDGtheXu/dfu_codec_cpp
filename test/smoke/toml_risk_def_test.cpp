// TOML 冒烟测试：同一个 codec 吃 JSON 与 TOML，以及 Passthrough 真正跨格式往返。
//
// 这是"抽象没漏"的核心回归：`riskDocumentCodec()` 一行都没改，却能直接解码
// TOML 版的风险文档，并得到与 JSON 版完全相同的结构。
#include <string>
#include <utility>
#include <vector>

#include "codec_toml.hpp"
#include "risk_def.hpp"
#include "test_support.hpp"
#include <gtest/gtest.h>

namespace {

using codec::DataResult;
using codec::Dynamic;
using codec::JsonOps;
using codec::JsonValue;
using codec::TomlDocument;
using codec::TomlOps;
using codec::Value;
using codec::codecs::Passthrough;
using codec::dumpToml;
using codec::parseToml;
using codec::testing::dynamicJson;
using codec::testing::dumpJson;

// JSON 样例 → 结构体 → TOML 文本（走 TomlOps 的 createX）。
std::string riskDocumentAsToml(const risk::RiskDocument& document) {
  const DataResult<Value> encoded =
      risk::riskDocumentCodec().encodeStart(TomlOps::INSTANCE, document);
  if (!encoded.result().has_value()) {
    throw std::runtime_error("encode to TOML failed: " + encoded.message());
  }
  const DataResult<std::string> text = dumpToml(*encoded.result());
  if (!text.result().has_value()) {
    throw std::runtime_error("dumpToml failed: " + text.message());
  }
  return *text.result();
}

TEST(TomlRiskDefTest, TheSameCodecDecodesTomlAndJson) {
  const DataResult<risk::RiskDocument> fromJson =
      risk::riskDocumentCodec().parse(JsonOps::INSTANCE, JsonValue::parse(risk::kSampleRiskJson()));
  ASSERT_TRUE(fromJson.result().has_value()) << fromJson.message();

  const std::string toml = riskDocumentAsToml(*fromJson.result());
  // 表数组会渲染成 [[risks]] 段。
  EXPECT_NE(toml.find("[[risks]]"), std::string::npos);
  EXPECT_NE(toml.find("[risks.condition]"), std::string::npos);

  const DataResult<TomlDocument> parsed = parseToml(toml);
  ASSERT_TRUE(parsed.result().has_value()) << parsed.message();

  const DataResult<risk::RiskDocument> fromToml =
      risk::riskDocumentCodec().parse(TomlOps::INSTANCE, parsed.result()->root());
  ASSERT_TRUE(fromToml.result().has_value()) << fromToml.message();
  EXPECT_EQ(*fromToml.result(), *fromJson.result());
}

TEST(TomlRiskDefTest, DumpingAndReparsingIsStable) {
  const DataResult<risk::RiskDocument> document =
      risk::riskDocumentCodec().parse(JsonOps::INSTANCE, JsonValue::parse(risk::kSampleRiskJson()));
  ASSERT_TRUE(document.result().has_value());

  const std::string toml = riskDocumentAsToml(*document.result());
  const DataResult<TomlDocument> reparsed = parseToml(toml);
  ASSERT_TRUE(reparsed.result().has_value());
  const DataResult<std::string> again = dumpToml(reparsed.result()->root());
  ASSERT_TRUE(again.result().has_value());
  EXPECT_EQ(*again.result(), toml);
}

TEST(TomlRiskDefTest, PassthroughValuesCrossFormats) {
  // JSON 侧构造一个动态值，编码到 TOML（Passthrough 会 convertTo 到 TomlOps），
  // 再经 TOML 解码回来 —— 这是 convertTo 跨格式分支真正跑通的地方。
  const Dynamic original = dynamicJson(R"({"a":1,"b":[1,2]})");

  const DataResult<Value> encoded =
      Passthrough.encode(original, TomlOps::INSTANCE, TomlOps::INSTANCE.empty());
  ASSERT_TRUE(encoded.result().has_value()) << encoded.message();

  const DataResult<std::string> toml = dumpToml(*encoded.result());
  ASSERT_TRUE(toml.result().has_value()) << toml.message();
  EXPECT_EQ(*toml.result(), "a = 1\nb = [1, 2]\n");

  const DataResult<TomlDocument> reparsed = parseToml(*toml.result());
  ASSERT_TRUE(reparsed.result().has_value()) << reparsed.message();
  const DataResult<Dynamic> decoded =
      Passthrough.parse(TomlOps::INSTANCE, reparsed.result()->root());
  ASSERT_TRUE(decoded.result().has_value()) << decoded.message();
  EXPECT_EQ(&decoded.result()->ops(), &TomlOps::INSTANCE);

  // 值层面相等：Dynamic::operator== 会把**对方**转成自己的 ops 再比较，
  // 所以这里是把 JSON 值转成 TOML 后用 TomlOps::valueEquals 比的。
  EXPECT_EQ(*decoded.result(), original);
  const Dynamic backToJson = decoded.result()->convertTo(JsonOps::INSTANCE);
  EXPECT_EQ(dumpJson(backToJson.value()), R"({"a":1,"b":[1,2]})");
}

TEST(TomlRiskDefTest, TomlValuesConvertBackToJson) {
  // TOML → JSON：日期时间会退化成字符串（有损，写侧不会再造出 TIME）。
  const DataResult<TomlDocument> parsed = parseToml(
      "title = \"demo\"\n"
      "when = 1979-05-27T07:32:00Z\n"
      "[limits]\n"
      "  max = 10\n");
  ASSERT_TRUE(parsed.result().has_value()) << parsed.message();

  const Value asJson = TomlOps::INSTANCE.convertTo(JsonOps::INSTANCE, parsed.result()->root());
  const std::string text = asJson.asJson().dump();
  EXPECT_NE(text.find(R"("title":"demo")"), std::string::npos);
  EXPECT_NE(text.find("1979-05-27"), std::string::npos);
  EXPECT_NE(text.find(R"("limits":{"max":10})"), std::string::npos);
}

}  // namespace
