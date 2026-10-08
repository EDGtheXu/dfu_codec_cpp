// End-to-end tests for the reference use case: the risk-definition document.
#include <algorithm>
#include <string>
#include <vector>

#include "risk_def.hpp"
#include "test_support.hpp"

namespace {

using codec::Codec;
using codec::DataResult;
using codec::JsonOps;
using codec::JsonValue;
using codec::Value;
using codec::testing::dynamicJson;
using codec::testing::dumpJson;
using risk::Condition;
using risk::RiskDef;
using risk::RiskDocument;

RiskDocument sampleDocument() {
  return codec::testing::decode(risk::riskDocumentCodec(), risk::kSampleRiskJson());
}

TEST(RiskDefTest, DecodesTheSampleDocument) {
  const RiskDocument document = sampleDocument();
  ASSERT_EQ(document.risks.size(), 2u);

  const RiskDef& bypass = document.risks[0];
  EXPECT_EQ(bypass.id, "CLAUDE-BYPASS-001");
  EXPECT_EQ(bypass.vid, "VID0000004");
  EXPECT_EQ(bypass.riskType, "security_bypass");
  EXPECT_EQ(bypass.severity, "critical");
  EXPECT_EQ(bypass.name.cn, "Claude Code 权限审批被绕过");
  EXPECT_EQ(bypass.name.en, "Claude Code permission approval bypassed");
  EXPECT_EQ(bypass.evidence, std::vector<std::string>({"default_mode", "skip_permissions_flag"}));

  const RiskDef& mcp = document.risks[1];
  EXPECT_EQ(mcp.id, "CLAUDE-MCP-001");
  EXPECT_EQ(mcp.vid, "VID0000005");
  EXPECT_EQ(mcp.riskType, "other");
  EXPECT_EQ(mcp.severity, "medium");
  EXPECT_NE(mcp.description.en.find("--dangerously-skip-permissions"), std::string::npos);
  EXPECT_EQ(mcp.evidence, std::vector<std::string>({"mcp_args"}));
}

TEST(RiskDefTest, DecodesTheOrCondition) {
  const RiskDocument document = sampleDocument();
  ASSERT_TRUE(document.risks[0].condition.has_value());
  const Condition& condition = *document.risks[0].condition;

  // The group node only carries the "or" clause.
  EXPECT_FALSE(condition.param.has_value());
  EXPECT_FALSE(condition.op.has_value());
  EXPECT_FALSE(condition.value.has_value());
  ASSERT_EQ(condition.orClauses.size(), 2u);

  EXPECT_EQ(*condition.orClauses[0].param, "skip_permissions_flag");
  EXPECT_EQ(*condition.orClauses[0].op, "is_true");
  EXPECT_FALSE(condition.orClauses[0].value.has_value());
  EXPECT_TRUE(condition.orClauses[0].orClauses.empty());

  EXPECT_EQ(*condition.orClauses[1].param, "default_mode");
  EXPECT_EQ(*condition.orClauses[1].op, "eq");
  ASSERT_TRUE(condition.orClauses[1].value.has_value());
  EXPECT_EQ(dumpJson(condition.orClauses[1].value->value()), "\"bypassPermissions\"");
}

TEST(RiskDefTest, DecodesALeafConditionWithListMatch) {
  const RiskDocument document = sampleDocument();
  ASSERT_TRUE(document.risks[1].condition.has_value());
  const Condition& condition = *document.risks[1].condition;

  EXPECT_EQ(*condition.param, "mcp_args");
  EXPECT_EQ(*condition.op, "contains");
  ASSERT_TRUE(condition.value.has_value());
  EXPECT_EQ(*condition.value->asString().result(), "--dangerously-skip-permissions");
  ASSERT_TRUE(condition.listMatch.has_value());
  EXPECT_EQ(*condition.listMatch, "any");
  EXPECT_TRUE(condition.orClauses.empty());
  EXPECT_TRUE(condition.andClauses.empty());
  EXPECT_TRUE(condition.notClauses.empty());
}

TEST(RiskDefTest, EncodeReproducesTheSampleExactly) {
  const RiskDocument document = sampleDocument();
  const std::string encoded = codec::testing::encode(risk::riskDocumentCodec(), document);
  // Field declaration order mirrors the document, so the compact encoding is
  // byte-for-byte identical to a compact re-serialisation of the input.
  EXPECT_EQ(encoded, JsonValue::parse(risk::kSampleRiskJson()).dump());
}

TEST(RiskDefTest, DecodeEncodeDecodeIsStable) {
  const RiskDocument first = sampleDocument();
  const std::string encoded = codec::testing::encode(risk::riskDocumentCodec(), first);
  const RiskDocument second = codec::testing::decode(risk::riskDocumentCodec(), encoded);
  EXPECT_EQ(first, second);
  EXPECT_EQ(codec::testing::encode(risk::riskDocumentCodec(), second), encoded);
}

TEST(RiskDefTest, EncodedMemberOrderFollowsTheRecordDeclaration) {
  const std::string encoded = codec::testing::encode(risk::riskDocumentCodec(), sampleDocument());
  EXPECT_EQ(encoded.rfind(R"({"risks":[{"id":"CLAUDE-BYPASS-001)", 0), 0u);

  const JsonValue parsed = JsonValue::parse(encoded);
  ASSERT_TRUE(parsed.find("risks").has_value());
  const JsonValue::Array risks = parsed.find("risks")->asArray();
  const JsonValue::Object risk = risks[0].asObject();
  std::vector<std::string> keys;
  for (const auto& member : risk) {
    keys.push_back(member.first);
  }
  EXPECT_EQ(keys, (std::vector<std::string>{"id", "vid", "risk_type", "severity", "name",
                                            "description", "solution", "condition", "evidence"}));
}

TEST(RiskDefTest, MissingRequiredFieldsAreReportedTogether) {
  JsonValue::Object document = JsonValue::parse(risk::kSampleRiskJson()).asObject();
  JsonValue::Array risks = document[0].second.asArray();
  JsonValue::Object first = risks[0].asObject();
  first.erase(std::remove_if(first.begin(), first.end(),
                             [](const std::pair<std::string, Value>& member) {
                               return member.first == "severity" || member.first == "solution";
                             }),
              first.end());
  risks[0] = JsonValue::object(std::move(first));
  document[0].second = JsonValue::array(std::move(risks));

  const DataResult<RiskDocument> result =
      risk::riskDocumentCodec().parse(JsonOps::INSTANCE, JsonValue::object(std::move(document)));
  ASSERT_TRUE(result.isError());
  const std::string message = result.message();
  EXPECT_NE(message.find("No key severity in"), std::string::npos) << message;
  EXPECT_NE(message.find("No key solution in"), std::string::npos) << message;
  // The two failures are joined in declaration order.
  EXPECT_LT(message.find("No key severity in"), message.find("No key solution in"));
}

TEST(RiskDefTest, FailedElementsContributeTheirPartialValues) {
  const std::string text = R"({"risks":[
      {"id":"ok","vid":"v","risk_type":"t","severity":"s","name":{"cn":"c","en":"e"},
       "description":{"cn":"c","en":"e"},"solution":{"cn":"c","en":"e"},"evidence":[]},
      {"id":"bad","vid":"v","risk_type":"t","severity":"s","name":{"cn":"c","en":"e"},
       "description":{"cn":"c","en":"e"},"solution":{"cn":"c","en":"e"},"evidence":[1]}]})";
  const DataResult<RiskDocument> result =
      risk::riskDocumentCodec().parse(JsonOps::INSTANCE, codec::testing::json(text));
  ASSERT_TRUE(result.isError());
  EXPECT_EQ(result.message(), "Not a string: 1");

  // ListCodec keeps the successfully decoded prefix and, because the failing
  // risk could still be built partially (only "evidence" failed), DFU's
  // Applicative accumulation also contributes that partial value.
  ASSERT_TRUE(result.valueOrPartial().has_value());
  const RiskDocument& partial = *result.valueOrPartial();
  ASSERT_EQ(partial.risks.size(), 2u);
  EXPECT_EQ(partial.risks[0].id, "ok");
  EXPECT_EQ(partial.risks[1].id, "bad");
  EXPECT_TRUE(partial.risks[1].evidence.empty());
}

