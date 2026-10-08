// Error locations.
//
// DataFixerUpper messages carry no location: a bad value inside
// {"risks": [..., {"condition": {"or": [{"op": 1}]}}]} only says `Not a string: 1`.
// The port records where a failure happened, so:
//
//     result.message()   -> "Not a string: 1"                       (DFU text)
//     result.location()  -> "risks[3].condition.or[0].op"
//     result.describe()  -> "risks[3].condition.or[0].op: Not a string: 1"
//     result.report()    -> the same, plus the chain of codecs that handled it,
//                           each with the place it was built (clickable in an IDE)
//     result.throwIfError() / getOrThrow() -> throws codec::CodecError carrying all
//                           of the above (portable: std::runtime_error subclass)
//
// message() is unchanged, which is why every pre-existing assertion still holds;
// describe() is the form to show a user.  Containers attach the segment as the
// error travels outwards: fieldOf adds the key, ListCodec adds [i], unboundedMap
// adds the entry key, dispatch adds "value", and the record builders add the field
// name when encoding.
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "codec.hpp"
#include "risk_def.hpp"
#include <gtest/gtest.h>

namespace {

using codec::Codec;
using codec::CodecResultFunction;
using codec::DataResult;
using codec::DynamicOps;
using codec::FrameStyle;
using codec::JsonOps;
using codec::JsonValue;

// --- a small model with the same nesting as the reported case ---------------

// 两个小断言，代替 gmock 匹配器（本层只链接 gtest）。
void ExpectStartsWith(const std::string& text, const std::string& prefix) {
  EXPECT_EQ(text.compare(0, prefix.size(), prefix), 0) << "text: " << text;
}
void ExpectContains(const std::string& text, const std::string& needle) {
  EXPECT_NE(text.find(needle), std::string::npos) << "text: " << text;
}
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
  // Rewriting the wording keeps the location *and* the frames (see FrameTest).
  const std::vector<std::string> expected{"String",
                                          "RecordCodec[op]",
                                          "list",
                                          "RecordCodec[or]",
                                          "optional[condition]",
                                          "RecordCodec[condition]",
                                          "list",
                                          "RecordCodec[risks]"};
  EXPECT_EQ(result.frameNames(), expected);
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
  // mapError rebuilt the message but kept the entry's location and the codec chain
  // that produced it (both parts agreed, so both survive).
  EXPECT_EQ(badEntry.frameNames(), (std::vector<std::string>{"Int", "unboundedMap"}));
  ExpectStartsWith(badEntry.report(), "b: Not a number: \"x\" missed input: {\"b\":\"x\"}\n");

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
  ExpectStartsWith(flatPayload.report(), "radius: Not a number: \"x\"\n");
  EXPECT_EQ(flatPayload.frameNames(),
            (std::vector<std::string>{"Double", "RecordCodec[radius]", "dispatch[type]"}));

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
  ExpectStartsWith(nestedPayload.report(), "value.radius: Not a number: \"x\"\n");
  EXPECT_EQ(nestedPayload.frameNames(),
            (std::vector<std::string>{"Double", "RecordCodec[radius]", "dispatch[type]"}));
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
  EXPECT_EQ(result.report(), "");
}

// --- frames -----------------------------------------------------------------
//
// Every codec that handles a failure appends its own name, so report() reads like
// the stack of codecs the value passed through, innermost first.  Each frame also
// carries the place the codec was built, rendered so an IDE can click it:
//
//     msvc:  risks[3].condition.or[0].op: Not a string: 1
//              0> D:\...\error_path_test.cpp(70): String
//              1> D:\...\error_path_test.cpp(71): RecordCodec[op]
//
//     gnu:   risks[3].condition.or[0].op: Not a string: 1
//              #0 String at D:\...\error_path_test.cpp:70
//
// The names are the codecs' own short names (primitiveCodec's "Int", the record
// builder's "RecordCodec[fields]"), never the composed name a container carries
// ("ListCodec[...]"), so a chain of nested records does not repeat itself.  A
// container that receives several failures gives each part its own chain, and
// frames are only materialised while an error travels outwards -- a successful
// decode allocates nothing for them.
//
// The location comes from SourceLocation: std::source_location under C++20,
// __builtin_FILE()/__builtin_LINE() under MSVC/GCC/Clang, and "no location" when
// neither exists.  The assertions below therefore check *that* a frame has a
// location pointing into this file, never a particular line number.

