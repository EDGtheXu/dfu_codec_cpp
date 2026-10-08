#pragma once
// ===========================================================================
// codec_toml.hpp —— 用 tinytoml 接第二种序列化格式（TOML）
//
// 核（`codec.hpp`）依旧是单头文件、不依赖 tinytoml；这一层是**可选**的：
// `DynamicOps` 抽出来之后，接一种新格式只需要
//   * 把格式的 DOM 装箱成 `Value`（所有者 + 节点 + 标签）；
//   * 实现 `DynamicOps` 的读写方法；
//   * 提供一个"写出"入口——因为 `createX` 是全函数、报不了错（见 dumpToml）。
// codec 层一行都不用改：同一个 `record<RiskDef>(...)` 既能吃 JSON 也能吃 TOML。
//
// 用法：
//
//   #include "codec_toml.hpp"
//
//   const DataResult<TomlDocument> document = parseToml(text);
//   if (document.isError()) { ... document.message() ... }        // 形如 "Error: line 2: ..."
//   const DataResult<RiskDocument> decoded =
//       riskDocumentCodec().parse(TomlOps::INSTANCE, document.result()->root());
//
//   // 反方向：编码成 TOML 文本（写出前会先校验）
//   const DataResult<Value> encoded = riskDocumentCodec().encodeStart(TomlOps::INSTANCE, value);
//   const DataResult<std::string> text = dumpToml(*encoded.result());
//
// ---------------------------------------------------------------------------
// 类型映射（TOML → codec 值）
//
//   BOOL          ↔ bool
//   INT (int64)   ↔ 整数（Number::ofInt）
//   DOUBLE        ↔ 浮点（Number::ofDouble）
//   STRING        ↔ 字符串
//   ARRAY         ↔ 列表
//   TABLE         ↔ 映射（键是 `std::string`）
//   TIME          → 字符串（**有损**：用 tinytoml 的 writer 渲染；写侧永远不会
//                    造出 TIME，所以只有"解析带日期时间的 TOML"会碰到它）
//   NULL_TYPE     ← 仅作为内存中的哨兵（`TomlOps::empty()`）；TOML 文档里没有
//                    null，因此 `dumpToml` 会拒绝任何 NULL_TYPE。
//
// 与 JsonOps 的差异（**按格式**的差异，不是 bug）：
//   * `getNumberValue` 只认 INT/DOUBLE，**不认 BOOL**：TOML 的类型是严格的，
//     而 JsonOps 会接受布尔并把 `true` 读成 1（DFU/JsonOps 的既有怪癖）；
//   * `valueEquals` 用 tinytoml 的 `operator==`——**类型严格**的深度比较，
//     因此 `1 != 1.0`；JsonOps 走 Gson 规则，`1 == 1.0`；
//   * `MapLike::get` 用 `findChild`（只找直接子节点）。tinytoml 的 `find` 会把
//     键里的点号当路径，而字段名里的 `"a.b"` 是字面量——JSON 侧没有这个问题。
//
// 已知限制：
//   * tinytoml v0.4 的解析器**拒绝**点号键 `a.b = 1`、混型数组 `[1,"two"]`、
//     本地时间 `07:32:00`（日期与日期时间可以）；
//   * 解析错误文本形如 `Error: line 2: unexpected token`（有行号、无列号），
//     而 codec 自身的错误（类型不符等）**给不出 TOML 的行列**：`report()` 里的
//     位置是 codec 路径（`risks[0].severity`），不是文件位置；
//   * `Table` 是 `std::map`，键按字母序，因此 TOML 侧不可能逐字节往返；
//   * 根必须是表（TOML 文档本身就是表），标量/数组根在 `dumpToml` 时被拒。
//
// 编码复杂度：`TomlOps::listBuilder()` / `mapBuilder()` 返回本文件里的
// `TomlListBuilder` / `TomlRecordBuilder`（mutable 累加器，add 只追加、build 才成型），
// 因此 codec 的编码路径是**线性**的。若退回 `DynamicOps` 的通用构造器，它会逐元素调
// `mergeToList` / `mergeToMap`，而这两个方法每次调用都复制整个容器 —— 编码会变成 O(N²)
// （400 个风险项的文档实测 4.2 ms → 248 ms）。这两个方法本身保持 DFU 的逐调用复制语义，
// 供直接调用者使用。
// ===========================================================================

