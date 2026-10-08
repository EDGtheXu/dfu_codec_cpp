// Tests for DynamicOps, JsonOps, MapLike, the record/list builders and the
// compressed-map support (KeyCompressor).
#include "test_support.hpp"

namespace {

using codec::DataResult;
using codec::JsonOps;
using codec::JsonValue;
using codec::Value;
using codec::KeyCompressor;
using codec::Lifecycle;
using codec::MapLike;
using codec::MapLikePtr;
using codec::RecordBuilder;
using codec::codecs::Int;
using codec::codecs::String;
using codec::fieldOf;
using codec::record;
using codec::testing::decode;
using codec::testing::json;

struct Point {
  int32_t x = 0;
  int32_t y = 0;
  bool operator==(const Point& other) const { return x == other.x && y == other.y; }
};

codec::Codec<Point> pointCodec() {
  static const codec::Codec<Point> codec =
      record<Point>(fieldOf("x", &Point::x, Int), fieldOf("y", &Point::y, Int));
  return codec;
}

TEST(DynamicOpsTest, EmptyValues) {
  EXPECT_TRUE(jsonView(JsonOps::INSTANCE.empty()).isNull());
  EXPECT_EQ(jsonView(JsonOps::INSTANCE.emptyMap()).dump(), "{}");
  EXPECT_EQ(jsonView(JsonOps::INSTANCE.emptyList()).dump(), "[]");
  EXPECT_EQ(jsonView(JsonOps::INSTANCE.convertTo(JsonOps::INSTANCE, json(R"({"a":1})"))).dump(),
            R"({"a":1})");
}

TEST(DynamicOpsTest, PrimitiveAccessors) {
  EXPECT_EQ(*JsonOps::INSTANCE.getNumberValue(json("3")).result(), codec::Number::ofInt(3));
  EXPECT_TRUE(JsonOps::INSTANCE.getNumberValue(json("true")).result().has_value());
  EXPECT_TRUE(JsonOps::INSTANCE.getNumberValue(json("\"x\"")).isError());
  EXPECT_EQ(JsonOps::INSTANCE.getNumberValue(json("\"x\""), codec::Number::ofInt(9)).intValue(), 9);

  EXPECT_EQ(*JsonOps::INSTANCE.getStringValue(json("\"x\"")).result(), "x");
  EXPECT_TRUE(JsonOps::INSTANCE.getStringValue(json("1")).isError());

  EXPECT_FALSE(*JsonOps::INSTANCE.getBooleanValue(json("0")).result());
  EXPECT_TRUE(JsonOps::INSTANCE.getBooleanValue(json("\"true\"")).isError());
}

TEST(DynamicOpsTest, MergeToList) {
  const DataResult<Value> fresh = JsonOps::INSTANCE.mergeToList(JsonOps::INSTANCE.empty(), json("1"));
  ASSERT_TRUE(fresh.result().has_value());
  EXPECT_EQ(jsonView(*fresh.result()).dump(), "[1]");

  const DataResult<Value> appended = JsonOps::INSTANCE.mergeToList(json("[1]"), json("2"));
  ASSERT_TRUE(appended.result().has_value());
  EXPECT_EQ(jsonView(*appended.result()).dump(), "[1,2]");

  const DataResult<Value> many =
      JsonOps::INSTANCE.mergeToList(json("[1]"), std::vector<Value>{json("2"), json("3")});
  ASSERT_TRUE(many.result().has_value());
  EXPECT_EQ(jsonView(*many.result()).dump(), "[1,2,3]");

  const DataResult<Value> failure = JsonOps::INSTANCE.mergeToList(json(R"({"a":1})"), json("2"));
  ASSERT_TRUE(failure.isError());
  EXPECT_EQ(failure.message(), R"(mergeToList called with not a list: {"a":1})");
  EXPECT_EQ(jsonView(*failure.valueOrPartial()).dump(), R"({"a":1})");
}

TEST(DynamicOpsTest, MergeToMap) {
  const DataResult<Value> fresh =
      JsonOps::INSTANCE.mergeToMap(JsonOps::INSTANCE.empty(), json("\"a\""), json("1"));
  ASSERT_TRUE(fresh.result().has_value());
  EXPECT_EQ(jsonView(*fresh.result()).dump(), R"({"a":1})");

  const DataResult<Value> appended =
      JsonOps::INSTANCE.mergeToMap(json(R"({"a":1})"), json("\"b\""), json("2"));
  ASSERT_TRUE(appended.result().has_value());
  EXPECT_EQ(jsonView(*appended.result()).dump(), R"({"a":1,"b":2})");

  const DataResult<Value> notAMap =
      JsonOps::INSTANCE.mergeToMap(json("[1]"), json("\"a\""), json("1"));
  ASSERT_TRUE(notAMap.isError());
  EXPECT_EQ(notAMap.message(), "mergeToMap called with not a map: [1]");

  const DataResult<Value> badKey =
      JsonOps::INSTANCE.mergeToMap(JsonOps::INSTANCE.empty(), json("1"), json("1"));
  ASSERT_TRUE(badKey.isError());
  EXPECT_EQ(badKey.message(), "key is not a string: 1");
}

TEST(DynamicOpsTest, MapViewFiltersNullsOnGetButNotOnEntries) {
  const JsonValue object = json(R"({"a":null,"b":1})");
  const DataResult<MapLikePtr> map = JsonOps::INSTANCE.getMap(object);
  ASSERT_TRUE(map.result().has_value());
  // JsonOps.getMap returns null for an explicit JSON null member...
  EXPECT_FALSE((*map.result())->get("a").has_value());
  ASSERT_TRUE((*map.result())->get("b").has_value());
  EXPECT_EQ(jsonView(*(*map.result())->get("b")).dump(), "1");
  EXPECT_FALSE((*map.result())->get("missing").has_value());
  // ... but entries() still reports it.
  EXPECT_EQ((*map.result())->entries().size(), 2u);
  EXPECT_NE((*map.result())->toString().find("MapLike["), std::string::npos);

  const DataResult<MapLikePtr> notAnObject = JsonOps::INSTANCE.getMap(json("[1]"));
  ASSERT_TRUE(notAnObject.isError());
  EXPECT_EQ(notAnObject.message(), "Not a JSON object: [1]");
}

TEST(DynamicOpsTest, StreamAndMapValues) {
  const DataResult<std::vector<Value>> stream = JsonOps::INSTANCE.getStream(json("[1,2]"));
  ASSERT_TRUE(stream.result().has_value());
  EXPECT_EQ(stream.result()->size(), 2u);

  const DataResult<std::vector<Value>> notAList = JsonOps::INSTANCE.getStream(json("{}"));
  ASSERT_TRUE(notAList.isError());
  EXPECT_EQ(notAList.message(), "Not a json array: {}");

  const DataResult<std::vector<std::pair<Value, Value>>> values =
      JsonOps::INSTANCE.getMapValues(json(R"({"a":1})"));
  ASSERT_TRUE(values.result().has_value());
  EXPECT_EQ(jsonView(values.result()->at(0).first).dump(), "\"a\"");
  EXPECT_EQ(jsonView(values.result()->at(0).second).dump(), "1");
}

TEST(DynamicOpsTest, GenericGetSetUpdateRemove) {
  const JsonValue object = json(R"({"a":1,"b":2})");
  ASSERT_TRUE(JsonOps::INSTANCE.get(object, "a").result().has_value());
  EXPECT_EQ(jsonView(*JsonOps::INSTANCE.get(object, "a").result()).dump(), "1");

  const DataResult<Value> missing = JsonOps::INSTANCE.get(object, "zz");
  ASSERT_TRUE(missing.isError());
  EXPECT_EQ(missing.message(), "No element \"zz\" in the map {\"a\":1,\"b\":2}");

  EXPECT_EQ(jsonView(JsonOps::INSTANCE.set(object, "c", json("3"))).dump(), R"({"a":1,"b":2,"c":3})");
  EXPECT_EQ(jsonView(JsonOps::INSTANCE.update(object, "a", [](const Value& value) {
              return JsonValue::number(jsonView(value).asNumber().intValue() + 10);
            })).dump(),
            R"({"a":11,"b":2})");
  EXPECT_EQ(jsonView(JsonOps::INSTANCE.update(object, "missing",
                                     [](const Value& value) { return value; })).dump(),
            R"({"a":1,"b":2})");
  EXPECT_EQ(jsonView(JsonOps::INSTANCE.remove(object, "a")).dump(), R"({"b":2})");
  EXPECT_EQ(jsonView(JsonOps::INSTANCE.remove(json("[1]"), "a")).dump(), "[1]");
}

TEST(DynamicOpsTest, RecordBuilderBuildsObjects) {
  const std::shared_ptr<RecordBuilder> builder = JsonOps::INSTANCE.mapBuilder();
  builder->add(std::string("a"), json("1"));
  builder->add(std::string("b"), DataResult<Value>::success(json("2")));
  const DataResult<Value> built = builder->build(JsonOps::INSTANCE.empty());
  ASSERT_TRUE(built.result().has_value());
  EXPECT_EQ(jsonView(*built.result()).dump(), R"({"a":1,"b":2})");

  // A failing field value is carried into the builder result.
  const std::shared_ptr<RecordBuilder> failing = JsonOps::INSTANCE.mapBuilder();
  failing->add(std::string("a"), DataResult<Value>::error("bad value"));
  const DataResult<Value> failed = failing->build(JsonOps::INSTANCE.empty());
  ASSERT_TRUE(failed.isError());
  EXPECT_EQ(failed.message(), "bad value");

  // Appending to an existing object keeps the original members first.
  const std::shared_ptr<RecordBuilder> merged = JsonOps::INSTANCE.mapBuilder();
  merged->add(std::string("b"), json("2"));
  const DataResult<Value> mergedResult = merged->build(json(R"({"a":1})"));
  ASSERT_TRUE(mergedResult.result().has_value());
  EXPECT_EQ(jsonView(*mergedResult.result()).dump(), R"({"a":1,"b":2})");

  // ... but refuses a non-object prefix.
  const std::shared_ptr<RecordBuilder> badPrefix = JsonOps::INSTANCE.mapBuilder();
  badPrefix->add(std::string("b"), json("2"));
  const DataResult<Value> badResult = badPrefix->build(json("[1]"));
  ASSERT_TRUE(badResult.isError());
  EXPECT_EQ(badResult.message(), "mergeToMap called with not a map: [1]");
}

TEST(DynamicOpsTest, RecordBuilderWithErrorsFrom) {
  const std::shared_ptr<RecordBuilder> builder = JsonOps::INSTANCE.mapBuilder();
  builder->withErrorsFrom(DataResult<codec::Unit>::error("external failure"));
  const DataResult<Value> built = builder->build(JsonOps::INSTANCE.empty());
  ASSERT_TRUE(built.isError());
  EXPECT_EQ(built.message(), "external failure");
}

TEST(DynamicOpsTest, RecordBuilderMapErrorAndLifecycle) {
  std::shared_ptr<RecordBuilder> builder = JsonOps::INSTANCE.mapBuilder();
  builder->add(std::string("a"), DataResult<Value>::error("bad"));
  builder->mapError([](const std::string& message) { return "[" + message + "]"; });
  const DataResult<Value> built = builder->build(JsonOps::INSTANCE.empty());
  ASSERT_TRUE(built.isError());
  EXPECT_EQ(built.message(), "[bad]");

  // A failing builder keeps the lifecycle set on it...
  std::shared_ptr<RecordBuilder> failure = JsonOps::INSTANCE.mapBuilder();
  failure->add(std::string("a"), DataResult<Value>::error("bad"));
  failure->setLifecycle(Lifecycle::stable());
  const DataResult<Value> failedBuild = failure->build(JsonOps::INSTANCE.empty());
  ASSERT_TRUE(failedBuild.isError());
  EXPECT_TRUE(failedBuild.lifecycle().isStable());

  // ... while a successful build reports the JsonOps merge lifecycle
  // (experimental), because JsonRecordBuilder#build returns success(object).
  std::shared_ptr<RecordBuilder> lifecycleBuilder = JsonOps::INSTANCE.mapBuilder();
  lifecycleBuilder->add(std::string("a"), json("1"));
  lifecycleBuilder->setLifecycle(Lifecycle::stable());
  const DataResult<Value> successBuild = lifecycleBuilder->build(JsonOps::INSTANCE.empty());
  ASSERT_TRUE(successBuild.result().has_value());
  EXPECT_TRUE(successBuild.lifecycle().isExperimental());
}

TEST(DynamicOpsTest, RecordBuilderRejectsNonStringKeys) {
  const std::shared_ptr<RecordBuilder> builder = JsonOps::INSTANCE.mapBuilder();
  builder->add(json("1"), json("2"));
  const DataResult<Value> built = builder->build(JsonOps::INSTANCE.empty());
  ASSERT_TRUE(built.isError());
  EXPECT_EQ(built.message(), "Not a string: 1");
}

TEST(DynamicOpsTest, ListBuilderBuildsArrays) {
  const std::shared_ptr<codec::ListBuilder> builder = JsonOps::INSTANCE.listBuilder();
  builder->add(json("1"));
  builder->add(DataResult<Value>::success(json("2")));
  const DataResult<Value> built = builder->build(JsonOps::INSTANCE.empty());
  ASSERT_TRUE(built.result().has_value());
  EXPECT_EQ(jsonView(*built.result()).dump(), "[1,2]");

  const std::shared_ptr<codec::ListBuilder> failing = JsonOps::INSTANCE.listBuilder();
  failing->add(DataResult<Value>::error("bad element"));
  EXPECT_EQ(failing->build(JsonOps::INSTANCE.empty()).message(), "bad element");

  const std::shared_ptr<codec::ListBuilder> badPrefix = JsonOps::INSTANCE.listBuilder();
  badPrefix->add(json("1"));
  const DataResult<Value> badResult = badPrefix->build(json(R"({"a":1})"));
  ASSERT_TRUE(badResult.isError());
  EXPECT_EQ(badResult.message(), R"(Cannot append a list to not a list: {"a":1})");
}

TEST(KeyCompressorTest, AssignsDenseIndicesInKeyOrder) {
  const std::vector<Value> keys{JsonOps::INSTANCE.createString("a"),
                                JsonOps::INSTANCE.createString("b")};
  const KeyCompressor compressor(JsonOps::INSTANCE, keys);
  EXPECT_EQ(compressor.size(), 2);
  EXPECT_EQ(compressor.compress("a"), 0);
  EXPECT_EQ(compressor.compress("b"), 1);
  EXPECT_EQ(compressor.compress(json("\"b\"")), 1);
  EXPECT_EQ(compressor.compress("unknown"), -1);
  ASSERT_TRUE(compressor.decompress(1) != nullptr);
  EXPECT_EQ(jsonView(*compressor.decompress(1)).dump(), "\"b\"");
  EXPECT_EQ(compressor.decompress(5), nullptr);
  EXPECT_EQ(compressor.decompress(-1), nullptr);
}

TEST(KeyCompressorTest, DeduplicatesRepeatedKeys) {
  const std::vector<Value> keys{JsonOps::INSTANCE.createString("a"),
                                JsonOps::INSTANCE.createString("a"),
                                JsonOps::INSTANCE.createString("b")};
  const KeyCompressor compressor(JsonOps::INSTANCE, keys);
  EXPECT_EQ(compressor.size(), 2);
}

TEST(CompressedMapsTest, OpsReportsCompression) {
  EXPECT_FALSE(JsonOps::INSTANCE.compressMaps());
  EXPECT_TRUE(JsonOps::COMPRESSED.compressMaps());
}

TEST(CompressedMapsTest, RecordEncodesAndDecodesAsAKeyedList) {
  const Point point{1, 2};
  const DataResult<Value> encoded = pointCodec().encodeStart(JsonOps::COMPRESSED, point);
  ASSERT_TRUE(encoded.result().has_value());
  EXPECT_EQ(jsonView(*encoded.result()).dump(), "[1,2]");

  const Point decoded = decode(pointCodec(), "[1,2]", JsonOps::COMPRESSED);
  EXPECT_EQ(decoded, point);

  // Uncompressed ops still produce a readable object.
  EXPECT_EQ(codec::testing::encode(pointCodec(), point, JsonOps::INSTANCE), R"({"x":1,"y":2})");
}

TEST(CompressedMapsTest, MissingCompressedEntryIsReportedAsAMissingKey) {
  const DataResult<Point> result = pointCodec().parse(JsonOps::COMPRESSED, json("[1,null]"));
  ASSERT_TRUE(result.isError());
  EXPECT_NE(result.message().find("No key y in"), std::string::npos) << result.message();
}

TEST(CompressedMapsTest, NonListInputFails) {
  const DataResult<Point> result = pointCodec().parse(JsonOps::COMPRESSED, json(R"({"x":1})"));
  ASSERT_TRUE(result.isError());
  EXPECT_EQ(result.message(), "Input is not a list");
}

}  // namespace
