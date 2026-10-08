// Smoke tests: one fast end-to-end pass over the whole public API.
//
// test/unit/ checks components in isolation; this layer checks that they work
// together: a single document that uses every codec kind is decoded, encoded and
// re-decoded, malformed documents are rejected with a useful message, and the
// compressed and plain JsonOps modes agree on the same value.
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "codec/all.hpp"
#include "test_support.hpp"

namespace {

using codec::Codec;
using codec::DataResult;
using codec::Either;
using codec::JsonOps;
using codec::JsonValue;
using codec::codecs::Bool;
using codec::codecs::Double;
using codec::codecs::Int;
using codec::codecs::Passthrough;
using codec::codecs::String;
using codec::fieldOf;
using codec::optionalFieldOf;
using codec::recordCodec;
using codec::testing::decode;
using codec::testing::encode;
using codec::testing::json;

// ---------------------------------------------------------------------------
// A model that touches every codec kind the port ships.
// ---------------------------------------------------------------------------
struct Limits {
  int32_t minValue = 0;
  int32_t maxValue = 0;
  bool operator==(const Limits& other) const {
    return minValue == other.minValue && maxValue == other.maxValue;
  }
};

struct Shape {
  std::string kind;
  double radius = 0.0;
  double side = 0.0;
  bool operator==(const Shape& other) const {
    return kind == other.kind && radius == other.radius && side == other.side;
  }
};

struct Payload {
  std::string name;                                          // String
  int32_t count = 0;                                         // Int
  double ratio = 0.0;                                        // Double
  bool enabled = false;                                      // Bool
  std::vector<std::string> tags;                             // listOf
  std::optional<Limits> limits;                              // optionalFieldOf + nested record
  Either<int32_t, std::string> id;                           // either
  std::vector<std::pair<std::string, int32_t>> extra;        // unboundedMap
  int32_t level = 0;                                         // intRange
  JsonValue raw;                                             // Passthrough
  Shape shape;                                               // dispatch (KeyDispatchCodec)

