// TomlOps —— 用 tinytoml 实现的第二种 DynamicOps。
//
// 覆盖类型映射、与 JsonOps 的**按格式**差异（布尔不是数字、相等是类型严格的）、
// 字面点号键必须走 findChild、跨格式 convertTo、dumpToml 写出前的校验，以及
// 编码用的 mutable 构造器（TomlListBuilder / TomlRecordBuilder）的累加与错误语义。
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "codec_toml.hpp"
#include "test_support.hpp"
#include <gtest/gtest.h>

namespace {

using codec::DataResult;
using codec::Dynamic;
using codec::JsonOps;
using codec::JsonValue;
using codec::ListBuilder;
using codec::MapLikePtr;
using codec::Number;
using codec::RecordBuilder;
using codec::TomlDocument;
using codec::TomlOps;
using codec::Unit;
using codec::Value;
using codec::dumpToml;
using codec::parseToml;

// 解析一段 TOML 并交出根值（测试里重复很多次）。
Value parseRoot(const std::string& text) {
  const DataResult<TomlDocument> document = parseToml(text);
  if (!document.result().has_value()) {
    throw std::runtime_error("parseToml failed: " + document.message());
  }
  return document.result()->root();
}

TEST(TomlOpsTest, ReadsScalarsByType) {
  const Value root = parseRoot(
      "i = 42\n"
      "d = 1.5\n"
      "b = true\n"
      "s = \"text\"\n");
  const DataResult<MapLikePtr> map = TomlOps::INSTANCE.getMap(root);
  ASSERT_TRUE(map.result().has_value());

  const Value integer = *(*map.result())->get("i");
  EXPECT_EQ(TomlOps::INSTANCE.getNumberValue(integer).result()->intValue(), 42);

  const Value real = *(*map.result())->get("d");
  EXPECT_DOUBLE_EQ(TomlOps::INSTANCE.getNumberValue(real).result()->doubleValue(), 1.5);

  const Value flag = *(*map.result())->get("b");
  EXPECT_TRUE(TomlOps::INSTANCE.getBooleanValue(flag).result().value());

  const Value text = *(*map.result())->get("s");
  EXPECT_EQ(*TomlOps::INSTANCE.getStringValue(text).result(), "text");

  // 缺失的键在 MapLike 层就是 nullopt（与 JSON 侧一致）。
  EXPECT_FALSE((*map.result())->get("missing").has_value());
}

TEST(TomlOpsTest, BooleanIsNotANumber) {
  const Value root = parseRoot("b = true\n");
  const Value flag = *(*TomlOps::INSTANCE.getMap(root).result())->get("b");

  // TOML 的类型是严格的：布尔读不出数字 —— 这与 JsonOps 的怪癖不同
  // （JsonOps 接受布尔并把 true 读成 1，DFU 的既有行为）。
  EXPECT_TRUE(TomlOps::INSTANCE.getNumberValue(flag).isError());
  EXPECT_EQ(TomlOps::INSTANCE.getNumberValue(flag).message(), "Not a number: true");

  // 对照：同一个 true 在 JSON 侧就是数字 1；转到 TOML 之后仍然是布尔，
  // 因此依然读不出数字（convertTo 保留类型，不做隐式提升）。
  EXPECT_EQ(JsonOps::INSTANCE.getNumberValue(Value(JsonValue::boolean(true))).result()->intValue(),
            1);
  const Value converted =
      JsonOps::INSTANCE.convertTo(TomlOps::INSTANCE, Value(JsonValue::boolean(true)));
  EXPECT_TRUE(TomlOps::INSTANCE.valueEquals(converted, flag));
  EXPECT_TRUE(TomlOps::INSTANCE.getNumberValue(converted).isError());
}

TEST(TomlOpsTest, TimeIsReadAsAStringNotANumber) {
  const Value root = parseRoot("t = 1979-05-27T07:32:00Z\n");
  const Value time = *(*TomlOps::INSTANCE.getMap(root).result())->get("t");

  const DataResult<std::string> text = TomlOps::INSTANCE.getStringValue(time);
  ASSERT_TRUE(text.result().has_value());
  EXPECT_NE(text.result()->find("1979-05-27"), std::string::npos);
  EXPECT_TRUE(TomlOps::INSTANCE.getNumberValue(time).isError());
}

TEST(TomlOpsTest, LiteralDottedKeysUseFindChild) {
  // tinytoml 的 find() 把点号当路径，所以 MapLike 必须用 findChild；
  // 引号键 "a.b" 是**一个字面键**，不是路径。
  const Value root = parseRoot("\"a.b\" = 1\n");
  const DataResult<MapLikePtr> map = TomlOps::INSTANCE.getMap(root);
  ASSERT_TRUE(map.result().has_value());

  ASSERT_TRUE((*map.result())->get("a.b").has_value());
  EXPECT_EQ(TomlOps::INSTANCE.getNumberValue(*(*map.result())->get("a.b")).result()->intValue(), 1);
  EXPECT_FALSE((*map.result())->get("a").has_value());  // 不是路径查找

  const std::vector<std::pair<Value, Value>> entries = (*map.result())->entries();
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(*TomlOps::INSTANCE.getStringValue(entries[0].first).result(), "a.b");
}

TEST(TomlOpsTest, EqualityIsTypeStrict) {
  const Value one = TomlOps::INSTANCE.createInt(1);
  const Value oneDouble = TomlOps::INSTANCE.createDouble(1.0);

  // tinytoml 的 operator== 是类型严格的：1 != 1.0（JsonOps 走 Gson 规则 1 == 1.0）。
  EXPECT_FALSE(TomlOps::INSTANCE.valueEquals(one, oneDouble));
  EXPECT_TRUE(TomlOps::INSTANCE.valueEquals(one, TomlOps::INSTANCE.createInt(1)));
  EXPECT_TRUE(JsonOps::INSTANCE.valueEquals(Value(JsonValue::number(1)),
                                            Value(JsonValue::number(1.0))));
}

TEST(TomlOpsTest, ConvertsNestedStructuresBothWays) {
  const Value root = parseRoot(
      "[table]\n"
      "  numbers = [1, 2, 3]\n"
      "  nested = { flag = true }\n");

  // TOML → JSON：走 Tinytoml 节点，调 JsonOps 的 createX。
  const Value asJson = TomlOps::INSTANCE.convertTo(JsonOps::INSTANCE, root);
  EXPECT_EQ(asJson.asJson().dump(),
            R"({"table":{"nested":{"flag":true},"numbers":[1,2,3]}})");

  // JSON → TOML：走 JSON 节点，调 TomlOps 的 createX。
  const Value backToToml = JsonOps::INSTANCE.convertTo(TomlOps::INSTANCE, asJson);
  EXPECT_TRUE(TomlOps::INSTANCE.valueEquals(backToToml, root));

  // 同一个 ops 时是恒等（按值比较：两边都是同一份节点）。
  EXPECT_TRUE(TomlOps::INSTANCE.valueEquals(TomlOps::INSTANCE.convertTo(TomlOps::INSTANCE, root),
                                            root));
  // as<T>() 返回的是节点指针，两者指向同一份 tinytoml 节点。
  const Value identity = TomlOps::INSTANCE.convertTo(TomlOps::INSTANCE, root);
  EXPECT_EQ(identity.as<toml::Value>(), root.as<toml::Value>());
}

TEST(TomlOpsTest, DumpTomlRejectsANonTableRoot) {
  const DataResult<std::string> scalar = dumpToml(TomlOps::INSTANCE.createInt(7));
  ASSERT_TRUE(scalar.isError());
  EXPECT_NE(scalar.message().find("must be a table at the root"), std::string::npos);

  const DataResult<std::string> list =
      dumpToml(TomlOps::INSTANCE.createList({TomlOps::INSTANCE.createInt(1)}));
  EXPECT_TRUE(list.isError());
}

TEST(TomlOpsTest, DumpTomlRejectsNulls) {
  // 空值哨兵本身不是合法 TOML 文档。
  EXPECT_TRUE(dumpToml(TomlOps::INSTANCE.empty()).isError());

  // 表里的 null 也要被指出来（JSON 的 null 在 TOML 无对应物）。
  const DataResult<Value> table = TomlOps::INSTANCE.mergeToMap(
      TomlOps::INSTANCE.empty(), TomlOps::INSTANCE.createString("a"), TomlOps::INSTANCE.empty());
  ASSERT_TRUE(table.result().has_value());
  const DataResult<std::string> dumped = dumpToml(*table.result());
  ASSERT_TRUE(dumped.isError());
  EXPECT_NE(dumped.message().find("no null"), std::string::npos);
  EXPECT_NE(dumped.message().find("\"a\""), std::string::npos);
}

TEST(TomlOpsTest, DumpTomlRejectsMixedArrays) {
  const DataResult<Value> table = TomlOps::INSTANCE.mergeToMap(
      TomlOps::INSTANCE.empty(), TomlOps::INSTANCE.createString("a"),
      TomlOps::INSTANCE.createList({TomlOps::INSTANCE.createInt(1),
                                    TomlOps::INSTANCE.createString("two")}));
  ASSERT_TRUE(table.result().has_value());
  const DataResult<std::string> dumped = dumpToml(*table.result());
  ASSERT_TRUE(dumped.isError());
  EXPECT_NE(dumped.message().find("homogeneous"), std::string::npos);
}

TEST(TomlOpsTest, ParseErrorsCarryTheLineNumber) {
  const DataResult<TomlDocument> broken = parseToml("a = 1\nb = ?\n");
  ASSERT_TRUE(broken.isError());
  EXPECT_NE(broken.message().find("line 2"), std::string::npos);

  // v0.4 的能力边界：点号键、混型数组、本地时间都被拒绝。
  EXPECT_TRUE(parseToml("a.b = 1\n").isError());
  EXPECT_TRUE(parseToml("a = [1, \"two\"]\n").isError());
  EXPECT_TRUE(parseToml("t = 07:32:00\n").isError());
}

TEST(TomlOpsTest, EmptyDocumentIsAnEmptyTable) {
  const Value root = parseRoot("");
  const DataResult<MapLikePtr> map = TomlOps::INSTANCE.getMap(root);
  ASSERT_TRUE(map.result().has_value());
  EXPECT_EQ((*map.result())->entries().size(), 0u);

  const DataResult<std::string> dumped = dumpToml(root);
  ASSERT_TRUE(dumped.result().has_value());
  EXPECT_EQ(*dumped.result(), "");  // 空表就是空文档
}

TEST(TomlOpsTest, MergesListsAndMapsWithStrictKeys) {
  // 数组：空哨兵 → 新数组；数组 → 追加；标量 → 报错。
  const DataResult<Value> list =
      TomlOps::INSTANCE.mergeToList(TomlOps::INSTANCE.empty(), TomlOps::INSTANCE.createInt(1));
  ASSERT_TRUE(list.result().has_value());
  const DataResult<Value> appended =
      TomlOps::INSTANCE.mergeToList(*list.result(), TomlOps::INSTANCE.createInt(2));
  ASSERT_TRUE(appended.result().has_value());
  EXPECT_EQ(TomlOps::INSTANCE.toString(*appended.result()), "[1, 2]");
  EXPECT_TRUE(TomlOps::INSTANCE
                  .mergeToList(TomlOps::INSTANCE.createInt(1), TomlOps::INSTANCE.createInt(2))
                  .isError());

  // 表：非字符串键报错（createMap 是全函数，只能跳过；mergeToMap 能报错）。
  const DataResult<Value> badKey = TomlOps::INSTANCE.mergeToMap(
      TomlOps::INSTANCE.empty(), TomlOps::INSTANCE.createInt(1), TomlOps::INSTANCE.createInt(2));
  ASSERT_TRUE(badKey.isError());
  EXPECT_EQ(badKey.message(), "key is not a string: 1");

  const DataResult<Value> table = TomlOps::INSTANCE.mergeToMap(
      TomlOps::INSTANCE.empty(), TomlOps::INSTANCE.createString("a"), TomlOps::INSTANCE.createInt(1));
  ASSERT_TRUE(table.result().has_value());
  EXPECT_EQ(dumpToml(*table.result()).result().value(), "a = 1\n");

  // remove：非表原样返回；表则去掉那个键。
  EXPECT_TRUE(TomlOps::INSTANCE.valueEquals(
      TomlOps::INSTANCE.remove(TomlOps::INSTANCE.createInt(1), "a"), TomlOps::INSTANCE.createInt(1)));
  const Value removed = TomlOps::INSTANCE.remove(*table.result(), "a");
  EXPECT_EQ(dumpToml(removed).result().value(), "");
}

// 编码走的是 mutable 累加器：add 只追加，build 时才成型（否则每加一个元素都要
// 深拷贝整个 toml::Table/Array，编码会退化成 O(N²)）。
TEST(TomlOpsTest, BuildersAccumulateIntoThePrefix) {
  // mapBuilder：同一个键 last-wins（与 JsonOps 一致）。
  const std::shared_ptr<RecordBuilder> map = TomlOps::INSTANCE.mapBuilder();
  map->add("b", TomlOps::INSTANCE.createInt(1));
  map->add("b", TomlOps::INSTANCE.createInt(2));
  const DataResult<Value> builtMap = map->build(TomlOps::INSTANCE.empty());
  ASSERT_TRUE(builtMap.result().has_value()) << builtMap.message();
  EXPECT_EQ(dumpToml(*builtMap.result()).result().value(), "b = 2\n");

  // build 之后累加器复位，可以继续复用（DFU 的 Builder 语义）。
  map->add("c", TomlOps::INSTANCE.createInt(3));
  const DataResult<Value> rebuilt = map->build(TomlOps::INSTANCE.empty());
  ASSERT_TRUE(rebuilt.result().has_value()) << rebuilt.message();
  EXPECT_EQ(dumpToml(*rebuilt.result()).result().value(), "c = 3\n");

  // 非空表前缀并入结果，且前缀自身不被修改。
  const DataResult<Value> prefixMap = TomlOps::INSTANCE.mergeToMap(
      TomlOps::INSTANCE.empty(), TomlOps::INSTANCE.createString("a"), TomlOps::INSTANCE.createInt(1));
  ASSERT_TRUE(prefixMap.result().has_value());
  const std::shared_ptr<RecordBuilder> onto = TomlOps::INSTANCE.mapBuilder();
  onto->add("b", TomlOps::INSTANCE.createInt(2));
  const DataResult<Value> merged = onto->build(*prefixMap.result());
  ASSERT_TRUE(merged.result().has_value()) << merged.message();
  EXPECT_EQ(dumpToml(*merged.result()).result().value(), "a = 1\nb = 2\n");
  EXPECT_EQ(dumpToml(*prefixMap.result()).result().value(), "a = 1\n");

  // listBuilder：追加 + 并入非空数组前缀。
  const std::shared_ptr<ListBuilder> list = TomlOps::INSTANCE.listBuilder();
  list->add(TomlOps::INSTANCE.createInt(1));
  list->add(TomlOps::INSTANCE.createString("two"));
  const DataResult<Value> builtList = list->build(TomlOps::INSTANCE.empty());
  ASSERT_TRUE(builtList.result().has_value()) << builtList.message();
  EXPECT_EQ(TomlOps::INSTANCE.toString(*builtList.result()), "[1, \"two\"]");

  const DataResult<Value> prefixList =
      TomlOps::INSTANCE.mergeToList(TomlOps::INSTANCE.empty(), TomlOps::INSTANCE.createInt(0));
  ASSERT_TRUE(prefixList.result().has_value());
  const std::shared_ptr<ListBuilder> appended = TomlOps::INSTANCE.listBuilder();
  appended->add(TomlOps::INSTANCE.createInt(9));
  const DataResult<Value> mergedList = appended->build(*prefixList.result());
  ASSERT_TRUE(mergedList.result().has_value()) << mergedList.message();
  EXPECT_EQ(TomlOps::INSTANCE.toString(*mergedList.result()), "[0, 9]");
  EXPECT_EQ(TomlOps::INSTANCE.toString(*prefixList.result()), "[0]");
}

TEST(TomlOpsTest, BuildersReportErrorsLikeJsonOps) {
  // prefix 不是表 / 不是数组：错误措辞与 JsonOps 的构造器一致。
  const std::shared_ptr<RecordBuilder> map = TomlOps::INSTANCE.mapBuilder();
  map->add("a", TomlOps::INSTANCE.createInt(1));
  const DataResult<Value> badMap = map->build(TomlOps::INSTANCE.createInt(7));
  ASSERT_TRUE(badMap.isError());
  EXPECT_NE(badMap.message().find("mergeToMap called with not a map"), std::string::npos);

  const std::shared_ptr<ListBuilder> list = TomlOps::INSTANCE.listBuilder();
  list->add(TomlOps::INSTANCE.createInt(1));
  const DataResult<Value> badList = list->build(TomlOps::INSTANCE.createInt(7));
  ASSERT_TRUE(badList.isError());
  EXPECT_NE(badList.message().find("Cannot append a list to not a list"), std::string::npos);

  // 元素的错误原样传播（add(const DataResult<Value>&)）。
  const std::shared_ptr<ListBuilder> failing = TomlOps::INSTANCE.listBuilder();
  failing->add(DataResult<Value>::error("boom"));
  const DataResult<Value> failed = failing->build(TomlOps::INSTANCE.empty());
  ASSERT_TRUE(failed.isError());
  EXPECT_EQ(failed.message(), "boom");

  // withErrorsFrom：来自别处的失败也让构造器失败，并保留已累加的部分值。
  const std::shared_ptr<RecordBuilder> partially =
      TomlOps::INSTANCE.mapBuilder();
  partially->add("a", TomlOps::INSTANCE.createInt(1));
  partially->withErrorsFrom(DataResult<Unit>::error("from-part"));
  const DataResult<Value> partialResult = partially->build(TomlOps::INSTANCE.empty());
  ASSERT_TRUE(partialResult.isError());
  EXPECT_EQ(partialResult.message(), "from-part");

  // mapError：在构造器层面改写错误消息。
  const std::shared_ptr<ListBuilder> rewritten = TomlOps::INSTANCE.listBuilder();
  rewritten->add(DataResult<Value>::error("inner"));
  rewritten->mapError([](const std::string& message) { return "wrapped: " + message; });
  const DataResult<Value> rewrittenResult = rewritten->build(TomlOps::INSTANCE.empty());
  ASSERT_TRUE(rewrittenResult.isError());
  EXPECT_EQ(rewrittenResult.message(), "wrapped: inner");

  // 键不是字符串：交给 ops 判定（getStringValue 失败），构造器整体失败。
  const std::shared_ptr<RecordBuilder> badKey = TomlOps::INSTANCE.mapBuilder();
  badKey->add(TomlOps::INSTANCE.createInt(1), TomlOps::INSTANCE.createInt(2));
  const DataResult<Value> badKeyResult = badKey->build(TomlOps::INSTANCE.empty());
  ASSERT_TRUE(badKeyResult.isError());
  EXPECT_EQ(badKeyResult.message(), "Not a string: 1");

  // 混进来的 JSON 句柄：明确报错，而不是静默 UB。
  const std::shared_ptr<ListBuilder> foreign = TomlOps::INSTANCE.listBuilder();
  foreign->add(Value(JsonValue::number(1)));
  const DataResult<Value> foreignList = foreign->build(TomlOps::INSTANCE.empty());
  ASSERT_TRUE(foreignList.isError());
  EXPECT_NE(foreignList.message().find("non-TOML value"), std::string::npos);

  const std::shared_ptr<RecordBuilder> foreignMap = TomlOps::INSTANCE.mapBuilder();
  foreignMap->add("a", Value(JsonValue::number(1)));
  const DataResult<Value> foreignRecord = foreignMap->build(TomlOps::INSTANCE.empty());
  ASSERT_TRUE(foreignRecord.isError());
  EXPECT_NE(foreignRecord.message().find("non-TOML value"), std::string::npos);
}

// 通用累加器（UniversalRecordBuilder）是"没有自带构造器的 ops"的回退路径。
// 它不能假设键是 JSON 节点：这里用 TomlOps 直接驱动它，验证编码失败仍然带上
// 键名作为位置——旧实现嗅探 key.as<JsonValue::Raw>()，对 TOML 键只会拿到
// nullptr，于是错误位置被静默丢掉，这个用例会失败。
TEST(TomlOpsTest, UniversalRecordBuilderAsksTheOpsForKeyNames) {
  codec::UniversalRecordBuilder builder(TomlOps::INSTANCE);
  builder.add(TomlOps::INSTANCE.createString("severity"),
              DataResult<Value>::error("unmapped severity"));
  const DataResult<Value> built = builder.build(TomlOps::INSTANCE.empty());
  ASSERT_TRUE(built.isError());
  EXPECT_EQ(built.location(), "severity");
  EXPECT_EQ(built.describe(), "severity: unmapped severity");

  // 非字符串键：ops 说不算键名，于是不加位置（与 JSON 侧的严格判定一致）。
  codec::UniversalRecordBuilder numberKey(TomlOps::INSTANCE);
  numberKey.add(TomlOps::INSTANCE.createInt(1), DataResult<Value>::error("boom"));
  const DataResult<Value> builtNumberKey = numberKey.build(TomlOps::INSTANCE.empty());
  ASSERT_TRUE(builtNumberKey.isError());
  EXPECT_EQ(builtNumberKey.location(), "");
  EXPECT_EQ(builtNumberKey.message(), "boom");
}

}  // namespace