// Asserts that the frames of this result carry construction sites: every frame
// has a location (library frames point into codec.hpp, user frames into this test
// file), and at least one comes from this file -- i.e. the call site of the
// user's own fieldOf/listOf/record really was captured.
template <class A>
void ExpectFramesCarryTheirConstructionSite(const DataResult<A>& result) {
  size_t frames = 0;
  size_t located = 0;
  size_t fromThisFile = 0;
  for (const codec::ErrorPart& part : result.errors()) {
    for (const codec::Frame& frame : part.frames) {
      ++frames;
      const std::string_view file = frame.where.file ? frame.where.file : "";
      if (frame.where.valid() && frame.where.line > 0) {
        ++located;
        if (file.find("error_path_test.cpp") != std::string_view::npos) {
          ++fromThisFile;
        }
      }
    }
  }
  EXPECT_GT(frames, 0u);
#if defined(CODEC_HAS_SOURCE_LOCATION) || defined(CODEC_HAS_BUILTIN_FILE)
  EXPECT_EQ(located, frames) << "every frame should carry a location";
  EXPECT_GT(fromThisFile, 0u) << "the frame built in this file should be located";
#else
  // 该编译器没有位置信息，只有名字——功能降级，但仍是合法的。
  (void)located;
  (void)fromThisFile;
#endif
}

TEST(FrameTest, ReportsTheCodecChainOfTheFailingValue) {
  const DataResult<Document> result = parseDocument(documentWithBadOp());
  ASSERT_TRUE(result.isError());

  // The chain, programmatically: frameNames() gives the names in order.
  ASSERT_EQ(result.errors().size(), 1u);
  const std::vector<std::string> expected{"String",
                                          "RecordCodec[op]",
                                          "list",
                                          "RecordCodec[or]",
                                          "optional[condition]",
                                          "RecordCodec[condition]",
                                          "list",
                                          "RecordCodec[risks]"};
  EXPECT_EQ(result.frameNames(), expected);
  ExpectFramesCarryTheirConstructionSite(result);

  // ... and rendered: first line the located message, then one line per frame
  // with its own construction site.
  ExpectStartsWith(result.report(), "risks[3].condition.or[0].op: Not a string: 1\n");
  ExpectContains(result.report(FrameStyle::gnu), "#0 String at ");
  ExpectContains(result.report(FrameStyle::gnu), "error_path_test.cpp:");
  ExpectContains(result.report(FrameStyle::msvc), "0> ");
  ExpectContains(result.report(FrameStyle::msvc), "error_path_test.cpp(");
  // describe() stays the single line form; report() is the one with frames.
  EXPECT_EQ(result.describe(), "risks[3].condition.or[0].op: Not a string: 1");
}

TEST(FrameTest, FramesPointAtTheLineThatBuiltTheCodec) {
  // The primitive "Int" is defined in codec.hpp, the list in this file: the two
  // frames of the same failure therefore point at different files.
  const Codec<std::vector<int32_t>> codec = codec::listOf(codec::codecs::Int);
  const DataResult<std::vector<int32_t>> result = decode(codec, R"([1,"x"])");
  ASSERT_TRUE(result.isError());
  ASSERT_EQ(result.errors().size(), 1u);
  ASSERT_EQ(result.errors().front().frames.size(), 2u);

#if defined(CODEC_HAS_SOURCE_LOCATION) || defined(CODEC_HAS_BUILTIN_FILE)
  const codec::Frame& intFrame = result.errors().front().frames[0];
  const codec::Frame& listFrame = result.errors().front().frames[1];
  EXPECT_EQ(intFrame.codec, "Int");
  EXPECT_EQ(listFrame.codec, "list");
  ExpectContains(std::string(intFrame.where.file), "codec.hpp");
  EXPECT_GT(intFrame.where.line, 0u);
  ExpectContains(std::string(listFrame.where.file), "error_path_test.cpp");
  EXPECT_GT(listFrame.where.line, 0u);
#endif
}

TEST(FrameTest, PrimitivesAndListsNameThemselves) {
  const Codec<std::vector<int32_t>> codec = codec::listOf(codec::codecs::Int);
  const DataResult<std::vector<int32_t>> result = decode(codec, R"([1,"x"])");
  ASSERT_TRUE(result.isError());
  EXPECT_EQ(result.frameNames(), (std::vector<std::string>{"Int", "list"}));
  ExpectStartsWith(result.report(), "[1]: Not a number: \"x\"\n");
  ExpectFramesCarryTheirConstructionSite(result);
}