#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "codec.hpp"

#if defined(_MSC_VER)
#pragma warning(push)
// tinytoml 内部用了 gmtime（MSVC 视为不安全）。只在本文件包含它时屏蔽，
// 不用全局 /wd4996。
#pragma warning(disable : 4996)
#endif
#include <toml/toml.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace codec {

// ---------------------------------------------------------------------------
// TomlDocument —— 一份解析好的 TOML 文档
//
// 持有 `shared_ptr<const toml::Value>`（**move** 进去，不深拷贝），`root()`
// 交出可以装箱的根值：句柄与文档共用同一个所有者，装箱零分配。
// ---------------------------------------------------------------------------
class TomlDocument {
 public:
  // 空文档 = 空表（TOML 里"什么都没有"就是一张空表）。
  TomlDocument() : root_(std::make_shared<const toml::Value>(toml::Table{})) {}
  explicit TomlDocument(std::shared_ptr<const toml::Value> root) : root_(std::move(root)) {}

  const toml::Value& raw() const { return *root_; }
  // 根值的擦除句柄（复用同一个所有者）。
  Value root() const { return Value::of<toml::Value>(root_, root_.get()); }

 private:
  std::shared_ptr<const toml::Value> root_;
};

// 解析 TOML 文本。失败时返回 tinytoml 的 errorReason（形如
// "Error: line 2: unexpected token"）。
inline DataResult<TomlDocument> parseToml(std::string_view text) {
  std::istringstream stream{std::string(text)};
  toml::ParseResult parsed = toml::parse(stream);
  if (!parsed.valid()) {
    return DataResult<TomlDocument>::error(
        parsed.errorReason.empty() ? std::string("invalid TOML document") : parsed.errorReason);
  }
  // move 进 shared_ptr：不做深拷贝。
  auto root = std::make_shared<const toml::Value>(std::move(parsed.value));
  return DataResult<TomlDocument>::success(TomlDocument(std::move(root)));
}

namespace detail {

// 把节点渲染成文本（错误消息与 getStringValue 的 TIME 分支用）。
inline std::string renderToml(const toml::Value& node) {
  std::ostringstream out;
  node.write(&out, std::string(), 0);
  return out.str();
}

inline const char* tomlTypeName(toml::Value::Type type) {
  switch (type) {
    case toml::Value::NULL_TYPE: return "null";
    case toml::Value::BOOL_TYPE: return "boolean";
    case toml::Value::INT_TYPE: return "integer";
    case toml::Value::DOUBLE_TYPE: return "float";
    case toml::Value::STRING_TYPE: return "string";
    case toml::Value::TIME_TYPE: return "time";
    case toml::Value::ARRAY_TYPE: return "array";
    case toml::Value::TABLE_TYPE: return "table";
  }
  return "unknown";
}

// 写出前的校验（`createX` 是全函数，报不了错，所以在这里一次说清）：
//   * 根必须是表（TOML 文档就是表）；
//   * 任何地方都不能有 NULL_TYPE（TOML 没有 null）；
//   * 数组元素必须同型（TOML v0.4 读不回混型数组）。
// 返回空串表示通过。
inline std::string validateTomlForWriting(const toml::Value& node, bool isRoot) {
  if (node.type() == toml::Value::NULL_TYPE) {
    return std::string("TOML has no null value: drop JSON nulls") +
           " (use optional fields) before writing TOML";
  }
  if (isRoot && !node.is<toml::Table>()) {
    return std::string("a TOML document must be a table at the root, got ") +
           tomlTypeName(node.type());
  }
  if (node.is<toml::Table>()) {
    for (const auto& entry : node.as<toml::Table>()) {
      const std::string nested = validateTomlForWriting(entry.second, false);
      if (!nested.empty()) {
        return "in table key \"" + entry.first + "\": " + nested;
      }
    }
    return {};
  }
  if (node.is<toml::Array>()) {
    const toml::Array& array = node.as<toml::Array>();
    if (array.empty()) {
      return {};
    }
    const toml::Value::Type first = array.front().type();
    for (const toml::Value& element : array) {
      if (element.type() != first) {
        return std::string("TOML arrays must be homogeneous: found ") +
               tomlTypeName(element.type()) + " after " + tomlTypeName(first);
      }
      const std::string nested = validateTomlForWriting(element, false);
      if (!nested.empty()) {
        return nested;
      }
    }
  }
  return {};
}

// 把刚建好的 TOML 节点装箱：一次分配、零引用计数（所有者直接 move 进去）。
// 构造器（build/buildState）与 `TomlOps::createX` 都走这里。
inline Value boxToml(toml::Value&& node) {
  auto owner = std::make_shared<const toml::Value>(std::move(node));
  return Value::of<toml::Value>(std::move(owner), owner.get());
}

}  // namespace detail

