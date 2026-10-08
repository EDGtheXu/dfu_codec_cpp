// Conversions between JSON scalars and C++ enums / numbers / strings.
//
// The Codec layer has no dedicated "enum codec": every direction below is built
// from the same two combinators, so this file is the cookbook.
//
//   JSON number  ->  C++ enum class   Int.flatXmap<E> / Int.xmap<E>
//   JSON string  ->  C++ enum class   codecs::stringEnum<E> (or String.flatXmap<E>)
//   JSON string  ->  C++ number       String.flatXmap<int32_t>
//   JSON number  ->  C++ string       Int.xmap<std::string>
//   JSON number *or* string -> number either(Int, String).flatXmap<int32_t>
//
// `xmap` is for total, bijective mappings; `flatXmap` is for mappings that can
// fail and therefore has to produce a DataResult with a message.
#include <charconv>
#include <optional>
#include <string>
#include <vector>

#include "codec.hpp"
#include <gtest/gtest.h>

namespace {

using codec::Codec;
using codec::DataResult;
using codec::Either;
using codec::JsonOps;
using codec::JsonValue;
using codec::codecs::Int;
using codec::codecs::String;

enum class Level { Low, High };
enum class Severity { Low, Medium, Critical };

// --- JSON number <-> enum class --------------------------------------------
Codec<Level> levelCodec() {
  return Int.flatXmap<Level>(
      [](const int32_t& value) -> DataResult<Level> {
        switch (value) {
          case 0:
            return DataResult<Level>::success(Level::Low);
          case 1:
            return DataResult<Level>::success(Level::High);
          default:
            return DataResult<Level>::error("Unknown level: " + std::to_string(value));
        }
      },
      [](const Level& value) -> DataResult<int32_t> {
        return DataResult<int32_t>::success(value == Level::Low ? 0 : 1);
      });
}

// --- JSON string <-> enum class, table driven ------------------------------
Codec<Severity> severityCodec() {
  static const Codec<Severity> codec = codec::codecs::stringEnum<Severity>(
      {{"low", Severity::Low}, {"medium", Severity::Medium}, {"critical", Severity::Critical}},
      "Severity");
  return codec;
}

// --- JSON string <-> enum class, written by hand (what stringEnum replaces) --
Codec<Level> levelNameCodec() {
  return String.flatXmap<Level>(
      [](const std::string& text) -> DataResult<Level> {
        if (text == "low") {
          return DataResult<Level>::success(Level::Low);
        }
        if (text == "high") {
          return DataResult<Level>::success(Level::High);
        }
        return DataResult<Level>::error("Unknown level name: \"" + text + "\"");
      },
      [](const Level& value) -> DataResult<std::string> {
        return DataResult<std::string>::success(value == Level::Low ? "low" : "high");
      });
}

// --- JSON string <-> number ------------------------------------------------
// std::stoi would accept "42abc"; from_chars is the strict C++17 parser.
DataResult<int32_t> parseInt(const std::string& text) {
  if (text.empty()) {
    return DataResult<int32_t>::error("Not a number: \"\"");
  }
  int32_t value = 0;
  const char* begin = text.data();
  const char* end = begin + text.size();
  const std::from_chars_result parsed = std::from_chars(begin, end, value);
  if (parsed.ec != std::errc() || parsed.ptr != end) {
    return DataResult<int32_t>::error("Not a number: \"" + text + "\"");
  }
  return DataResult<int32_t>::success(value);
}

Codec<int32_t> portFromStringCodec() {
  return String.flatXmap<int32_t>(
      [](const std::string& text) { return parseInt(text); },
      [](const int32_t& value) -> DataResult<std::string> {
        return DataResult<std::string>::success(std::to_string(value));
      });
}

// --- JSON number or string -> number ---------------------------------------
// Configurations in the wild write "port": 8080 and "port": "8080"; this accepts
// both and always *encodes* the canonical number form.
Codec<int32_t> lenientPortCodec() {
  return codec::either(Int, String)
      .flatXmap<int32_t>(
          [](const Either<int32_t, std::string>& value) -> DataResult<int32_t> {
            return value.isLeft() ? DataResult<int32_t>::success(value.left())
                                  : parseInt(value.right());
          },
          [](const int32_t& value) -> DataResult<Either<int32_t, std::string>> {
            return DataResult<Either<int32_t, std::string>>::success(
                Either<int32_t, std::string>::left(value));
          });
}

// --- a record with an enum field -------------------------------------------
struct Rule {
  std::string id;
  Severity severity = Severity::Low;
  bool operator==(const Rule& other) const {
    return id == other.id && severity == other.severity;
  }
};

Codec<Rule> ruleCodec() {
  static const Codec<Rule> codec = codec::recordCodec<Rule>(
      codec::fieldOf("id", &Rule::id, String),
      codec::fieldOf("severity", &Rule::severity, severityCodec()));
  return codec;
}

// --- helpers ---------------------------------------------------------------
template <class A>
DataResult<A> decode(const Codec<A>& codec, const std::string& text) {
  return codec.parse(JsonOps::INSTANCE, JsonValue::parse(text));
}

template <class A>
std::string encode(const Codec<A>& codec, const typename Codec<A>::value_type& value) {
  const DataResult<JsonValue> encoded = codec.encodeStart(JsonOps::INSTANCE, value);
  return encoded.result().has_value() ? encoded.result()->dump() : "error: " + encoded.message();
}

TEST(NumberToEnumTest, JsonNumberDecodesIntoAnEnumClass) {
  ASSERT_TRUE(decode(levelCodec(), "1").result().has_value());
  EXPECT_EQ(*decode(levelCodec(), "1").result(), Level::High);
  EXPECT_EQ(*decode(levelCodec(), "0").result(), Level::Low);
  EXPECT_EQ(encode(levelCodec(), Level::High), "1");

  const DataResult<Level> failed = decode(levelCodec(), "7");
  ASSERT_TRUE(failed.isError());
  EXPECT_EQ(failed.message(), "Unknown level: 7");
}

TEST(StringToEnumTest, JsonStringDecodesIntoAnEnumClass) {
  EXPECT_EQ(*decode(severityCodec(), "\"critical\"").result(), Severity::Critical);
  EXPECT_EQ(*decode(severityCodec(), "\"low\"").result(), Severity::Low);
  EXPECT_EQ(encode(severityCodec(), Severity::Medium), "\"medium\"");

  // The name table also drives the error message, and encoding an unmapped value
  // (a default-constructed enum, say) is reported instead of silently emitting
  // something wrong.
  const DataResult<Severity> unknown = decode(severityCodec(), "\"fatal\"");
  ASSERT_TRUE(unknown.isError());
  EXPECT_EQ(unknown.message(), "Unknown Severity: \"fatal\"");

  const DataResult<JsonValue> unmapped =
      severityCodec().encodeStart(JsonOps::INSTANCE, static_cast<Severity>(99));
  ASSERT_TRUE(unmapped.isError());
  EXPECT_EQ(unmapped.message(), "Unmapped Severity value");

  // Numbers are not names: a JSON number is rejected by the string based codec.
  const DataResult<Severity> wrongType = decode(severityCodec(), "1");
  ASSERT_TRUE(wrongType.isError());
  EXPECT_EQ(wrongType.message(), "Not a string: 1");
}

TEST(StringToEnumTest, FlatXmapIsAllStringEnumDoes) {
  EXPECT_EQ(*decode(levelNameCodec(), "\"high\"").result(), Level::High);
  EXPECT_EQ(encode(levelNameCodec(), Level::Low), "\"low\"");
  EXPECT_EQ(decode(levelNameCodec(), "\"medium\"").message(),
            "Unknown level name: \"medium\"");
}

TEST(StringToEnumTest, EnumsWorkAsRecordFields) {
  const DataResult<Rule> rule = decode(ruleCodec(), R"({"id":"R-1","severity":"critical"})");
  ASSERT_TRUE(rule.result().has_value()) << rule.message();
  EXPECT_EQ(*rule.result(), (Rule{"R-1", Severity::Critical}));
  EXPECT_EQ(encode(ruleCodec(), Rule{"R-1", Severity::Critical}),
            R"({"id":"R-1","severity":"critical"})");

  // A bad name is reported as the field error (DFU messages do not carry the
  // field name; the surrounding record joins them in declaration order).
  const DataResult<Rule> bad = decode(ruleCodec(), R"({"id":"R-1","severity":"fatal"})");
  ASSERT_TRUE(bad.isError());
  EXPECT_EQ(bad.message(), "Unknown Severity: \"fatal\"");
}

TEST(StringToEnumTest, EnumsWorkInsideLists) {
  const Codec<std::vector<Severity>> codec = codec::listOf(severityCodec());
  const DataResult<std::vector<Severity>> decoded = decode(codec, R"(["low","critical"])");
  ASSERT_TRUE(decoded.result().has_value()) << decoded.message();
  EXPECT_EQ(*decoded.result(), (std::vector<Severity>{Severity::Low, Severity::Critical}));
  EXPECT_EQ(encode(codec, std::vector<Severity>{Severity::Medium}), R"(["medium"])");

  // The list codec reports the failed element and keeps the decoded prefix.
  const DataResult<std::vector<Severity>> partial = decode(codec, R"(["low","fatal"])");
  ASSERT_TRUE(partial.isError());
  EXPECT_EQ(partial.message(), "Unknown Severity: \"fatal\"");
  ASSERT_TRUE(partial.valueOrPartial().has_value());
  EXPECT_EQ(*partial.valueOrPartial(), (std::vector<Severity>{Severity::Low}));
}

TEST(StringToNumberTest, JsonStringDecodesIntoANumber) {
  const Codec<int32_t> codec = portFromStringCodec();
  EXPECT_EQ(*decode(codec, "\"8080\"").result(), 8080);
  EXPECT_EQ(encode(codec, 8080), "\"8080\"");

  EXPECT_EQ(decode(codec, "\"80x\"").message(), "Not a number: \"80x\"");
  EXPECT_EQ(decode(codec, "\"\"").message(), "Not a number: \"\"");
  EXPECT_EQ(decode(codec, "8080").message(), "Not a string: 8080");
}

TEST(NumberToStringTest, JsonNumberDecodesIntoAString) {
  // Int.xmap<std::string>: the JSON stays a number, only the C++ type changes.
  const Codec<std::string> asDecimalText = Int.xmap<std::string>(
      [](const int32_t& value) { return std::to_string(value); },
      [](const std::string& text) { return std::stoi(text); });

  EXPECT_EQ(*decode(asDecimalText, "42").result(), "42");
  EXPECT_EQ(encode(asDecimalText, "42"), "42");
  EXPECT_EQ(decode(asDecimalText, "\"42\"").message(), "Not a number: \"42\"");
}

TEST(NumberOrStringTest, AcceptsBothFormsAndEncodesTheCanonicalOne) {
  const Codec<int32_t> codec = lenientPortCodec();
  EXPECT_EQ(*decode(codec, "8080").result(), 8080);
  EXPECT_EQ(*decode(codec, "\"8080\"").result(), 8080);
  EXPECT_EQ(encode(codec, 8080), "8080");
  EXPECT_EQ(decode(codec, "\"http\"").message(), "Not a number: \"http\"");
}

TEST(NumberToStringTest, OptionalEnumFieldsFollowTheDfuNullRules) {
  const Codec<std::optional<Severity>> codec = severityCodec().optionalFieldOf("severity").codec();
  EXPECT_EQ(*decode(codec, R"({"severity":"medium"})").result(), Severity::Medium);
  EXPECT_FALSE(decode(codec, "{}").result()->has_value());
  // A JSON null is "absent" for MapLike, like every other JsonOps codec.
  EXPECT_FALSE(decode(codec, R"({"severity":null})").result()->has_value());
  // A present but invalid name is swallowed by OptionalFieldCodec (DFU behaviour).
  EXPECT_FALSE(decode(codec, R"({"severity":"fatal"})").result()->has_value());
  EXPECT_EQ(encode(codec, Severity::Low), R"({"severity":"low"})");
  EXPECT_EQ(encode(codec, std::optional<Severity>{}), "{}");
}

}  // namespace