TEST(FrameTest, StrictOptionalFieldsNameTheirWrapper) {
  struct Holder {
    std::optional<int32_t> n;
  };
  const Codec<Holder> codec = codec::recordCodec<Holder>(
      codec::optionalFieldOfStrict("n", &Holder::n, codec::codecs::Int));

  const DataResult<Holder> result = decode(codec, R"({"n":"x"})");
  ASSERT_TRUE(result.isError());
  EXPECT_EQ(result.frameNames(), (std::vector<std::string>{"Int", "optional[n]", "RecordCodec[n]"}));
  ExpectStartsWith(result.report(), "n: Not a number: \"x\"\n");
  ExpectFramesCarryTheirConstructionSite(result);
}

TEST(FrameTest, EveryFailedPartCarriesItsOwnFrames) {
  struct Pair {
    int32_t a = 0;
    int32_t b = 0;
  };
  const Codec<Pair> codec = codec::recordCodec<Pair>(
      codec::fieldOf("a", &Pair::a, codec::codecs::Int),
      codec::fieldOf("b", &Pair::b, codec::codecs::Int));

  const DataResult<Pair> result = decode(codec, "{}");
  ASSERT_TRUE(result.isError());
  ASSERT_EQ(result.errors().size(), 2u);
  // Two failures, one block each: the report interleaves message and frames.
  ExpectContains(result.report(), "a: No key a in MapLike[{}]\n");
  ExpectContains(result.report(), "\nb: No key b in MapLike[{}]\n");
  EXPECT_EQ(result.errors()[0].frames.size(), 1u);
  EXPECT_EQ(result.errors()[0].frames[0].codec, "RecordCodec[a, b]");
  EXPECT_EQ(result.errors()[1].frames[0].codec, "RecordCodec[a, b]");
  ExpectFramesCarryTheirConstructionSite(result);
}

TEST(FrameTest, EncodeFailuresKeepTheirCodecChain) {
  const Codec<int32_t> bounded = codec::codecs::Int.flatComapMap<int32_t>(
      [](const int32_t& value) { return value; },
      [](const int32_t& value) -> DataResult<int32_t> {
        if (value > 100) {
          return DataResult<int32_t>::error("too large to encode: " + std::to_string(value));
        }
        return DataResult<int32_t>::success(value);
      });

  const DataResult<JsonValue> encoded =
      codec::listOf(bounded).encodeStart(JsonOps::INSTANCE, std::vector<int32_t>{1, 200});
  ASSERT_TRUE(encoded.isError());
  // The rejected element has no codec of its own to name (flatComapMap replaces the
  // encoder), so the chain starts at the list.
  EXPECT_EQ(encoded.describe(), "[1]: too large to encode: 200");
  EXPECT_EQ(encoded.frameNames(), (std::vector<std::string>{"list"}));
}

// --- throwing the diagnostic (portable, no compiler builtins needed) --------

TEST(CodecErrorTest, ThrowIfErrorCarriesLocationFramesAndReport) {
  const Codec<std::vector<int32_t>> codec = codec::listOf(codec::codecs::Int);
  const DataResult<std::vector<int32_t>> result = decode(codec, R"([1,"x"])");

  try {
    result.throwIfError();
    FAIL() << "expected a CodecError";
  } catch (const codec::CodecError& error) {
    EXPECT_EQ(error.location(), "[1]");
    EXPECT_EQ(std::string(error.what()), "[1]: Not a number: \"x\"");
    ExpectContains(error.report(), "Int");
    ExpectContains(error.report(), "list");
    ASSERT_EQ(error.errors().size(), 1u);
    EXPECT_EQ(error.errors().front().message, "Not a number: \"x\"");
    EXPECT_TRUE(error.hasPartial());  // the list keeps its decoded prefix
  }
}

TEST(CodecErrorTest, IsCatchableAsAStandardException) {
  struct Holder {
    int32_t n = 0;
  };
  const Codec<Holder> codec =
      codec::recordCodec<Holder>(codec::fieldOf("n", &Holder::n, codec::codecs::Int));
  const DataResult<Holder> result = decode(codec, R"({"n":"x"})");

  // CodecError derives from std::runtime_error: existing catch blocks keep working.
  try {
    result.getOrThrow(false, [](const std::string&) {});
    FAIL() << "expected a CodecError";
  } catch (const std::runtime_error& error) {
    EXPECT_EQ(std::string(error.what()), "n: Not a number: \"x\"");
  }
}

TEST(CodecErrorTest, SuccessDoesNotThrow) {
  const DataResult<std::vector<int32_t>> result =
      decode(codec::listOf(codec::codecs::Int), "[1,2,3]");
  ASSERT_TRUE(result.isSuccess());
  EXPECT_EQ(result.throwIfError(), (std::vector<int32_t>{1, 2, 3}));
}

}  // namespace