// ---------------------------------------------------------------------------
// TomlTableMapLike —— TOML 表之上的 MapLike（TomlOps::getMap）
// ---------------------------------------------------------------------------
class TomlTableMapLike : public MapLike {
 public:
  TomlTableMapLike(std::shared_ptr<const void> owner, const toml::Value* node)
      : owner_(std::move(owner)), node_(node) {}

  std::optional<Value> get(const Value& keyHandle) const override {
    const toml::Value* key = keyHandle.as<toml::Value>();
    if (key == nullptr || !key->is<std::string>()) {
      return std::nullopt;
    }
    return get(key->as<std::string>());
  }

  std::optional<Value> get(const std::string& key) const override {
    if (node_ == nullptr || !node_->is<toml::Table>()) {
      return std::nullopt;
    }
    // 必须用 findChild：tinytoml 的 find 会把键里的点号当路径，
    // 而字段名里的 `"a.b"` 是字面量。
    const toml::Value* child = node_->findChild(key);
    if (child == nullptr || child->type() == toml::Value::NULL_TYPE) {
      return std::nullopt;
    }
    return Value::ofChild(owner_, child, tagOf<toml::Value>());
  }

  std::vector<std::pair<Value, Value>> entries() const override {
    std::vector<std::pair<Value, Value>> out;
    if (node_ == nullptr || !node_->is<toml::Table>()) {
      return out;
    }
    const toml::Table& table = node_->as<toml::Table>();
    out.reserve(table.size());
    for (const auto& entry : table) {
      // 键是 std::string（不是节点），所以装箱键要新建一个节点；值只借子节点。
      auto keyOwner = std::make_shared<const toml::Value>(toml::Value(entry.first));
      out.emplace_back(Value::of<toml::Value>(keyOwner, keyOwner.get()),
                       Value::ofChild(owner_, &entry.second, tagOf<toml::Value>()));
    }
    return out;
  }

  std::string toString() const override {
    return "MapLike[" + (node_ != nullptr ? detail::renderToml(*node_) : std::string()) + "]";
  }

 private:
  std::shared_ptr<const void> owner_;
  const toml::Value* node_;
};

