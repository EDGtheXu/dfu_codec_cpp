// Tests for the Codec combinators: xmap/flatXmap/comapFlatMap/flatComapMap,
// orElse, promotePartial, mapResult, either, pair, listOf, unboundedMap,
// ranges, lifecycle wrapping and recursive codecs.
#include "test_support.hpp"

namespace {

using codec::Codec;
using codec::CodecResultFunction;
using codec::DataResult;
using codec::DynamicOps;
using codec::Either;
using codec::JsonOps;
using codec::JsonValue;
using codec::Lifecycle;
using codec::MapCodec;
using codec::codecs::Bool;
using codec::codecs::Int;
using codec::codecs::Passthrough;
using codec::codecs::String;
using codec::testing::decode;
using codec::testing::decodeError;
using codec::testing::encode;
using codec::testing::json;

enum class Level { Low, High };

Codec<Level> levelCodec() {
  return Int.flatXmap<Level>(
      [](const int32_t& value) -> DataResult<Level> {
        if (value == 0) {
          return DataResult<Level>::success(Level::Low);
        }
        if (value == 1) {
          return DataResult<Level>::success(Level::High);
        }
        return DataResult<Level>::error("unknown level " + std::to_string(value));
      },
      [](const Level& value) -> DataResult<int32_t> {
        return DataResult<int32_t>::success(value == Level::Low ? 0 : 1);
      });
}

TEST(CodecCombinatorTest, XmapRoundTrip) {
  const Codec<std::string> asText = Int.xmap<std::string>(
      [](const int32_t& value) { return std::to_string(value); },
      [](const std::string& text) { return std::stoi(text); });
  EXPECT_EQ(decode(asText, "5"), "5");
  EXPECT_EQ(encode(asText, "5"), "5");
}

TEST(CodecCombinatorTest, FlatXmapCanRejectValues) {
  const Codec<Level> codec = levelCodec();
  EXPECT_EQ(decode(codec, "0"), Level::Low);
  EXPECT_EQ(decode(codec, "1"), Level::High);
  EXPECT_EQ(encode(codec, Level::High), "1");
  EXPECT_EQ(decodeError<Level>(codec, "2"), "unknown level 2");
}

TEST(CodecCombinatorTest, ComapFlatMapAndFlatComapMap) {
  const Codec<int32_t> positive = Int.comapFlatMap<int32_t>(
      [](const int32_t& value) -> DataResult<int32_t> {
        if (value < 0) {
          return DataResult<int32_t>::error("negative: " + std::to_string(value));
        }
        return DataResult<int32_t>::success(value);
      },
      [](const int32_t& value) { return value; });
  EXPECT_EQ(decode(positive, "3"), 3);
  EXPECT_EQ(decodeError<int32_t>(positive, "-3"), "negative: -3");

  const Codec<int32_t> bounded = Int.flatComapMap<int32_t>(
      [](const int32_t& value) { return value; },
      [](const int32_t& value) -> DataResult<int32_t> {
        if (value > 100) {
          return DataResult<int32_t>::error("too large to encode: " + std::to_string(value));
        }
        return DataResult<int32_t>::success(value);
      });
  EXPECT_EQ(decode(bounded, "50"), 50);
  const DataResult<JsonValue> tooLarge = bounded.encodeStart(JsonOps::INSTANCE, 200);
  ASSERT_TRUE(tooLarge.isError());
  EXPECT_EQ(tooLarge.message(), "too large to encode: 200");
}

TEST(CodecCombinatorTest, OrElseFallsBackOnFailure) {
  EXPECT_EQ(decode(Int.orElse(42), "\"not a number\""), 42);
  EXPECT_EQ(decode(Int.orElse(42), "7"), 7);
  EXPECT_EQ(decode(Int.orElseGet([] { return 9; }), "{}"), 9);
}

TEST(CodecCombinatorTest, PromotePartialTurnsFailedElementsIntoASuccess) {
  const Codec<std::vector<int32_t>> codec = codec::listOf(Int);
  const DataResult<std::vector<int32_t>> strict = codec.parse(JsonOps::INSTANCE, json("[1,\"x\"]"));
  ASSERT_TRUE(strict.isError());
  // message() is DFU's text; location()/describe() add where it happened.
  EXPECT_EQ(strict.message(), "Not a number: \"x\"");
  EXPECT_EQ(strict.location(), "[1]");
  EXPECT_EQ(strict.describe(), "[1]: Not a number: \"x\"");

  std::string reported;
  const DataResult<std::vector<int32_t>> promoted =
      codec.promotePartial([&](const std::string& message) { reported = message; })
          .parse(JsonOps::INSTANCE, json("[1,\"x\"]"));
  ASSERT_TRUE(promoted.isSuccess());
  // Diagnostics callbacks get the located form.
  EXPECT_EQ(reported, "[1]: Not a number: \"x\"");
  EXPECT_EQ(promoted.result()->size(), 1u);
  EXPECT_EQ(promoted.result()->at(0), 1);
}

TEST(CodecCombinatorTest, MapResultCanRewriteErrors) {
  CodecResultFunction<int32_t> function;
  function.apply = [](const DynamicOps&, const JsonValue&,
                      const DataResult<std::pair<int32_t, JsonValue>>& result) {
    return result.mapError([](const std::string& message) { return "E:" + message; });
  };
  function.coApply = [](const DynamicOps&, const int32_t&,
                        const DataResult<JsonValue>& result) { return result; };

  const Codec<int32_t> wrapped = Int.mapResult(function);
  EXPECT_EQ(wrapped.parse(JsonOps::INSTANCE, json("1")).result().value(), 1);
  EXPECT_EQ(wrapped.parse(JsonOps::INSTANCE, json("\"x\"")).message(), "E:Not a number: \"x\"");
}

TEST(CodecCombinatorTest, LifecycleWrapping) {
  EXPECT_TRUE(Int.stable().parse(JsonOps::INSTANCE, json("1")).lifecycle().isStable());
  EXPECT_EQ(Int.deprecated(3).parse(JsonOps::INSTANCE, json("1")).lifecycle().since(), 3);
  EXPECT_TRUE(Int.stable().withLifecycle(Lifecycle::experimental())
                  .parse(JsonOps::INSTANCE, json("1"))
                  .lifecycle()
                  .isExperimental());
}

TEST(EitherCodecTest, DecodesFirstMatchingAlternative) {
  const Codec<Either<int32_t, std::string>> codec = codec::either(Int, String);
  const Either<int32_t, std::string> left = decode(codec, "7");
  ASSERT_TRUE(left.isLeft());
  EXPECT_EQ(left.left(), 7);

  const Either<int32_t, std::string> right = decode(codec, "\"text\"");
  ASSERT_TRUE(right.isRight());
  EXPECT_EQ(right.right(), "text");

  EXPECT_EQ(encode(codec, Either<int32_t, std::string>::left(7)), "7");
  EXPECT_EQ(encode(codec, Either<int32_t, std::string>::right("text")), "\"text\"");

  // JsonOps also accepts booleans as numbers, so `true` takes the Int branch.
  const Either<int32_t, std::string> boolean = decode(codec, "true");
  ASSERT_TRUE(boolean.isLeft());
  EXPECT_EQ(boolean.left(), 1);
}

TEST(EitherCodecTest, ReportsTheSecondFailureWhenNeitherMatches) {
  const Codec<Either<int32_t, bool>> codec = codec::either(Int, Bool);
  const DataResult<Either<int32_t, bool>> result = codec.parse(JsonOps::INSTANCE, json("\"x\""));
  ASSERT_TRUE(result.isError());
  EXPECT_NE(result.message().find("Not a boolean"), std::string::npos) << result.message();
}

TEST(PairCodecTest, ChainsCodecsOverTheRemainingPrefix) {
  const Codec<std::pair<JsonValue, JsonValue>> codec = codec::pair(Passthrough, Passthrough);
  const std::pair<JsonValue, JsonValue> expected{json("1"), json("null")};
  // The first codec consumes the input, the second reads the first one's rest
  // (JsonOps primitives always leave ops.empty() behind).
  EXPECT_EQ(decode(codec, "1"), expected);
  EXPECT_EQ(encode(codec, expected), "1");
}

TEST(ListCodecTest, DecodeEncodeRoundTrip) {
  const Codec<std::vector<int32_t>> codec = codec::listOf(Int);
  const std::vector<int32_t> values{1, 2, 3};
  EXPECT_EQ(decode(codec, "[1,2,3]"), values);
  EXPECT_EQ(encode(codec, values), "[1,2,3]");
  EXPECT_TRUE(decode(codec, "[]").empty());
}

TEST(ListCodecTest, FailedElementsAreReportedInThePartialResult) {
  const Codec<std::vector<int32_t>> codec = codec::listOf(Int);
  // Codec::decode keeps the ListCodec pair: the decoded prefix plus the list of
  // raw elements that failed.
  const DataResult<std::pair<std::vector<int32_t>, JsonValue>> decoded =
      codec.decode(JsonOps::INSTANCE, json("[1,\"x\",3]"));
  ASSERT_TRUE(decoded.isError());
  EXPECT_EQ(decoded.message(), "Not a number: \"x\"");
  ASSERT_TRUE(decoded.valueOrPartial().has_value());
  const std::pair<std::vector<int32_t>, JsonValue>& partial = *decoded.valueOrPartial();
  // Only the elements up to the first failure are kept...
  EXPECT_EQ(partial.first, std::vector<int32_t>{1});
  // ... and the offending raw values are collected as the "remaining" value.
  EXPECT_EQ(partial.second.dump(), "[\"x\"]");

  // Codec::parse projects the pair onto its first component, so the partial
  // value of the failing parse is the vector itself.
  const DataResult<std::vector<int32_t>> parsed = codec.parse(JsonOps::INSTANCE, json("[1,\"x\",3]"));
  ASSERT_TRUE(parsed.isError());
  ASSERT_TRUE(parsed.valueOrPartial().has_value());
  EXPECT_EQ(*parsed.valueOrPartial(), std::vector<int32_t>{1});
}

TEST(ListCodecTest, NonArrayInputFails) {
  const Codec<std::vector<int32_t>> codec = codec::listOf(Int);
  EXPECT_EQ(decodeError<std::vector<int32_t>>(codec, "{}"), "Not a json array: {}");
}

TEST(ListCodecTest, ListOfList) {
  const Codec<std::vector<std::vector<int32_t>>> codec = codec::listOf(codec::listOf(Int));
  EXPECT_EQ(encode(codec, {{1, 2}, {3}}), "[[1,2],[3]]");
  EXPECT_EQ(decode(codec, "[[1,2],[3]]").size(), 2u);
}

TEST(UnboundedMapTest, DecodeEncodeRoundTrip) {
  const Codec<std::vector<std::pair<std::string, int32_t>>> codec =
      codec::unboundedMap(String, Int);
  const std::vector<std::pair<std::string, int32_t>> values{{"a", 1}, {"b", 2}};
  EXPECT_EQ(decode(codec, R"({"a":1,"b":2})"), values);
  EXPECT_EQ(encode(codec, values), R"({"a":1,"b":2})");
}

TEST(UnboundedMapTest, ReportsBadEntriesWithTheMissedInput) {
  const Codec<std::vector<std::pair<std::string, int32_t>>> codec =
      codec::unboundedMap(String, Int);
  const DataResult<std::vector<std::pair<std::string, int32_t>>> result =
      codec.parse(JsonOps::INSTANCE, json(R"({"a":"x"})"));
  ASSERT_TRUE(result.isError());
  EXPECT_EQ(result.message(), "Not a number: \"x\" missed input: {\"a\":\"x\"}");

  const Codec<std::vector<std::pair<int32_t, int32_t>>> keyed = codec::unboundedMap(Int, Int);
  const DataResult<std::vector<std::pair<int32_t, int32_t>>> keyFailure =
      keyed.parse(JsonOps::INSTANCE, json(R"({"a":1})"));
  ASSERT_TRUE(keyFailure.isError());
  EXPECT_EQ(keyFailure.message(), "Not a number: \"a\" missed input: {\"a\":1}");
}

TEST(RangeCodecTest, IntRangeChecksBounds) {
  const Codec<int32_t> codec = codec::intRange(-5, 5);
  EXPECT_EQ(decode(codec, "3"), 3);
  EXPECT_EQ(decode(codec, "-5"), -5);
  const DataResult<int32_t> out = codec.parse(JsonOps::INSTANCE, json("6"));
  ASSERT_TRUE(out.isError());
  EXPECT_EQ(out.message(), "Value 6 outside of range [-5:5]");
  EXPECT_EQ(out.valueOrPartial().value(), 6);
  EXPECT_EQ(codec::intRange(0, 10).parse(JsonOps::INSTANCE, json("-1")).message(),
            "Value -1 outside of range [0:10]");
}

TEST(RangeCodecTest, FloatingPointRanges) {
  EXPECT_DOUBLE_EQ(decode(codec::doubleRange(0.0, 1.0), "0.5"), 0.5);
  EXPECT_EQ(codec::doubleRange(0.0, 1.0).parse(JsonOps::INSTANCE, json("2")).message(),
            "Value 2.0 outside of range [0.0:1.0]");
  EXPECT_FLOAT_EQ(decode(codec::floatRange(0.0f, 1.0f), "0.25"), 0.25f);
}

TEST(RecursiveCodecTest, ResolvesItsSupplierOnFirstUse) {
  const Codec<std::vector<int32_t>> codec = codec::recursive<std::vector<int32_t>>(
      [] { return codec::listOf(Int); });
  EXPECT_EQ(decode(codec, "[1,2]"), std::vector<int32_t>({1, 2}));
  EXPECT_EQ(encode(codec, {1, 2}), "[1,2]");
}

TEST(CodecFieldTest, FieldOfReadsAndWritesAMember) {
  const MapCodec<std::string> field = String.fieldOf("name");
  EXPECT_EQ(*field.codec().parse(JsonOps::INSTANCE, json(R"({"name":"x"})")).result(), "x");

  const DataResult<std::string> missing = field.codec().parse(JsonOps::INSTANCE, json("{}"));
  ASSERT_TRUE(missing.isError());
  EXPECT_EQ(missing.message(), "No key name in MapLike[{}]");

  const std::shared_ptr<codec::RecordBuilder> builder = JsonOps::INSTANCE.mapBuilder();
  field.encode("x", JsonOps::INSTANCE, *builder);
  EXPECT_EQ(builder->build(JsonOps::INSTANCE.empty()).result()->dump(), R"({"name":"x"})");
}

TEST(CodecFieldTest, OptionalFieldTreatsInvalidValuesAsAbsent) {
  const MapCodec<std::optional<int32_t>> field = Int.optionalFieldOf("n");
  const Codec<std::optional<int32_t>> codec = field.codec();
  EXPECT_EQ(*codec.parse(JsonOps::INSTANCE, json(R"({"n":1})")).result(), 1);
  EXPECT_FALSE(codec.parse(JsonOps::INSTANCE, json("{}")).result()->has_value());
  EXPECT_FALSE(codec.parse(JsonOps::INSTANCE, json(R"({"n":"x"})")).result()->has_value());
  // An explicit JSON null is "absent" for MapLike, exactly like JsonOps.
  EXPECT_FALSE(codec.parse(JsonOps::INSTANCE, json(R"({"n":null})")).result()->has_value());
  EXPECT_EQ(encode(codec, 1), R"({"n":1})");
  EXPECT_EQ(encode(codec, std::optional<int32_t>{}), "{}");
}

TEST(CodecFieldTest, OptionalFieldWithDefaultValue) {
  const Codec<int32_t> codec = Int.optionalFieldOf("n", 7).codec();
  EXPECT_EQ(*codec.parse(JsonOps::INSTANCE, json("{}")).result(), 7);
  EXPECT_EQ(*codec.parse(JsonOps::INSTANCE, json(R"({"n":1})")).result(), 1);
  // A value equal to the default is not written back out.
  EXPECT_EQ(encode(codec, 7), "{}");
  EXPECT_EQ(encode(codec, 1), R"({"n":1})");
}

TEST(UnitCodecTest, UnitCodecsCarryNoData) {
  const Codec<int32_t> codec = Codec<int32_t>::unit(7);
  EXPECT_EQ(*codec.parse(JsonOps::INSTANCE, json("{}")).result(), 7);
  EXPECT_EQ(encode(codec, 7), "{}");

  const Codec<codec::Unit> empty = codec::codecs::Empty;
  EXPECT_TRUE(empty.parse(JsonOps::INSTANCE, json("{}")).isSuccess());
  EXPECT_EQ(encode(empty, codec::Unit{}), "{}");
}

TEST(MapCodecCombinatorTest, OrElseAndSetPartial) {
  const Codec<int32_t> fallback = Int.fieldOf("n").orElse(5).codec();
  EXPECT_EQ(*fallback.parse(JsonOps::INSTANCE, json("{}")).result(), 5);
  EXPECT_EQ(*fallback.parse(JsonOps::INSTANCE, json(R"({"n":1})")).result(), 1);

  const DataResult<int32_t> partial =
      Int.fieldOf("n").setPartial([] { return 9; }).codec().parse(JsonOps::INSTANCE, json("{}"));
  ASSERT_TRUE(partial.isError());
  EXPECT_NE(partial.message().find("No key n in"), std::string::npos);
  ASSERT_TRUE(partial.valueOrPartial().has_value());
  EXPECT_EQ(*partial.valueOrPartial(), 9);
}

TEST(MapCodecCombinatorTest, XmapAndFlatXmapOnAMapCodec) {
  struct Wrapper {
    int32_t value = 0;
    bool operator==(const Wrapper& other) const { return value == other.value; }
  };
  const Codec<Wrapper> codec =
      Int.fieldOf("n")
          .flatXmap<Wrapper>(
              [](const int32_t& raw) -> DataResult<Wrapper> {
                return DataResult<Wrapper>::success(Wrapper{raw});
              },
              [](const Wrapper& wrapper) -> DataResult<int32_t> {
                return DataResult<int32_t>::success(wrapper.value);
              })
          .codec();
  EXPECT_EQ(decode(codec, R"({"n":3})"), Wrapper{3});
  EXPECT_EQ(encode(codec, Wrapper{3}), R"({"n":3})");

  const Codec<int32_t> mapped =
      Int.fieldOf("n").xmap<int32_t>([](const int32_t& raw) { return raw * 2; },
                                     [](const int32_t& raw) { return raw / 2; })
          .codec();
  EXPECT_EQ(decode(mapped, R"({"n":4})"), 8);
  EXPECT_EQ(encode(mapped, 8), R"({"n":4})");
}

}  // namespace
