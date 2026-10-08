// Verifies that the port's serialized value type is backed by nlohmann/json and
// that applications can hand nlohmann documents straight to the codecs.
#include <nlohmann/json.hpp>

#include "risk_def.hpp"
#include "test_support.hpp"

namespace {

using codec::DataResult;
using codec::JsonOps;
using codec::JsonValue;
using codec::Value;

TEST(NlohmannInteropTest, JsonValueWrapsANlohmannDocument) {
  const nlohmann::ordered_json document =
      nlohmann::ordered_json::parse(R"({"b":1,"a":[true,null,2.5]})");
  const JsonValue value = document;  // implicit: nlohmann::ordered_json -> JsonValue

  EXPECT_TRUE(value.isObject());
  EXPECT_EQ(value.size(), 2u);
  // ordered_json keeps the insertion order, so the compact dump round-trips.
  EXPECT_EQ(value.dump(), R"({"b":1,"a":[true,null,2.5]})");
  EXPECT_EQ(value.raw(), document);
  EXPECT_EQ(value.raw().at("b").get<int>(), 1);
  EXPECT_EQ(value.raw().at("a").at(2).get<double>(), 2.5);
}

TEST(NlohmannInteropTest, CodecsConsumeNlohmannDocumentsAndProduceThemBack) {
  // Build the input with nlohmann/json directly.
  nlohmann::ordered_json risk;
  risk["id"] = "R-1";
  risk["vid"] = "VID-1";
  risk["risk_type"] = "security_bypass";
  risk["severity"] = "critical";
  risk["name"] = {{"cn", "名称"}, {"en", "name"}};
  risk["description"] = {{"cn", "描述"}, {"en", "description"}};
  risk["solution"] = {{"cn", "方案"}, {"en", "solution"}};
  risk["condition"] = {{"or", nlohmann::ordered_json::array({{{"param", "a"}, {"op", "is_true"}}})}};
  risk["evidence"] = nlohmann::ordered_json::array({"a", "b"});

  nlohmann::ordered_json document;
  document["risks"] = nlohmann::ordered_json::array({risk});

  // Decode it through the ported codec.
  const DataResult<risk::RiskDocument> decoded =
      risk::riskDocumentCodec().parse(JsonOps::INSTANCE, JsonValue(document));
  ASSERT_TRUE(decoded.result().has_value()) << decoded.message();
  const risk::RiskDef& decodedRisk = decoded.result()->risks.at(0);
  EXPECT_EQ(decodedRisk.id, "R-1");
  EXPECT_EQ(decodedRisk.name.cn, "名称");
  ASSERT_TRUE(decodedRisk.condition.has_value());
  EXPECT_EQ(*decodedRisk.condition->orClauses.at(0).param, "a");

  // Encode it back and read the result through nlohmann/json.
  const DataResult<Value> encoded =
      risk::riskDocumentCodec().encodeStart(JsonOps::INSTANCE, *decoded.result());
  ASSERT_TRUE(encoded.result().has_value()) << encoded.message();
  const nlohmann::ordered_json& raw = jsonView(*encoded.result()).raw();
  ASSERT_TRUE(raw.is_object());
  ASSERT_TRUE(raw.contains("risks"));
  EXPECT_EQ(raw.at("risks").at(0).at("id").get<std::string>(), "R-1");
  EXPECT_EQ(raw.at("risks").at(0).at("name").at("cn").get<std::string>(), "名称");
  EXPECT_EQ(raw.at("risks").at(0).at("evidence").size(), 2u);
  EXPECT_EQ(raw.dump(), document.dump());
}

TEST(NlohmannInteropTest, ParsingAndDumpingAreNlohmanns) {
  // JsonValue::parse is nlohmann's parser, JsonValue::dump is its serializer.
  const JsonValue parsed = JsonValue::parse(R"({"x": [1, 2, 3]})");
  EXPECT_EQ(parsed.raw(), nlohmann::ordered_json::parse(R"({"x":[1,2,3]})"));

  // nlohmann writes "null" for non-finite floats, which JsonValue inherits.
  const JsonValue notANumber = JsonValue::number(std::numeric_limits<double>::quiet_NaN());
  EXPECT_EQ(notANumber.dump(), "null");
  EXPECT_EQ(notANumber.raw().dump(), "null");

  // Malformed input raises JsonParseError, wrapping nlohmann's parse_error.
  try {
    JsonValue::parse("{oops}");
    FAIL() << "expected a parse error";
  } catch (const codec::JsonParseError& error) {
    EXPECT_NE(std::string(error.what()).find("parse_error"), std::string::npos);
  }
}

}  // namespace