TEST(RiskDefTest, MalformedConditionalFieldsAreReportedWithTheirLocation) {
  // The model uses the strict optional variants on purpose: a malformed condition
  // must not be silently dropped, because that would turn "this rule is broken"
  // into "this rule does not apply".  message() stays DFU's text, describe() adds
  // where it happened.
  const DataResult<RiskDef> result = risk::riskDefCodec().parse(
      JsonOps::INSTANCE,
      codec::testing::json(R"({"id":"x","vid":"v","risk_type":"t","severity":"s",)"
                           R"("name":{"cn":"c","en":"e"},"description":{"cn":"c","en":"e"},)"
                           R"("solution":{"cn":"c","en":"e"},"evidence":[],)"
                           R"("condition":{"param":"a","list_match":1}})"));
  ASSERT_TRUE(result.isError());
  EXPECT_EQ(result.message(), "Not a string: 1");
  EXPECT_EQ(result.location(), "condition.list_match");
  EXPECT_EQ(result.describe(), "condition.list_match: Not a string: 1");
}

TEST(RiskDefTest, ErrorLocationPointsIntoTheDocument) {
  // Four risks, the last one with a numeric "op" where the model expects a string.
  std::string text = R"({"risks":[)";
  for (int i = 0; i < 3; ++i) {
    text += R"({"id":"ok","vid":"v","risk_type":"t","severity":"s","name":{"cn":"c","en":"e"},)"
            R"("description":{"cn":"c","en":"e"},"solution":{"cn":"c","en":"e"},)"
            R"("condition":{"or":[{"param":"p","op":"is_true"}]},"evidence":[]},)";
  }
  text += R"({"id":"bad","vid":"v","risk_type":"t","severity":"s","name":{"cn":"c","en":"e"},)"
          R"("description":{"cn":"c","en":"e"},"solution":{"cn":"c","en":"e"},)"
          R"("condition":{"or":[{"param":"p","op":1}]},"evidence":[]})";
  text += "]}";

  const DataResult<RiskDocument> result =
      risk::riskDocumentCodec().parse(JsonOps::INSTANCE, codec::testing::json(text));
  ASSERT_TRUE(result.isError());
  // DFU's message ...
  EXPECT_EQ(result.message(), "Not a string: 1");
  // ... plus where it happened, in the requested shape.
  EXPECT_EQ(result.location(), "risks[3].condition.or[0].op");
  EXPECT_EQ(result.describe(), "risks[3].condition.or[0].op: Not a string: 1");
  // ... and the chain of codecs that handled the value.  The condition tree is
  // recursive, so the same RecordCodec appears at both levels: the "or" element is
  // itself a condition, whose "or" field is the list that holds the bad leaf.
  const std::vector<std::string> expected{"String",
                                          "optional[op]",
                                          "RecordCodec[or, and, not, param, op, value, list_match]",
                                          "list",
                                          "optional[or]",
                                          "RecordCodec[or, and, not, param, op, value, list_match]",
                                          "optional[condition]",
                                          "RecordCodec[id, vid, risk_type, severity, name, "
                                          "description, solution, condition, evidence]",
                                          "list",
                                          "RecordCodec[risks]"};
  EXPECT_EQ(result.frameNames(), expected);

  // report() renders the same chain with each codec's construction site, so an IDE
  // can jump from the diagnostic to the line that built that codec.  The first
  // line is still the located message; every frame carries a location (the leaves
  // point into include/codec.hpp, the model frames into models/risk_def.hpp).
  const std::string report = result.report();
  const std::string firstLine = "risks[3].condition.or[0].op: Not a string: 1\n";
  EXPECT_EQ(report.compare(0, firstLine.size(), firstLine), 0) << report;
  EXPECT_NE(report.find("risk_def.hpp"), std::string::npos) << report;
  for (const codec::ErrorPart& part : result.errors()) {
    for (const codec::Frame& frame : part.frames) {
#if defined(CODEC_HAS_SOURCE_LOCATION) || defined(CODEC_HAS_BUILTIN_FILE)
      EXPECT_TRUE(frame.where.valid()) << frame.codec;
      EXPECT_GT(frame.where.line, 0u) << frame.codec;
#endif
    }
  }
}

