// Tests for the JsonValue DOM, the JSON parser and the JSON writer.
#include "test_support.hpp"

namespace {

using codec::JsonParseError;
using codec::JsonValue;
using codec::Number;

TEST(JsonValueTest, ParsesScalars) {
  EXPECT_TRUE(codec::testing::json("null").isNull());
  EXPECT_TRUE(codec::testing::json("true").asBoolean());
  EXPECT_FALSE(codec::testing::json("false").asBoolean());
  EXPECT_TRUE(codec::testing::json("\"text\"").isString());
  EXPECT_EQ(codec::testing::json("\"text\"").asString(), "text");
  EXPECT_TRUE(codec::testing::json("1").isNumber());
  EXPECT_TRUE(codec::testing::json("-12.5").isNumber());
}

TEST(JsonValueTest, KeepsIntegralAndFloatingNumbersApart) {
  const JsonValue integral = codec::testing::json("42");
  const JsonValue floating = codec::testing::json("42.0");
  EXPECT_TRUE(integral.asNumber().isIntegral());
  EXPECT_FALSE(floating.asNumber().isIntegral());
  // Gson's JsonPrimitive treats 42 and 42.0 as equal.
  EXPECT_TRUE(integral == floating);
  // ... but the literal representation is preserved.
  EXPECT_EQ(integral.dump(), "42");
  EXPECT_EQ(floating.dump(), "42.0");
}

TEST(JsonValueTest, ParsesExponentsAndLargeNumbers) {
  EXPECT_DOUBLE_EQ(codec::testing::json("1e3").asNumber().doubleValue(), 1000.0);
  EXPECT_DOUBLE_EQ(codec::testing::json("-2.5E-2").asNumber().doubleValue(), -0.025);
  // Beyond int64 range the parser falls back to a double.
  EXPECT_FALSE(codec::testing::json("99999999999999999999").asNumber().isIntegral());
  EXPECT_EQ(codec::testing::json("9223372036854775807").asNumber().longValue(),
            std::numeric_limits<int64_t>::max());
}

TEST(JsonValueTest, ParsesStringsWithEscapes) {
  // NOTE: MSVC's preprocessor rewrites raw string literals containing backslash
  // escapes when they are passed straight to a gtest macro (the escapes then get
  // re-interpreted as real escapes).  Bind such text to a local first.
  const std::string text = R"("a\tb\n\"c\"\\d\/e")";
  EXPECT_EQ(codec::testing::json(text).asString(), "a\tb\n\"c\"\\d/e");
}

TEST(JsonValueTest, ParsesUnicodeEscapesAndSurrogatePairs) {
  const std::string chinese = R"("\u4e2d\u6587")";
  EXPECT_EQ(codec::testing::json(chinese).asString(), "\xe4\xb8\xad\xe6\x96\x87");
  // U+1F600 GRINNING FACE encoded as a surrogate pair.
  const std::string emoji = R"("\ud83d\ude00")";
  EXPECT_EQ(codec::testing::json(emoji).asString(), "\xf0\x9f\x98\x80");
}

TEST(JsonValueTest, PreservesObjectInsertionOrder) {
  const JsonValue value = codec::testing::json(R"({"b":1,"a":2,"c":3})");
  ASSERT_EQ(value.size(), 3u);
  EXPECT_EQ(value.asObject()[0].first, "b");
  EXPECT_EQ(value.asObject()[1].first, "a");
  EXPECT_EQ(value.asObject()[2].first, "c");
  EXPECT_EQ(value.dump(), R"({"b":1,"a":2,"c":3})");
}

TEST(JsonValueTest, ObjectEqualityIgnoresOrderButEqualsOrderedDoesNot) {
  const JsonValue first = codec::testing::json(R"({"a":1,"b":2})");
  const JsonValue reordered = codec::testing::json(R"({"b":2,"a":1})");
  EXPECT_TRUE(first == reordered);                 // Gson (LinkedTreeMap) semantics
  EXPECT_FALSE(first.equalsOrdered(reordered));
  EXPECT_TRUE(first.equalsOrdered(codec::testing::json(R"({"a":1,"b":2})")));
}

TEST(JsonValueTest, DeepEquality) {
  EXPECT_TRUE(codec::testing::json(R"({"a":[1,{"b":null}]})") ==
              codec::testing::json(R"({ "a" : [ 1 , { "b" : null } ] })"));
  EXPECT_FALSE(codec::testing::json(R"({"a":1})") == codec::testing::json(R"({"a":"1"})"));
  EXPECT_FALSE(codec::testing::json(R"({"a":1})") == codec::testing::json(R"({"a":2})"));
  EXPECT_FALSE(codec::testing::json("[1,2]") == codec::testing::json("[2,1]"));
}