  bool operator==(const Payload& other) const {
    return name == other.name && count == other.count && ratio == other.ratio &&
           enabled == other.enabled && tags == other.tags && limits == other.limits &&
           id == other.id && extra == other.extra && level == other.level && raw == other.raw &&
           shape == other.shape;
  }
};

Codec<Limits> limitsCodec() {
  static const Codec<Limits> codec = recordCodec<Limits>(
      fieldOf("min_value", &Limits::minValue, Int), fieldOf("max_value", &Limits::maxValue, Int));
  return codec;
}

Codec<Shape> shapeCodec() {
  static const Codec<Shape> codec = String.partialDispatch<Shape>(
      "kind",
      [](const Shape& shape) -> DataResult<std::string> {
        return DataResult<std::string>::success(shape.kind);
      },
      [](const std::string& kind) -> DataResult<Codec<Shape>> {
        if (kind == "circle") {
          return DataResult<Codec<Shape>>::success(
              recordCodec<Shape>(fieldOf("kind", &Shape::kind, String),
                                 fieldOf("radius", &Shape::radius, Double)));
        }
        if (kind == "square") {
          return DataResult<Codec<Shape>>::success(
              recordCodec<Shape>(fieldOf("kind", &Shape::kind, String),
                                 fieldOf("side", &Shape::side, Double)));
        }
        return DataResult<Codec<Shape>>::error("Unknown shape kind: " + kind);
      });
  return codec;
}

Codec<Payload> payloadCodec() {
  static const Codec<Payload> codec = recordCodec<Payload>(
      fieldOf("name", &Payload::name, String), fieldOf("count", &Payload::count, Int),
      fieldOf("ratio", &Payload::ratio, Double), fieldOf("enabled", &Payload::enabled, Bool),
      fieldOf("tags", &Payload::tags, codec::listOf(String)),
      optionalFieldOf("limits", &Payload::limits, limitsCodec()),
      fieldOf("id", &Payload::id, codec::either(Int, String)),
      fieldOf("extra", &Payload::extra, codec::unboundedMap(String, Int)),
      fieldOf("level", &Payload::level, codec::intRange(0, 10)),
      fieldOf("raw", &Payload::raw, Passthrough), fieldOf("shape", &Payload::shape, shapeCodec()));
  return codec;
}

const char* kDocument() {
  return R"({"name":"sample","count":3,"ratio":0.25,"enabled":true,"tags":["a","b"],)"
         R"("limits":{"min_value":0,"max_value":10},"id":42,"extra":{"x":1,"y":2},)"
         R"("level":5,"raw":{"any":["json",1]},"shape":{"kind":"circle","radius":1.5}})";
}

// ---------------------------------------------------------------------------
// Smoke checks
// ---------------------------------------------------------------------------
TEST(SmokeTest, DecodesEveryCodecKind) {
  const Payload payload = decode(payloadCodec(), kDocument());
  EXPECT_EQ(payload.name, "sample");
  EXPECT_EQ(payload.count, 3);
  EXPECT_DOUBLE_EQ(payload.ratio, 0.25);
  EXPECT_TRUE(payload.enabled);
  EXPECT_EQ(payload.tags, std::vector<std::string>({"a", "b"}));
  ASSERT_TRUE(payload.limits.has_value());
  EXPECT_EQ(payload.limits->maxValue, 10);
  ASSERT_TRUE(payload.id.isLeft());
  EXPECT_EQ(payload.id.left(), 42);
  EXPECT_EQ(payload.extra.size(), 2u);
  EXPECT_EQ(payload.extra[1].first, "y");
  EXPECT_EQ(payload.level, 5);
  EXPECT_EQ(payload.raw.dump(), R"({"any":["json",1]})");
  EXPECT_EQ(payload.shape.kind, "circle");
  EXPECT_DOUBLE_EQ(payload.shape.radius, 1.5);
}

TEST(SmokeTest, ReEncodesTheSameDocument) {
  const Payload payload = decode(payloadCodec(), kDocument());
  // The record declaration order mirrors the document, so the compact encoding
  // is byte-for-byte identical.
  EXPECT_EQ(encode(payloadCodec(), payload), kDocument());
}

TEST(SmokeTest, RoundTripIsStable) {
  const Payload first = decode(payloadCodec(), kDocument());
  const std::string encoded = encode(payloadCodec(), first);
  const Payload second = decode(payloadCodec(), encoded);
  EXPECT_EQ(first, second);
  EXPECT_EQ(encode(payloadCodec(), second), encoded);
}

TEST(SmokeTest, HandlesEveryEitherAlternativeAndShapeBranch) {
  Payload payload = decode(payloadCodec(), kDocument());
  payload.id = Either<int32_t, std::string>::right("text-id");
  payload.shape = Shape{"square", 0.0, 2.0};
  payload.limits.reset();

  const std::string encoded = encode(payloadCodec(), payload);
  EXPECT_NE(encoded.find(R"("id":"text-id")"), std::string::npos) << encoded;
  EXPECT_NE(encoded.find(R"("shape":{"kind":"square","side":2.0})"), std::string::npos) << encoded;
  EXPECT_EQ(encoded.find("\"limits\""), std::string::npos) << encoded;
  EXPECT_EQ(decode(payloadCodec(), encoded), payload);
}

TEST(SmokeTest, CompressedAndPlainOpsAgree) {
  const Payload payload = decode(payloadCodec(), kDocument());

  const DataResult<JsonValue> compressed =
      payloadCodec().encodeStart(JsonOps::COMPRESSED, payload);
  ASSERT_TRUE(compressed.result().has_value()) << compressed.message();
  // Compressed records become key-indexed lists.
  EXPECT_TRUE(compressed.result()->isArray()) << compressed.result()->dump();

  const Payload restored = decode(payloadCodec(), compressed.result()->dump(), JsonOps::COMPRESSED);
  EXPECT_EQ(restored, payload);

  // Compressed ops expect a list, not an object.
  const DataResult<Payload> mismatch =
      payloadCodec().parse(JsonOps::COMPRESSED, json(kDocument()));
  ASSERT_TRUE(mismatch.isError());
  EXPECT_EQ(mismatch.message(), "Input is not a list");
}

TEST(SmokeTest, RejectsMalformedDocuments) {
  const std::vector<std::pair<std::string, std::string>> cases = {
      {R"({"count":1})", "No key name in"},
      {R"({"name":1})", "Not a string: 1"},
      {R"({"name":"x","count":"many"})", "Not a number: \"many\""},
      {R"({"name":"x","count":1,"level":99})", "Value 99 outside of range [0:10]"},
      {R"({"name":"x","count":1,"tags":{}})", "Not a json array: {}"},
      {R"({"name":"x","count":1,"shape":{"kind":"blob"}})", "Unknown shape kind: blob"},
      {R"({"name":"x","count":1,"extra":{"a":"x"}})",
       "Not a number: \"x\" missed input: {\"a\":\"x\"}"},
      {R"({"name":"x","count":1,"raw":null})", "No key raw in"},
  };

  for (const auto& testCase : cases) {
    const DataResult<Payload> result = payloadCodec().parse(JsonOps::INSTANCE, json(testCase.first));
    ASSERT_TRUE(result.isError()) << "expected a failure for " << testCase.first;
    EXPECT_NE(result.message().find(testCase.second), std::string::npos)
        << "input: " << testCase.first << "\nmessage: " << result.message();
  }
}

TEST(SmokeTest, FailedDecodeStillYieldsTheGoodFields) {
  const DataResult<Payload> result = payloadCodec().parse(
      JsonOps::INSTANCE,
      json(R"({"name":"sample","count":3,"ratio":0.5,"enabled":true,"tags":["a"],)"
           R"("id":"text","extra":{"x":1},"level":99,"raw":true,)"
           R"("shape":{"kind":"square","side":2.0}})"));

  ASSERT_TRUE(result.isError());
  EXPECT_EQ(result.message(), "Value 99 outside of range [0:10]");
  ASSERT_TRUE(result.valueOrPartial().has_value());
  const Payload& partial = *result.valueOrPartial();
  EXPECT_EQ(partial.name, "sample");
  EXPECT_EQ(partial.count, 3);
  EXPECT_EQ(partial.level, 99);
  EXPECT_EQ(partial.tags, std::vector<std::string>({"a"}));
  ASSERT_TRUE(partial.id.isRight());
  EXPECT_EQ(partial.shape.kind, "square");
}

}  // namespace