// ---------------------------------------------------------------------------
// TomlListBuilder / TomlRecordBuilder —— TOML 侧的 mutable 累加器
//
// 为什么必须有它们：`DynamicOps` 的通用构造器（UniversalListBuilder /
// UniversalRecordBuilder）是"逐元素调 mergeToList / mergeToMap"，而 TOML 的这两个
// 方法每次都 `as<Array>()` / `as<Table>()` 复制**整个已累积的容器**再追加一个元素
// —— 于是编码成 O(N²)（400 个风险项约 250 ms，而 dumpToml 是线性的）。
// JsonOps 没这个问题，因为它的 ArrayListBuilder / StringRecordBuilder 把累加器挂在
// `shared_ptr` 后面：DFU 的 Builder 本来就是**可变对象**，由 applicative 链按引用携带，
// 所以 add 只追加、不复制容器，build 时才一次性成型。
//
// 这里与 JsonOps 的两个构造器逐条对应：状态类型相同（装箱句柄，写盘时才落进 DOM）、
// add / withErrorsFrom / mapError / lifecycle / prefix 的"空 or 非空"判定与错误消息
// 都一致；只有"成型"那一步是 TOML 专有的（build / buildState）。
// 语义差异只有一处、且是 DFU 本来的分工：`mergeToList` / `mergeToMap` 仍是逐元素
// 深拷贝的 ops 语义（供直接调用），构造器走的是它们自己的线性路径。
// ---------------------------------------------------------------------------
class TomlListBuilder : public ListBuilder {
 public:
  // 与 ArrayListBuilder 同理：累加器存装箱句柄（每个元素一次引用计数拷贝），
  // 而不是 toml::Value——add 期间一次深拷贝都不做。
  using State = std::shared_ptr<std::vector<Value>>;

  explicit TomlListBuilder(const DynamicOps& ops)
      : ops_(&ops), builder_(DataResult<State>::success(initial(), Lifecycle::stable())) {}

  const DynamicOps& ops() const override { return *ops_; }

  ListBuilder& add(const Value& value) override {
    builder_ = builder_.map([value](const State& state) {
      state->push_back(value);
      return state;
    });
    return *this;
  }

  ListBuilder& add(const DataResult<Value>& value) override {
    builder_ = builder_.apply2stable(
        [](const State& state, const Value& element) {
          state->push_back(element);
          return state;
        },
        value);
    return *this;
  }

  ListBuilder& withErrorsFrom(const DataResultBase& result) override {
    builder_ = detail::propagateErrors(builder_, result);
    return *this;
  }

  ListBuilder& mapError(const StringUnaryOperator& onError) override {
    builder_ = builder_.mapError(onError);
    return *this;
  }

  DataResult<Value> build(const Value& prefix) override;

 private:
  static State initial() { return std::make_shared<std::vector<Value>>(); }

  const DynamicOps* ops_;
  DataResult<State> builder_;
};

class TomlRecordBuilder
    : public AbstractRecordBuilder<std::shared_ptr<std::vector<std::pair<std::string, Value>>>> {
 public:
  // 与 StringRecordBuilder 同形：键已经由 ops 的 getStringValue 转成 std::string，
  // 值是装箱句柄。
  using Members = std::vector<std::pair<std::string, Value>>;
  using State = std::shared_ptr<Members>;

  explicit TomlRecordBuilder(const DynamicOps& ops)
      : AbstractRecordBuilder<State>(ops, initial()) {}

  RecordBuilder& add(const std::string& key, const Value& value) override {
    builder_ = builder_.map([key, value](const State& state) {
      state->emplace_back(key, value);
      return state;
    });
    return *this;
  }

  RecordBuilder& add(const std::string& key, const DataResult<Value>& value) override {
    builder_ = builder_.apply2stable(
        [key](const State& state, const Value& element) {
          state->emplace_back(key, element);
          return state;
        },
        // 编码失败会携带其来源成员，与 JsonOps 一致。
        value.addPath(key));
    return *this;
  }

  // 键是句柄时交给 ops 取字符串（RecordBuilder.AbstractStringBuilder 的写法），
  // 因此这里没有任何 JSON 假设。
  RecordBuilder& add(const Value& key, const Value& value) override {
    builder_ = ops().getStringValue(key).flatMap([this, value](const std::string& k) {
      add(k, value);
      return builder_;
    });
    return *this;
  }

  RecordBuilder& add(const Value& key, const DataResult<Value>& value) override {
    builder_ = ops().getStringValue(key).flatMap([this, value](const std::string& k) {
      add(k, value);
      return builder_;
    });
    return *this;
  }

  RecordBuilder& add(const DataResult<Value>& key, const DataResult<Value>& value) override {
    builder_ = key.flatMap([this](const Value& k) { return ops().getStringValue(k); })
                   .flatMap([this, value](const std::string& k) {
                     add(k, value);
                     return builder_;
                   });
    return *this;
  }

  static State initial() { return std::make_shared<Members>(); }

 protected:
  State initBuilder() const override { return initial(); }
  DataResult<Value> buildState(const State& state, const Value& prefix) override;
};