TEST(RiskDefTest, LenientOptionalFieldsStillSwallowTheError) {
  // The other half of the same contract: codec::optionalFieldOf keeps DFU's
  // lenient behaviour, so a malformed value becomes "absent" instead of an error.
  // Useful for data you do not control; not what a validator wants.
  struct Lenient {
    std::optional<std::string> op;
  };
  const Codec<Lenient> lenient = codec::recordCodec<Lenient>(
      codec::optionalFieldOf("op", &Lenient::op, codec::codecs::String));
  const DataResult<Lenient> result =
      lenient.parse(JsonOps::INSTANCE, codec::testing::json(R"({"op":1})"));
  ASSERT_TRUE(result.isSuccess());
  EXPECT_FALSE(result.result()->op.has_value());
}

TEST(RiskDefTest, NestedTypeErrorsPointAtTheFailingField) {
  const DataResult<RiskDocument> result = risk::riskDocumentCodec().parse(
      JsonOps::INSTANCE,
      codec::testing::json(R"({"risks":[{"id":"x","vid":"v","risk_type":"t","severity":"s",)"
                           R"("name":{"cn":1,"en":"n"},"description":{"cn":"c","en":"e"},)"
                           R"("solution":{"cn":"c","en":"e"},"evidence":[]}]})"));
  ASSERT_TRUE(result.isError());
  EXPECT_EQ(result.message(), "Not a string: 1");
}

