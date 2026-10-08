// Error locations.
//
// DataFixerUpper messages carry no location: a bad value inside
// {"risks": [..., {"condition": {"or": [{"op": 1}]}}]} only says `Not a string: 1`.
// The port records where a failure happened, so:
//
//     result.message()   -> "Not a string: 1"                       (DFU text)
//     result.location()  -> "risks[3].condition.or[0].op"
//     result.describe()  -> "risks[3].condition.or[0].op: Not a string: 1"
//
// message() is unchanged, which is why every pre-existing assertion still holds;
// describe() is the form to show a user.  Containers attach the segment as the
// error travels outwards: fieldOf adds the key, ListCodec adds [i], unboundedMap
// adds the entry key, dispatch adds "value", and the record builders add the field
// name when encoding.
#include <optional>
#include <string>
#include <vector>

#include "codec.hpp"
#include "risk_def.hpp"
#include <gtest/gtest.h>

namespace {

using codec::Codec;
using codec::CodecResultFunction;
using codec::DataResult;
using codec::DynamicOps;
using codec::JsonOps;
using codec::JsonValue;

// --- a small model with the same nesting as the reported case ---------------
struct Leaf {
  std::string op;
};
struct Group {
  std::vector<Leaf> orClauses;  // JSON "or"
};
struct Rule {
  std::optional<Group> condition;
};
struct Document {
  std::vector<Rule> risks;
};

Codec<Leaf> leafCodec(const Codec<std::string>& opCodec) {
  return codec::recordCodec<Leaf>(codec::fieldOf("op", &Leaf::op, opCodec));
}

Codec<Group> groupCodec(const Codec<std::string>& opCodec) {
  return codec::recordCodec<Group>(codec::fieldOf("or", &Group::orClauses,
                                                  codec::listOf(leafCodec(opCodec))));
}

Codec<Rule> ruleCodec(const Codec<std::string>& opCodec) {
  return codec::recordCodec<Rule>(
      codec::optionalFieldOfStrict("condition", &Rule::condition, groupCodec(opCodec)));
}

Codec<Document> documentCodec(const Codec<std::string>& opCodec) {
  return codec::recordCodec<Document>(
      codec::fieldOf("risks", &Document::risks, codec::listOf(ruleCodec(opCodec))));
}

// Four risks, the last one with a numeric "op" where a string is expected.
std::string documentWithBadOp() {
  std::string text = R"({"risks":[)";
  for (int i = 0; i < 3; ++i) {
    text += R"({"condition":{"or":[{"op":"is_true"}]}},)";
  }
  text += R"({"condition":{"or":[{"op":1}]}})";
  text += "]}";
  return text;
}

DataResult<Document> parseDocument(const std::string& text) {
  return documentCodec(codec::codecs::String)
      .parse(JsonOps::INSTANCE, JsonValue::parse(text));
}

template <class A>
DataResult<A> decode(const Codec<A>& codec, const std::string& text) {
  return codec.parse(JsonOps::INSTANCE, JsonValue::parse(text));
}

TEST(ErrorPathTest, ReportsTheRequestedLocationFormat) {
  const DataResult<Document> result = parseDocument(documentWithBadOp());
  ASSERT_TRUE(result.isError());
  // DFU's message is untouched ...
  EXPECT_EQ(result.message(), "Not a string: 1");
  // ... and the location is additive.
  EXPECT_EQ(result.location(), "risks[3].condition.or[0].op");
  EXPECT_EQ(result.describe(), "risks[3].condition.or[0].op: Not a string: 1");
}

TEST(ErrorPathTest, WordingCanBeRewrittenWhileKeepingTheLocation) {
  // If you prefer "expected string, got number" to DFU's "Not a string: 1", rewrite
  // the leaf codec with mapResult (DFU's result function); the location survives.
  CodecResultFunction<std::string> rewriteWording;
  rewriteWording.apply = [](const DynamicOps&, const JsonValue&,
                            const DataResult<std::pair<std::string, JsonValue>>& result) {
    return result.mapError([](const std::string&) {
      return std::string("expected string, got number");
    });
  };
  rewriteWording.coApply = [](const DynamicOps&, const std::string&,
                              const DataResult<JsonValue>& result) { return result; };

  const Codec<std::string> expectedString = codec::codecs::String.mapResult(rewriteWording);
  const DataResult<Document> result =
      documentCodec(expectedString).parse(JsonOps::INSTANCE, JsonValue::parse(documentWithBadOp()));

  ASSERT_TRUE(result.isError());
  EXPECT_EQ(result.describe(), "risks[3].condition.or[0].op: expected string, got number");
}

TEST(ErrorPathTest, PathsComposeThroughNestedLists) {
  const Codec<std::vector<std::vector<int32_t>>> codec =
      codec::listOf(codec::listOf(codec::codecs::Int));
  const DataResult<std::vector<std::vector<int32_t>>> result = decode(codec, "[[1,2],[3,\"x\"]]");
  ASSERT_TRUE(result.isError());
  EXPECT_EQ(result.message(), "Not a number: \"x\"");
  EXPECT_EQ(result.location(), "[1][1]");
  EXPECT_EQ(result.describe(), "[1][1]: Not a number: \"x\"");
}

TEST(ErrorPathTest, EachFailedPartKeepsItsOwnLocation) {
  struct Pair {
    int32_t a = 0;
    int32_t b = 0;
  };
  const Codec<Pair> codec = codec::recordCodec<Pair>(
      codec::fieldOf("a", &Pair::a, codec::codecs::Int),
      codec::fieldOf("b", &Pair::b, codec::codecs::Int));

  const DataResult<Pair> result = decode(codec, R"({"a":"x","b":true,"c":0})");
  ASSERT_TRUE(result.isError());
  // `true` is a number for JsonOps, so only "a" fails.
  EXPECT_EQ(result.message(), "Not a number: \"x\"");
  EXPECT_EQ(result.location(), "a");
  EXPECT_EQ(result.describe(), "a: Not a number: \"x\"");

  // Both fields missing: message() joins DFU-style, describe() locates each.
  const DataResult<Pair> missing = decode(codec, "{}");
  ASSERT_TRUE(missing.isError());
  EXPECT_EQ(missing.message(), "No key a in MapLike[{}]; No key b in MapLike[{}]");
  EXPECT_EQ(missing.describe(),
            "a: No key a in MapLike[{}]; b: No key b in MapLike[{}]");
  // Different locations -> location() cannot pick one and says so.
  EXPECT_EQ(missing.location(), "");
  EXPECT_EQ(missing.errors().size(), 2u);
}

TEST(ErrorPathTest, RelativePathsFollowTheFieldNesting) {
  // Relative paths must not repeat the enclosing field names.
  const DataResult<Document> result = parseDocument(R"({"risks":[{"condition":{"or":[{"op":1}]}}]})");
  ASSERT_TRUE(result.isError());
  EXPECT_EQ(result.describe(), "risks[0].condition.or[0].op: Not a string: 1");
}

TEST(ErrorPathTest, MissingKeysAndWrongTypesBothCarryThePath) {
  const Codec<Document> codec = documentCodec(codec::codecs::String);
  const DataResult<Document> missingOp =
      codec.parse(JsonOps::INSTANCE, JsonValue::parse(R"({"risks":[{"condition":{"or":[{}]}}]})"));
  ASSERT_TRUE(missingOp.isError());
  EXPECT_EQ(missingOp.describe(), "risks[0].condition.or[0].op: No key op in MapLike[{}]");

  const DataResult<Document> notAnObject =
      codec.parse(JsonOps::INSTANCE, JsonValue::parse(R"({"risks":[{"condition":{"or":1}}]})"));
  ASSERT_TRUE(notAnObject.isError());
  EXPECT_EQ(notAnObject.describe(), "risks[0].condition.or: Not a json array: 1");
}

TEST(ErrorPathTest, UnboundedMapsAndDispatchLocateTheirFailures) {
  const Codec<std::vector<std::pair<std::string, int32_t>>> mapCodec =
      codec::unboundedMap(codec::codecs::String, codec::codecs::Int);
  const DataResult<std::vector<std::pair<std::string, int32_t>>> badEntry =
      decode(mapCodec, R"({"a":1,"b":"x"})");
  ASSERT_TRUE(badEntry.isError());
  EXPECT_EQ(badEntry.message(), "Not a number: \"x\" missed input: {\"b\":\"x\"}");
  EXPECT_EQ(badEntry.describe(), "b: Not a number: \"x\" missed input: {\"b\":\"x\"}");

  // A dispatch payload: a MapCodecCodec payload shares the outer object, so the
  // path is just the payload field ...
  struct Circle {
    double radius = 0.0;
  };
  const Codec<Circle> circleCodec =
      codec::recordCodec<Circle>(codec::fieldOf("radius", &Circle::radius, codec::codecs::Double));
  const Codec<Circle> dispatched = codec::codecs::String.partialDispatch<Circle>(
      "type",
      [](const Circle&) -> DataResult<std::string> {
        return DataResult<std::string>::success("circle");
      },
      [circleCodec](const std::string&) -> DataResult<Codec<Circle>> {
        return DataResult<Codec<Circle>>::success(circleCodec);
      });
  const DataResult<Circle> flatPayload =
      decode(dispatched, R"({"type":"circle","radius":"x"})");
  ASSERT_TRUE(flatPayload.isError());
  EXPECT_EQ(flatPayload.describe(), "radius: Not a number: \"x\"");

  // ... while a payload that is *not* map backed is stored under "value", and the
  // location says so (xmap drops the MapCodec, exactly as in DFU).
  const Codec<Circle> plainCircle = circleCodec.xmap<Circle>([](const Circle& value) { return value; },
                                                             [](const Circle& value) { return value; });
  const Codec<Circle> valueDispatched = codec::codecs::String.partialDispatch<Circle>(
      "type",
      [](const Circle&) -> DataResult<std::string> {
        return DataResult<std::string>::success("circle");
      },
      [plainCircle](const std::string&) -> DataResult<Codec<Circle>> {
        return DataResult<Codec<Circle>>::success(plainCircle);
      });
  const DataResult<Circle> nestedPayload =
      decode(valueDispatched, R"({"type":"circle","value":{"radius":"x"}})");
  ASSERT_TRUE(nestedPayload.isError());
  EXPECT_EQ(nestedPayload.describe(), "value.radius: Not a number: \"x\"");
}

TEST(ErrorPathTest, EncodeFailuresCarryTheirFieldToo) {
  struct Holder {
    int32_t small = 0;
  };
  const Codec<int32_t> bounded = codec::codecs::Int.flatComapMap<int32_t>(
      [](const int32_t& value) { return value; },
      [](const int32_t& value) -> DataResult<int32_t> {
        if (value > 100) {
          return DataResult<int32_t>::error("too large to encode: " + std::to_string(value));
        }
        return DataResult<int32_t>::success(value);
      });
  const Codec<Holder> codec =
      codec::recordCodec<Holder>(codec::fieldOf("small", &Holder::small, bounded));

  const DataResult<JsonValue> encoded = codec.encodeStart(JsonOps::INSTANCE, Holder{200});
  ASSERT_TRUE(encoded.isError());
  EXPECT_EQ(encoded.message(), "too large to encode: 200");
  EXPECT_EQ(encoded.describe(), "small: too large to encode: 200");
}

TEST(ErrorPathTest, StrictAndLenientOptionalFieldsDiffer) {
  struct Holder {
    std::optional<int32_t> n;
  };
  const Codec<Holder> lenient = codec::recordCodec<Holder>(
      codec::optionalFieldOf("n", &Holder::n, codec::codecs::Int));
  const Codec<Holder> strict = codec::recordCodec<Holder>(
      codec::optionalFieldOfStrict("n", &Holder::n, codec::codecs::Int));

  // DFU behaviour: a present-but-invalid optional field is "absent".
  const DataResult<Holder> lenientResult = decode(lenient, R"({"n":"x"})");
  ASSERT_TRUE(lenientResult.isSuccess());
  EXPECT_FALSE(lenientResult.result()->n.has_value());

  // Addition: it is reported, with a location.
  const DataResult<Holder> strictResult = decode(strict, R"({"n":"x"})");
  ASSERT_TRUE(strictResult.isError());
  EXPECT_EQ(strictResult.message(), "Not a number: \"x\"");
  EXPECT_EQ(strictResult.describe(), "n: Not a number: \"x\"");

  // Absent stays absent for both, and null is absent like everywhere else.
  EXPECT_TRUE(decode(strict, "{}").isSuccess());
  EXPECT_FALSE(decode(strict, "{}").result()->n.has_value());
  EXPECT_TRUE(decode(strict, R"({"n":null})").isSuccess());
}

TEST(ErrorPathTest, SuccessHasNoLocation) {
  const DataResult<Document> result = parseDocument(R"({"risks":[]})");
  ASSERT_TRUE(result.isSuccess());
  EXPECT_TRUE(result.errors().empty());
  EXPECT_EQ(result.location(), "");
  EXPECT_EQ(result.describe(), "");
}

}  // namespace