inline DataResult<Value> TomlListBuilder::build(const Value& prefix) {
  const toml::Value* prefixNode = prefix.as<toml::Value>();
  // 与 JsonOps 一致：prefix 与 empty() 按 ops 的深度相等比较（不是节点身份），
  // 否则"另一个空表/空值节点"会被当成"不是列表"而报错。
  const bool prefixIsEmpty = ops_->valueEquals(prefix, ops_->empty());
  DataResult<Value> result = builder_.flatMap([&](const State& elements) -> DataResult<Value> {
    if (prefixNode == nullptr) {
      const std::string message =
          "Cannot append a list to not a list: " + ops_->toString(prefix);
      return DataResult<Value>::error(message, prefix);
    }
    if (!prefixNode->is<toml::Array>() && !prefixIsEmpty) {
      return DataResult<Value>::error(
          "Cannot append a list to not a list: " + detail::renderToml(*prefixNode), prefix);
    }
    // 唯一的成型点：prefix（若非空）一次深拷贝 + 每个元素一次深拷贝，
    // 之后把数组 move 进 toml::Value（不再多一次容器拷贝）。
    toml::Array out =
        prefixNode->is<toml::Array>() ? prefixNode->as<toml::Array>() : toml::Array{};
    out.reserve(out.size() + elements->size());
    for (const Value& element : *elements) {
      const toml::Value* node = element.as<toml::Value>();
      if (node == nullptr) {
        return DataResult<Value>::error("Cannot append a non-TOML value to a list", prefix);
      }
      out.push_back(*node);
    }
    return DataResult<Value>::success(detail::boxToml(toml::Value(std::move(out))),
                                      Lifecycle::stable());
  });
  builder_ = DataResult<State>::success(initial(), Lifecycle::stable());
  return result;
}

inline DataResult<Value> TomlRecordBuilder::buildState(const State& state, const Value& prefix) {
  const toml::Value* prefixNode = prefix.as<toml::Value>();
  if (prefixNode == nullptr) {
    return DataResult<Value>::error("mergeToMap called with not a map: " + ops_->toString(prefix),
                                    prefix);
  }
  const bool prefixIsEmpty = ops_->valueEquals(prefix, ops_->empty());
  if (!prefixIsEmpty && !prefixNode->is<toml::Table>()) {
    // 非表（也不是空）的 prefix 要报错，与 JsonOps 一致。
    return DataResult<Value>::error(
        "mergeToMap called with not a map: " + detail::renderToml(*prefixNode), prefix);
  }
  const bool hasPrefix = prefixNode->is<toml::Table>() && !prefixIsEmpty;
  toml::Table members = hasPrefix ? prefixNode->as<toml::Table>() : toml::Table{};
  for (const auto& entry : *state) {
    const toml::Value* value = entry.second.as<toml::Value>();
    if (value == nullptr) {
      return DataResult<Value>::error("Cannot write a non-TOML value into a map", prefix);
    }
    // std::map：已存在的键原地覆盖（与 JsonOps 的 putRawMember 同样的 last-wins）。
    members[entry.first] = *value;
  }
  return DataResult<Value>::success(detail::boxToml(toml::Value(std::move(members))));
}

// ---------------------------------------------------------------------------
// TomlOps —— 用 tinytoml 实现 DynamicOps
// ---------------------------------------------------------------------------
class TomlOps : public DynamicOps {
 public:
  static const TomlOps INSTANCE;

  TomlOps() : empty_(box(toml::Value())) {}  // NULL_TYPE 哨兵