TEST(RiskDefTest, NestedConditionalGroupsAreRecursive) {
  const RiskDocument document = codec::testing::decode(
      risk::riskDocumentCodec(),
      R"({"risks":[{"id":"x","vid":"v","risk_type":"t","severity":"s",)"
      R"("name":{"cn":"c","en":"e"},"description":{"cn":"c","en":"e"},)"
      R"("solution":{"cn":"c","en":"e"},"evidence":[],)"
      R"("condition":{"or":[{"and":[{"param":"a","op":"eq","value":1},)"
      R"({"not":[{"param":"b","op":"is_true"}]}]}]}}]})");

  ASSERT_EQ(document.risks.size(), 1u);
  ASSERT_TRUE(document.risks[0].condition.has_value());
  const Condition& root = *document.risks[0].condition;
  ASSERT_EQ(root.orClauses.size(), 1u);
  const Condition& group = root.orClauses[0];
  ASSERT_EQ(group.andClauses.size(), 2u);
  ASSERT_TRUE(group.andClauses[0].value.has_value());
  // Passthrough keeps the value type: 1 stays a number.
  // 这里刻意用 JSON 节点访问器而不是 ops 的 asNumber()：JsonOps 的
  // getNumberValue 会把布尔强制成数字（移植的既有怪癖），
  // 那样 `true` 也能通过这些断言，断言强度就弱了。
  EXPECT_TRUE(group.andClauses[0].value->value().asJson().isNumber());
  EXPECT_EQ(group.andClauses[0].value->value().asJson().asNumber().intValue(), 1);
  ASSERT_EQ(group.andClauses[1].notClauses.size(), 1u);
  EXPECT_EQ(*group.andClauses[1].notClauses[0].param, "b");

  const std::string encoded = codec::testing::encode(risk::riskDocumentCodec(), document);
  EXPECT_NE(encoded.find(R"("value":1)"), std::string::npos) << encoded;
  EXPECT_EQ(codec::testing::decode(risk::riskDocumentCodec(), encoded), document);
}

TEST(RiskDefTest, OptionalConditionCanBeAbsent) {
  const RiskDef risk = codec::testing::decode(
      risk::riskDefCodec(),
      R"({"id":"x","vid":"v","risk_type":"t","severity":"s","name":{"cn":"c","en":"e"},)"
      R"("description":{"cn":"c","en":"e"},"solution":{"cn":"c","en":"e"},"evidence":["a"]})");
  EXPECT_FALSE(risk.condition.has_value());
}

TEST(RiskDefTest, EncodesAHandBuiltDocument) {
  RiskDocument document;
  RiskDef risk;
  risk.id = "ID-1";
  risk.vid = "VID-1";
  risk.riskType = "other";
  risk.severity = "low";
  risk.name = risk::LocalizedText{"名", "name"};
  risk.description = risk::LocalizedText{"描述", "description"};
  risk.solution = risk::LocalizedText{"方案", "solution"};
  risk.evidence = {"a", "b"};
  Condition leaf;
  leaf.param = "p";
  leaf.op = "eq";
  leaf.value = dynamicJson("\"v\"");
  leaf.listMatch = "any";
  risk.condition = leaf;
  document.risks.push_back(risk);

  const std::string encoded = codec::testing::encode(risk::riskDocumentCodec(), document);
  EXPECT_EQ(encoded,
            R"({"risks":[{"id":"ID-1","vid":"VID-1","risk_type":"other","severity":"low",)"
            R"("name":{"cn":"名","en":"name"},"description":{"cn":"描述","en":"description"},)"
            R"("solution":{"cn":"方案","en":"solution"},)"
            R"("condition":{"param":"p","op":"eq","value":"v","list_match":"any"},)"
            R"("evidence":["a","b"]}]})");
  EXPECT_EQ(codec::testing::decode(risk::riskDocumentCodec(), encoded), document);
}

TEST(RiskDefTest, EvidenceOrderIsPreserved) {
  const RiskDef risk = codec::testing::decode(
      risk::riskDefCodec(),
      R"({"id":"x","vid":"v","risk_type":"t","severity":"s","name":{"cn":"c","en":"e"},)"
      R"("description":{"cn":"c","en":"e"},"solution":{"cn":"c","en":"e"},)"
      R"("evidence":["b","a","b"]})");
  EXPECT_EQ(risk.evidence, std::vector<std::string>({"b", "a", "b"}));
}

}  // namespace
