// Tests for the KeyDispatchCodec port (Codec.dispatch / partialDispatch /
// dispatchMap), including the MapCodecCodec "flat" encoding form.
#include "test_support.hpp"

namespace {

using codec::Codec;
using codec::DataResult;
using codec::JsonOps;
using codec::JsonValue;
using codec::Value;
using codec::codecs::Double;
using codec::codecs::String;
using codec::fieldOf;
using codec::recordCodec;
using codec::testing::decode;
using codec::testing::decodeError;
using codec::testing::encode;
using codec::testing::json;

struct Circle {
  double radius = 0.0;
  bool operator==(const Circle& other) const { return radius == other.radius; }
};

struct Square {
  double side = 0.0;
  bool operator==(const Square& other) const { return side == other.side; }
};

using Shape = std::variant<Circle, Square>;

// Each shape codec is xmapped into the variant.  The result is NOT backed by a
// MapCodec, so the dispatch codec stores the payload under "value" -- exactly
// what DFU's `c instanceof MapCodecCodec` check produces.
Codec<Shape> shapeCodec() {
  static const Codec<Shape> codec = String.partialDispatch<Shape>(
      "type",
      [](const Shape& shape) -> DataResult<std::string> {
        return DataResult<std::string>::success(std::holds_alternative<Circle>(shape) ? "circle"
                                                                                      : "square");
      },
      [](const std::string& key) -> DataResult<Codec<Shape>> {
        if (key == "circle") {
          return DataResult<Codec<Shape>>::success(
              recordCodec<Circle>(fieldOf("radius", &Circle::radius, Double))
                  .xmap<Shape>([](const Circle& circle) { return Shape(circle); },
                               [](const Shape& shape) { return std::get<Circle>(shape); }));
        }
        if (key == "square") {
          return DataResult<Codec<Shape>>::success(
              recordCodec<Square>(fieldOf("side", &Square::side, Double))
                  .xmap<Shape>([](const Square& square) { return Shape(square); },
                               [](const Shape& shape) { return std::get<Square>(shape); }));
        }
        return DataResult<Codec<Shape>>::error("Unknown shape type: " + key);
      });
  return codec;
}

// A tagged record: the selected codec IS a MapCodecCodec, so the dispatch codec
// merges the payload into the same object as the type key.
struct Tag {
  std::string type;
  std::string label;
  bool operator==(const Tag& other) const {
    return type == other.type && label == other.label;
  }
};

Codec<Tag> tagCodec() {
  static const Codec<Tag> codec = String.dispatch<Tag>(
      "type", [](const Tag& tag) { return tag.type; },
      [](const std::string&) {
        // recordCodec(...) is a MapCodecCodec, so the payload is merged into
        // the same object as the type key.
        return recordCodec<Tag>(fieldOf("type", &Tag::type, String),
                                fieldOf("label", &Tag::label, String));
      });
  return codec;
}

TEST(DispatchCodecTest, DecodesTheSelectedSubCodec) {
  const Shape shape = decode(shapeCodec(), R"({"type":"circle","value":{"radius":1.5}})");
  ASSERT_TRUE(std::holds_alternative<Circle>(shape));
  EXPECT_DOUBLE_EQ(std::get<Circle>(shape).radius, 1.5);

  const Shape square = decode(shapeCodec(), R"({"type":"square","value":{"side":2}})");
  ASSERT_TRUE(std::holds_alternative<Square>(square));
  EXPECT_DOUBLE_EQ(std::get<Square>(square).side, 2.0);
}

TEST(DispatchCodecTest, EncodesTypeAndPayload) {
  EXPECT_EQ(encode(shapeCodec(), Shape(Circle{1.5})), R"({"type":"circle","value":{"radius":1.5}})");
  EXPECT_EQ(encode(shapeCodec(), Shape(Square{2.0})), R"({"type":"square","value":{"side":2.0}})");
}

TEST(DispatchCodecTest, RoundTrips) {
  const std::vector<Shape> shapes{Shape(Circle{1.0}), Shape(Square{3.0})};
  for (const Shape& shape : shapes) {
    EXPECT_EQ(decode(shapeCodec(), encode(shapeCodec(), shape)), shape);
  }
}

TEST(DispatchCodecTest, UnknownTypeIsAnError) {
  const std::string error =
      decodeError<Shape>(shapeCodec(), R"({"type":"triangle","value":{}})");
  EXPECT_EQ(error, "Unknown shape type: triangle");
}

TEST(DispatchCodecTest, MissingTypeKeyIsAnError) {
  const std::string error = decodeError<Shape>(shapeCodec(), R"({"value":{"radius":1}})");
  EXPECT_NE(error.find("Input does not contain a key [type]"), std::string::npos) << error;
}

TEST(DispatchCodecTest, MissingValueEntryIsAnError) {
  const std::string error = decodeError<Shape>(shapeCodec(), R"({"type":"circle"})");
  EXPECT_NE(error.find("Input does not have a \"value\" entry"), std::string::npos) << error;
}

TEST(DispatchCodecTest, SubCodecFailureIsReported) {
  const std::string error =
      decodeError<Shape>(shapeCodec(), R"({"type":"circle","value":{"radius":"x"}})");
  EXPECT_EQ(error, "Not a number: \"x\"");
}

TEST(DispatchCodecTest, MapCodecCodecPayloadSharesTheOuterObject) {
  const Tag tag{"t", "L"};
  EXPECT_EQ(encode(tagCodec(), tag), R"({"type":"t","label":"L"})");
  EXPECT_EQ(decode(tagCodec(), R"({"type":"t","label":"L"})"), tag);
}

TEST(DispatchCodecTest, DispatchMapReturnsAMapCodec) {
  const auto mapCodec = String.dispatchMap<Tag>(
      "type", [](const Tag& tag) { return tag.type; },
      [](const std::string&) {
        return recordCodec<Tag>(fieldOf("type", &Tag::type, String),
                                fieldOf("label", &Tag::label, String));
      });
  const codec::JsonObjectMapLike map(json(R"({"type":"t","label":"L"})"));
  const DataResult<Tag> decoded = mapCodec.decode(JsonOps::INSTANCE, map);
  ASSERT_TRUE(decoded.result().has_value());
  EXPECT_EQ(*decoded.result(), Tag({"t", "L"}));

  // The map codec can also be turned into a full codec.
  EXPECT_EQ(decode(mapCodec.codec(), R"({"type":"t","label":"L"})"), Tag({"t", "L"}));
}

TEST(DispatchCodecTest, CompressedDispatchUsesTypeAndValueSlots) {
  const DataResult<Value> encoded =
      shapeCodec().encodeStart(JsonOps::COMPRESSED, Shape(Circle{1.5}));
  ASSERT_TRUE(encoded.result().has_value());
  // In compressed mode the dispatch codec writes a two-entry list, and the
  // payload is itself compressed by the selected record codec.
  EXPECT_EQ(encoded.result()->asJson().dump(), R"(["circle",[1.5]])");
  EXPECT_EQ(decode(shapeCodec(), R"(["circle",[1.5]])", JsonOps::COMPRESSED),
            Shape(Circle{1.5}));
}

}  // namespace