  // --- 基础类型 ---------------------------------------------------------
  Value empty() const override { return empty_; }

  Value convertTo(const DynamicOps& outOps, const Value& input) const override {
    if (&outOps == this) {
      return input;
    }
    return convertNode(outOps, *requireNode(input));
  }

  std::string toString() const { return "TOML"; }

  std::string toString(const Value& input) const override {
    return detail::renderToml(*requireNode(input));
  }

  // tinytoml 的 operator== 是**类型严格**的深度比较（1 != 1.0），
  // 与 JsonOps 的 Gson 式相等不同——这是按格式的差异。
  bool valueEquals(const Value& left, const Value& right) const override {
    const toml::Value* a = left.as<toml::Value>();
    const toml::Value* b = right.as<toml::Value>();
    if (a == nullptr || b == nullptr) {
      return a == nullptr && b == nullptr && left.node == right.node && left.tag == right.tag;
    }
    return *a == *b;
  }

  // TOML 的类型是严格的：整数/浮点才是数字，布尔不接受（JsonOps 会把
  // true 读成 1，那是 JSON 侧的既有怪癖）。
  DataResult<Number> getNumberValue(const Value& input) const override {
    const toml::Value* node = requireNode(input);
    if (node->is<int64_t>()) {
      return DataResult<Number>::success(Number::ofInt(node->as<int64_t>()));
    }
    if (node->is<double>()) {
      return DataResult<Number>::success(Number::ofDouble(node->as<double>()));
    }
    return DataResult<Number>::error("Not a number: " + detail::renderToml(*node));
  }

  Value createNumeric(const Number& value) const override {
    return box(value.isIntegral() ? toml::Value(value.longValue())
                                  : toml::Value(value.doubleValue()));
  }

  DataResult<bool> getBooleanValue(const Value& input) const override {
    const toml::Value* node = requireNode(input);
    if (!node->is<bool>()) {
      return DataResult<bool>::error("Not a boolean: " + detail::renderToml(*node));
    }
    return DataResult<bool>::success(node->as<bool>());
  }

  Value createBoolean(bool value) const override { return box(toml::Value(value)); }

  DataResult<std::string> getStringValue(const Value& input) const override {
    const toml::Value* node = requireNode(input);
    if (node->is<std::string>()) {
      return DataResult<std::string>::success(node->as<std::string>());
    }
    if (node->type() == toml::Value::TIME_TYPE) {
      // 有损：日期/时间渲染成字符串（tinytoml 没有对应的标量类型可映射）。
      return DataResult<std::string>::success(detail::renderToml(*node));
    }
    return DataResult<std::string>::error("Not a string: " + detail::renderToml(*node));
  }

  Value createString(const std::string& value) const override { return box(toml::Value(value)); }

  // --- list/map 构造 ---------------------------------------------
  DataResult<Value> mergeToList(const Value& list, const Value& value) const override {
    const toml::Value* listNode = requireNode(list);
    const toml::Value* valueNode = requireNode(value);
    if (!listNode->is<toml::Array>() && !valueEquals(list, empty_)) {
      return DataResult<Value>::error(
          "mergeToList called with not a list: " + detail::renderToml(*listNode), list);
    }
    toml::Array out = listNode->is<toml::Array>() ? listNode->as<toml::Array>() : toml::Array{};
    out.push_back(*valueNode);
    return DataResult<Value>::success(box(toml::Value(std::move(out))));
  }

  DataResult<Value> mergeToList(const Value& list,
                                const std::vector<Value>& values) const override {
    const toml::Value* listNode = requireNode(list);
    if (!listNode->is<toml::Array>() && !valueEquals(list, empty_)) {
      return DataResult<Value>::error(
          "mergeToList called with not a list: " + detail::renderToml(*listNode), list);
    }
    toml::Array out = listNode->is<toml::Array>() ? listNode->as<toml::Array>() : toml::Array{};
    for (const Value& value : values) {
      out.push_back(*requireNode(value));
    }
    return DataResult<Value>::success(box(toml::Value(std::move(out))));
  }