TEST(JsonValueTest, ParseErrors) {
  EXPECT_THROW(JsonValue::parse(""), JsonParseError);
  EXPECT_THROW(JsonValue::parse("{"), JsonParseError);
  EXPECT_THROW(JsonValue::parse("[1,]"), JsonParseError);
  EXPECT_THROW(JsonValue::parse("{\"a\":1} trailing"), JsonParseError);
  EXPECT_THROW(JsonValue::parse("{a:1}"), JsonParseError);
  EXPECT_THROW(JsonValue::parse("\"unterminated"), JsonParseError);
  EXPECT_THROW(JsonValue::parse("\"bad \\x escape\""), JsonParseError);
  EXPECT_THROW(JsonValue::parse("01"), JsonParseError);
  EXPECT_THROW(JsonValue::parse("1."), JsonParseError);
  EXPECT_THROW(JsonValue::parse("nul"), JsonParseError);
}

TEST(JsonValueTest, ParseErrorMentionsPosition) {
  try {
    JsonValue::parse("{\n  \"a\": oops\n}");
    FAIL() << "expected a parse error";
  } catch (const JsonParseError& error) {
    const std::string message = error.what();
    EXPECT_NE(message.find("line 2"), std::string::npos) << message;
    EXPECT_NE(message.find("column"), std::string::npos) << message;
  }
}

TEST(JsonValueTest, DumpsEscapedStrings) {
  const std::string escaped = R"("a\"b\\c\nd\te")";
  EXPECT_EQ(JsonValue::string("a\"b\\c\nd\te").dump(), escaped);
  const std::string controlCharacter = R"("\u0001")";
  EXPECT_EQ(JsonValue::string(std::string("\x01")).dump(), controlCharacter);
  // UTF-8 passes through unescaped, like Gson's default writer.
  EXPECT_EQ(JsonValue::string("\xe4\xb8\xad").dump(), "\"\xe4\xb8\xad\"");
}

TEST(JsonValueTest, DumpsPrettyPrinted) {
  const JsonValue value = codec::testing::json(R"({"a":1,"b":[true,null]})");
  const std::string pretty = value.dump(true, 2);
  EXPECT_NE(pretty.find("\n  \"a\": 1"), std::string::npos) << pretty;
  EXPECT_NE(pretty.find("\n  \"b\": ["), std::string::npos) << pretty;
}

TEST(JsonValueTest, DumpParseRoundTrip) {
  const std::string text =
      R"({"a":[1,2.5,-3,true,false,null,"x"],"b":{"c":{"d":[]}},"e":{}})";
  EXPECT_EQ(JsonValue::parse(text).dump(), text);
}

TEST(JsonValueTest, WritersEmitNullForNonFiniteNumbers) {
  EXPECT_EQ(JsonValue::number(std::numeric_limits<double>::quiet_NaN()).dump(), "null");
  EXPECT_EQ(JsonValue::number(std::numeric_limits<double>::infinity()).dump(), "null");
}

TEST(JsonValueTest, AccessorsAndTypedErrors) {
  const JsonValue object = codec::testing::json(R"({"a":1,"null":null})");
  EXPECT_EQ(object.typeName(), "object");
  EXPECT_EQ(object.size(), 2u);
  EXPECT_TRUE(object.contains("a"));
  EXPECT_FALSE(object.contains("missing"));
  ASSERT_TRUE(object.find("a").has_value());
  EXPECT_EQ(object.find("a")->asNumber().intValue(), 1);
  EXPECT_FALSE(object.find("missing").has_value());

  const JsonValue nullMember = codec::testing::json(R"({"null":null})");
  ASSERT_TRUE(nullMember.find("null").has_value());
  EXPECT_TRUE(nullMember.find("null")->isNull());
  // `find` sees the null member, `get` filters it like JsonOps' MapLike.
  EXPECT_TRUE(nullMember.contains("null"));
  EXPECT_FALSE(nullMember.get("null").has_value());

  EXPECT_THROW(object.asArray(), std::runtime_error);
  EXPECT_THROW(object.asString(), std::runtime_error);
  EXPECT_THROW(codec::testing::json("1").asObject(), std::runtime_error);
  EXPECT_THROW(codec::testing::json("1").asBoolean(), std::runtime_error);
}

TEST(NumberTest, JavaNumberNarrowingSemantics) {
  // Number.intValue() truncates, exactly like java.lang.Number.
  EXPECT_EQ(Number::ofDouble(1.7).intValue(), 1);
  EXPECT_EQ(Number::ofDouble(-1.7).intValue(), -1);
  EXPECT_EQ(Number::ofDouble(1.7).longValue(), 1);
  EXPECT_EQ(Number::ofDouble(300.9).byteValue(), 44);  // (byte) 300 == 44
  EXPECT_TRUE(Number::ofInt(1).booleanValue());
  EXPECT_FALSE(Number::ofInt(0).booleanValue());
}

TEST(NumberTest, ByteValueWrapsLikeJava) {
  // booleanValue() is byteValue() != 0, so 256 narrows to 0 and 255 to -1.
  EXPECT_FALSE(Number::ofInt(256).booleanValue());
  EXPECT_TRUE(Number::ofInt(255).booleanValue());
  EXPECT_EQ(Number::ofInt(256).byteValue(), 0);
}

TEST(NumberTest, ToString) {
  EXPECT_EQ(Number::ofInt(7).toString(), "7");
  EXPECT_EQ(Number::ofDouble(7).toString(), "7.0");
  EXPECT_EQ(Number::ofDouble(0.5).toString(), "0.5");
}

}  // namespace