  DataResult<Value> mergeToMap(const Value& map, const Value& key,
                               const Value& value) const override {
    const toml::Value* mapNode = requireNode(map);
    const toml::Value* keyNode = requireNode(key);
    const toml::Value* valueNode = requireNode(value);
    if (!mapNode->is<toml::Table>() && !valueEquals(map, empty_)) {
      return DataResult<Value>::error(
          "mergeToMap called with not a map: " + detail::renderToml(*mapNode), map);
    }
    if (!keyNode->is<std::string>()) {
      return DataResult<Value>::error("key is not a string: " + detail::renderToml(*keyNode), map);
    }
    toml::Table out = mapNode->is<toml::Table>() ? mapNode->as<toml::Table>() : toml::Table{};
    out[keyNode->as<std::string>()] = *valueNode;  // std::map：已存在的键原地覆盖
    return DataResult<Value>::success(box(toml::Value(std::move(out))));
  }

  // --- map 访问 ---------------------------------------------------------
  DataResult<MapLikePtr> getMap(const Value& input) const override {
    const toml::Value* node = requireNode(input);
    if (!node->is<toml::Table>()) {
      return DataResult<MapLikePtr>::error("Not a TOML table: " + detail::renderToml(*node));
    }
    return DataResult<MapLikePtr>::success(std::make_shared<TomlTableMapLike>(input.owner, node));
  }

  DataResult<std::vector<std::pair<Value, Value>>> getMapValues(
      const Value& input) const override {
    const toml::Value* node = requireNode(input);
    if (!node->is<toml::Table>()) {
      return DataResult<std::vector<std::pair<Value, Value>>>::error(
          "Not a TOML table: " + detail::renderToml(*node));
    }
    std::vector<std::pair<Value, Value>> out;
    const toml::Table& table = node->as<toml::Table>();
    out.reserve(table.size());
    for (const auto& entry : table) {
      auto keyOwner = std::make_shared<const toml::Value>(toml::Value(entry.first));
      out.emplace_back(Value::of<toml::Value>(keyOwner, keyOwner.get()),
                       Value::ofChild(input.owner, &entry.second, tagOf<toml::Value>()));
    }
    return DataResult<std::vector<std::pair<Value, Value>>>::success(std::move(out));
  }

  Value createMap(const std::vector<std::pair<Value, Value>>& entries) const override {
    // createX 是全函数、报不了错：键不是字符串时这里只能跳过（JsonOps 同样
    // 跳过）。能报错的那条路是 `mergeToMap(map, key, value)`，它会对非字符串键
    // 返回错误；dumpToml 的校验则兜住"写出非法文档"。
    toml::Table out;
    for (const auto& entry : entries) {
      const toml::Value* key = requireNode(entry.first);
      if (!key->is<std::string>()) {
        continue;
      }
      out[key->as<std::string>()] = *requireNode(entry.second);
    }
    return box(toml::Value(std::move(out)));
  }

  // --- list 访问 --------------------------------------------------------
  DataResult<std::vector<Value>> getStream(const Value& input) const override {
    const toml::Value* node = requireNode(input);
    if (!node->is<toml::Array>()) {
      return DataResult<std::vector<Value>>::error("Not a TOML array: " +
                                                   detail::renderToml(*node));
    }
    const toml::Array& array = node->as<toml::Array>();
    std::vector<Value> out;
    out.reserve(array.size());
    for (const toml::Value& element : array) {
      out.push_back(Value::ofChild(input.owner, &element, tagOf<toml::Value>()));
    }
    return DataResult<std::vector<Value>>::success(std::move(out));
  }

  DataResult<std::vector<Value>> getList(const Value& input) const override {
    return getStream(input);
  }

  Value createList(const std::vector<Value>& values) const override {
    toml::Array out;
    out.reserve(values.size());
    for (const Value& value : values) {
      out.push_back(*requireNode(value));
    }
    return box(toml::Value(std::move(out)));
  }

  Value remove(const Value& input, const std::string& key) const override {
    const toml::Value* node = requireNode(input);
    if (!node->is<toml::Table>()) {
      return input;
    }
    toml::Table out = node->as<toml::Table>();
    out.erase(key);
    return box(toml::Value(std::move(out)));
  }

  // --- 行为 -------------------------------------------------------------
  // 编码走 mutable 累加器（见上面的 TomlListBuilder / TomlRecordBuilder）：
  // 通用构造器逐元素调 mergeToList / mergeToMap 会复制整表，编码因此是 O(N²)。
  // TOML 没有压缩模式（compressMaps() 默认 false），所以只有未压缩的一套构造器。
  std::shared_ptr<ListBuilder> listBuilder() const override {
    return std::make_shared<TomlListBuilder>(*this);
  }

  std::shared_ptr<RecordBuilder> mapBuilder() const override {
    return std::make_shared<TomlRecordBuilder>(*this);
  }

 private:
  static const toml::Value* requireNode(const Value& value) {
    const toml::Value* node = value.as<toml::Value>();
    if (node == nullptr) {
      throw std::logic_error("TomlOps: the handle does not hold a TOML node");
    }
    return node;
  }

  // 一次分配、零引用计数（所有者直接 move 进节点）——与构造器共用同一个装箱入口。
  static Value box(toml::Value&& node) { return detail::boxToml(std::move(node)); }

  // 以 TOML 节点为源、逐节点导出到目标 ops（convertTo 的递归实现）。
  static Value convertNode(const DynamicOps& outOps, const toml::Value& node) {
    switch (node.type()) {
      case toml::Value::BOOL_TYPE:
        return outOps.createBoolean(node.as<bool>());
      case toml::Value::INT_TYPE:
        return outOps.createLong(node.as<int64_t>());
      case toml::Value::DOUBLE_TYPE:
        return outOps.createDouble(node.as<double>());
      case toml::Value::STRING_TYPE:
        return outOps.createString(node.as<std::string>());
      case toml::Value::TIME_TYPE:
        return outOps.createString(detail::renderToml(node));
      case toml::Value::ARRAY_TYPE: {
        std::vector<Value> elements;
        for (const toml::Value& element : node.as<toml::Array>()) {
          elements.push_back(convertNode(outOps, element));
        }
        return outOps.createList(std::move(elements));
      }
      case toml::Value::TABLE_TYPE: {
        std::vector<std::pair<Value, Value>> entries;
        for (const auto& entry : node.as<toml::Table>()) {
          entries.emplace_back(outOps.createString(entry.first), convertNode(outOps, entry.second));
        }
        return outOps.createMap(std::move(entries));
      }
      case toml::Value::NULL_TYPE:
        return outOps.empty();
    }
    return outOps.empty();
  }

  const Value empty_;
};

inline const TomlOps TomlOps::INSTANCE{};

// ---------------------------------------------------------------------------
// dumpToml —— 把（TomlOps 产出的）值渲染成 TOML 文本
//
// `createX` 是全函数、报不了错，所以**写出前统一校验**：
//   * 根必须是表（TOML 文档就是表）；
//   * 不得出现 NULL_TYPE（JSON 的 null 在 TOML 无对应物）；
//   * 数组元素必须同型（TOML v0.4 读不回混型数组）。
// 校验通过后用 tinytoml 的 writer 渲染。
// ---------------------------------------------------------------------------
inline DataResult<std::string> dumpToml(const Value& value) {
  const toml::Value* root = value.as<toml::Value>();
  if (root == nullptr) {
    return DataResult<std::string>::error("dumpToml: the value does not hold a TOML node");
  }
  const std::string problem = detail::validateTomlForWriting(*root, true);
  if (!problem.empty()) {
    return DataResult<std::string>::error(problem);
  }
  std::ostringstream out;
  root->write(&out, std::string(), 0);
  return DataResult<std::string>::success(out.str());
}

}  // namespace codec
