#pragma once
// 方案 B（泛化 ops）阶段 0 原型：把"值类型"从 ops/codec 层擦除，量一下代价。
//
// 这不是库的一部分，只是一次设计实验：
//   cmake -S . -B build-poc -DCODEC_BUILD_PROTOTYPE=ON && cmake --build build-poc
//   build-poc/prototype/codec_generic_ops_poc.exe
//
// 要回答三个问题：
//   1. 装箱（JsonValue 视图 → 擦除句柄）能不能做到零分配？
//      —— 能，只要句柄复用 JsonValue 已有的 shared_ptr 所有者（本文件照抄该模型）。
//   2. 每次取值多一次"标签检查 + static_cast"要多少钱？
//   3. 句柄是 24 字节（标签由 ops 自己知道）还是 32 字节（每个值自带标签），
//      在缓存上差多少？
//
// A 路径 = 今天的形态（ops 直接收发 codec::JsonValue）；
// B 路径 = 擦除后的形态（ops 收发 Erased 句柄，内部是 const Raw* + 所有者）。
// 两条路径在同一个 nlohmann 文档、同一份语义上跑，数字才有可比性。

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "codec.hpp"  // 真实的 codec::JsonValue：证明擦除句柄能和它零拷贝互操作

namespace proto {

using Raw = nlohmann::ordered_json;

// ---------------------------------------------------------------------------
// 值标签：每个具体值类型一个稳定地址，比较是 O(1) 指针比较
// （不用 typeid —— MSVC 的 type_info::operator== 可能退化成名字串比较）
// ---------------------------------------------------------------------------
template <class T>
const void* tagOf() {
  static const int tag = 0;
  return &tag;
}

// ---------------------------------------------------------------------------
// 两种尺寸的擦除句柄
// ---------------------------------------------------------------------------

// 24 字节：与 codec::JsonValue 完全同构（16 字节所有者 + 8 字节节点指针）。
// 代价：类型标签由 ops 自己持有，跨 ops 传值时要靠调用方保证类型正确。
struct Value24 {
  std::shared_ptr<const void> owner;
  const void* node = nullptr;

  template <class T>
  static Value24 of(std::shared_ptr<const void> owner, const T* node) {
    return Value24{std::move(owner), static_cast<const void*>(node)};
  }
  template <class T>
  const T* as() const {
    return static_cast<const T*>(node);
  }
  bool valid() const { return node != nullptr; }
};
static_assert(sizeof(Value24) == sizeof(codec::JsonValue), "24 字节句柄应与 JsonValue 同构");

// 32 字节：每个值自带标签，可以做运行时类型检查（跨格式 convertTo、诊断要用）。
struct Value32 {
  std::shared_ptr<const void> owner;
  const void* node = nullptr;
  const void* tag = nullptr;

  template <class T>
  static Value32 of(std::shared_ptr<const void> owner, const T* node) {
    return Value32{std::move(owner), static_cast<const void*>(node), tagOf<T>()};
  }
  template <class T>
  bool is() const {
    return tag == tagOf<T>();
  }
  template <class T>
  const T* as() const {
    return is<T>() ? static_cast<const T*>(node) : nullptr;
  }
  bool valid() const { return node != nullptr; }
};

// 选中的句柄（用 -DPROTO_TAGGED_HANDLE=0/1 切换，两条路径的代码完全相同）：
//   0 = Value24：标签由 ops 自己持有，句柄与今天的 JsonValue 同构（24 字节，
//       每个 map 条目 48 字节，与今天完全一致）
//   1 = Value32：每个值自带标签，可做运行时类型检查，代价是每值多 8 字节
#ifndef PROTO_TAGGED_HANDLE
#define PROTO_TAGGED_HANDLE 0
#endif

#if PROTO_TAGGED_HANDLE
using Erased = Value32;
#else
using Erased = Value24;
#endif

// ---------------------------------------------------------------------------
// A 路径：今天的形态 —— ops 直接收发 codec::JsonValue
// ---------------------------------------------------------------------------
class DirectOps {
 public:
  virtual ~DirectOps() = default;

  virtual codec::JsonValue createInt(int64_t value) const = 0;
  virtual codec::JsonValue createString(std::string value) const = 0;
  virtual codec::JsonValue createList(std::vector<codec::JsonValue> values) const = 0;
  virtual codec::JsonValue createMap(
      std::vector<std::pair<codec::JsonValue, codec::JsonValue>> entries) const = 0;

  virtual std::optional<int64_t> getInt(const codec::JsonValue& value) const = 0;
  virtual std::optional<std::string> getString(const codec::JsonValue& value) const = 0;
  virtual std::optional<std::vector<codec::JsonValue>> getList(const codec::JsonValue& value) const = 0;
  virtual std::optional<std::vector<std::pair<codec::JsonValue, codec::JsonValue>>> getMap(
      const codec::JsonValue& value) const = 0;
};

// ---------------------------------------------------------------------------
// B 路径：擦除后的形态 —— ops 收发 Erased，并新增真正的 convertTo
// ---------------------------------------------------------------------------
class ErasedOps {
 public:
  virtual ~ErasedOps() = default;

  virtual Erased createInt(int64_t value) const = 0;
  virtual Erased createString(std::string value) const = 0;
  virtual Erased createList(std::vector<Erased> values) const = 0;
  virtual Erased createMap(std::vector<std::pair<Erased, Erased>> entries) const = 0;

  virtual std::optional<int64_t> getInt(const Erased& value) const = 0;
  virtual std::optional<std::string> getString(const Erased& value) const = 0;
  virtual std::optional<std::vector<Erased>> getList(const Erased& value) const = 0;
  virtual std::optional<std::vector<std::pair<Erased, Erased>>> getMap(const Erased& value) const = 0;

  // DFU 的 convertTo：今天在 JsonOps 里是恒等，泛化之后才是真转换。
  virtual Erased convertTo(const ErasedOps& outOps, const Erased& input) const = 0;
};

// ---------------------------------------------------------------------------
// JSON 实现（同一个 nlohmann 文档）
// ---------------------------------------------------------------------------

// A：用 codec::JsonValue 干活（照抄今天 JsonOps 的做法：JsonValue 内部就是
// "所有者 + 指向文档里的节点"）。
class JsonDirectOps : public DirectOps {
 public:
  codec::JsonValue createInt(int64_t value) const override { return codec::JsonValue::number(value); }
  codec::JsonValue createString(std::string value) const override {
    return codec::JsonValue::string(std::move(value));
  }
  codec::JsonValue createList(std::vector<codec::JsonValue> values) const override {
    return codec::JsonValue::array(std::move(values));
  }
  codec::JsonValue createMap(
      std::vector<std::pair<codec::JsonValue, codec::JsonValue>> entries) const override {
    codec::JsonValue::Object object;
    object.reserve(entries.size());
    for (auto& entry : entries) {
      object.emplace_back(entry.first.asString(), std::move(entry.second));
    }
    return codec::JsonValue::object(std::move(object));
  }

  std::optional<int64_t> getInt(const codec::JsonValue& value) const override {
    if (!value.isNumber()) {
      return std::nullopt;
    }
    return value.asNumber().longValue();
  }
  std::optional<std::string> getString(const codec::JsonValue& value) const override {
    if (!value.isString()) {
      return std::nullopt;
    }
    return value.asString();
  }
  std::optional<std::vector<codec::JsonValue>> getList(const codec::JsonValue& value) const override {
    if (!value.isArray()) {
      return std::nullopt;
    }
    return value.asArray();
  }
  std::optional<std::vector<std::pair<codec::JsonValue, codec::JsonValue>>> getMap(
      const codec::JsonValue& value) const override {
    if (!value.isObject()) {
      return std::nullopt;
    }
    // asObject() 给出的是 (string, JsonValue)，键要包成 JsonValue ——
    // 擦除路径同样要为每个键建一个节点，两条路径的分配数因此可比。
    std::vector<std::pair<codec::JsonValue, codec::JsonValue>> out;
    out.reserve(value.size());
    for (auto& entry : value.asObject()) {
      out.emplace_back(codec::JsonValue::string(std::move(entry.first)), std::move(entry.second));
    }
    return out;
  }
};

// B：直接作用于 const Raw* 节点，只在边界上构造 JsonValue 视图 ——
// 这也正是把 JsonValue 交给调用方的方式（一次 shared_ptr 拷贝，零分配）。
class JsonErasedOps : public ErasedOps {
 public:
  Erased box(const std::shared_ptr<const void>& owner, const Raw* node) const {
    return Erased::of<Raw>(owner, node);
  }

  // 擦除句柄 → JsonValue 视图（互操作的证明）
  static codec::JsonValue view(const Erased& value) {
    const Raw* node = value.as<Raw>();
    if (node == nullptr) {
      throw std::logic_error("JsonErasedOps: value does not hold a JSON node");
    }
    return codec::JsonValue(std::static_pointer_cast<const Raw>(value.owner), node);
  }

  Erased createInt(int64_t value) const override {
    auto owner = std::make_shared<const Raw>(Raw(value));
    return box(owner, owner.get());
  }
  Erased createString(std::string value) const override {
    auto owner = std::make_shared<const Raw>(Raw(std::move(value)));
    return box(owner, owner.get());
  }
  Erased createList(std::vector<Erased> values) const override {
    auto owner = std::make_shared<Raw>(Raw::array());
    for (const Erased& value : values) {
      owner->push_back(*value.as<Raw>());
    }
    std::shared_ptr<const Raw> shared = owner;
    return box(shared, shared.get());
  }
  Erased createMap(std::vector<std::pair<Erased, Erased>> entries) const override {
    auto owner = std::make_shared<Raw>(Raw::object());
    for (const auto& entry : entries) {
      (*owner)[*entry.first.as<Raw>()] = *entry.second.as<Raw>();
    }
    std::shared_ptr<const Raw> shared = owner;
    return box(shared, shared.get());
  }

  std::optional<int64_t> getInt(const Erased& value) const override {
    const Raw* node = require(value);
    if (!node->is_number()) {
      return std::nullopt;
    }
    return node->get<int64_t>();
  }
  std::optional<std::string> getString(const Erased& value) const override {
    const Raw* node = require(value);
    if (!node->is_string()) {
      return std::nullopt;
    }
    return node->get<std::string>();
  }
  std::optional<std::vector<Erased>> getList(const Erased& value) const override {
    const Raw* node = require(value);
    if (!node->is_array()) {
      return std::nullopt;
    }
    std::vector<Erased> out;
    out.reserve(node->size());
    for (const Raw& element : *node) {
      out.push_back(box(value.owner, &element));
    }
    return out;
  }
  std::optional<std::vector<std::pair<Erased, Erased>>> getMap(const Erased& value) const override {
    const Raw* node = require(value);
    if (!node->is_object()) {
      return std::nullopt;
    }
    std::vector<std::pair<Erased, Erased>> out;
    out.reserve(node->size());
    for (auto it = node->begin(); it != node->end(); ++it) {
      // key 在 ordered_json 里是 std::string，不是节点，所以要单独装箱；
      // A 路径同样要付这笔钱（asObject() 会把键做成 JsonValue）。
      auto keyOwner = std::make_shared<const Raw>(Raw(it.key()));
      out.emplace_back(box(keyOwner, keyOwner.get()), box(value.owner, &it.value()));
    }
    return out;
  }

  Erased convertTo(const ErasedOps& outOps, const Erased& input) const override {
    const Raw* node = require(input);
    if (node->is_null()) {
      return outOps.createString("");  // 玩具 DOM 里没有 null，用空串占位（原型简化）
    }
    if (node->is_boolean()) {
      return outOps.createInt(node->get<bool>() ? 1 : 0);
    }
    if (node->is_number_integer()) {
      return outOps.createInt(node->get<int64_t>());
    }
    if (node->is_number_float()) {
      return outOps.createString(std::to_string(node->get<double>()));
    }
    if (node->is_string()) {
      return outOps.createString(node->get<std::string>());
    }
    if (node->is_array()) {
      std::vector<Erased> elements;
      elements.reserve(node->size());
      for (const Raw& element : *node) {
        elements.push_back(convertTo(outOps, box(input.owner, &element)));
      }
      return outOps.createList(std::move(elements));
    }
    std::vector<std::pair<Erased, Erased>> entries;
    entries.reserve(node->size());
    for (auto it = node->begin(); it != node->end(); ++it) {
      auto keyOwner = std::make_shared<const Raw>(Raw(it.key()));
      const Erased key = box(keyOwner, keyOwner.get());
      // 键也要走一遍转换（DFU 的 convertMap 同样如此）。
      entries.emplace_back(convertTo(outOps, key), convertTo(outOps, box(input.owner, &it.value())));
    }
    return outOps.createMap(std::move(entries));
  }

 private:
  static const Raw* require(const Erased& value) {
    const Raw* node = value.as<Raw>();
    if (node == nullptr) {
      throw std::logic_error("JsonErasedOps: value does not hold a JSON node");
    }
    return node;
  }
};

// ---------------------------------------------------------------------------
// 第二种值类型（玩具 DOM）：证明擦除句柄不依赖 JSON / nlohmann。
// 形状故意和 nlohmann 不同：递归 variant + shared_ptr 子节点。
// ---------------------------------------------------------------------------
struct ToyNode;
using ToyList = std::vector<std::shared_ptr<const ToyNode>>;
using ToyMap = std::vector<std::pair<std::string, std::shared_ptr<const ToyNode>>>;

struct ToyNode {
  std::variant<std::monostate, bool, int64_t, double, std::string,
               std::shared_ptr<const ToyList>, std::shared_ptr<const ToyMap>>
      payload;
};

inline std::shared_ptr<const ToyNode> toyInt(int64_t value) {
  auto node = std::make_shared<ToyNode>();
  node->payload = value;
  return node;
}
inline std::shared_ptr<const ToyNode> toyString(std::string value) {
  auto node = std::make_shared<ToyNode>();
  node->payload = std::move(value);
  return node;
}

class ToyOps : public ErasedOps {
 public:
  static Erased box(const std::shared_ptr<const ToyNode>& node) {
    return Erased::of<ToyNode>(std::shared_ptr<const void>(node), node.get());
  }
  static const ToyNode* require(const Erased& value) {
    const ToyNode* node = value.as<ToyNode>();
    if (node == nullptr) {
      throw std::logic_error("ToyOps: value does not hold a ToyNode");
    }
    return node;
  }

  Erased createInt(int64_t value) const override { return box(toyInt(value)); }
  Erased createString(std::string value) const override { return box(toyString(std::move(value))); }
  Erased createList(std::vector<Erased> values) const override {
    auto list = std::make_shared<ToyList>();
    for (const Erased& value : values) {
      list->push_back(std::shared_ptr<const ToyNode>(value.owner, require(value)));
    }
    auto node = std::make_shared<ToyNode>();
    node->payload = std::shared_ptr<const ToyList>(list);
    return box(node);
  }
  Erased createMap(std::vector<std::pair<Erased, Erased>> entries) const override {
    auto map = std::make_shared<ToyMap>();
    for (const auto& entry : entries) {
      // key 也是一个（字符串）玩具节点：和 JSON 侧一样，键要能被 ops 读出来。
      const ToyNode* keyNode = require(entry.first);
      const auto* keyText = std::get_if<std::string>(&keyNode->payload);
      map->emplace_back(keyText != nullptr ? *keyText : std::string(),
                        std::shared_ptr<const ToyNode>(entry.second.owner, require(entry.second)));
    }
    auto node = std::make_shared<ToyNode>();
    node->payload = std::shared_ptr<const ToyMap>(map);
    return box(node);
  }

  std::optional<int64_t> getInt(const Erased& value) const override {
    const ToyNode* node = require(value);
    if (auto* number = std::get_if<int64_t>(&node->payload)) {
      return *number;
    }
    return std::nullopt;
  }
  std::optional<std::string> getString(const Erased& value) const override {
    const ToyNode* node = require(value);
    if (auto* text = std::get_if<std::string>(&node->payload)) {
      return *text;
    }
    return std::nullopt;
  }
  std::optional<std::vector<Erased>> getList(const Erased& value) const override {
    const ToyNode* node = require(value);
    const auto* list = std::get_if<std::shared_ptr<const ToyList>>(&node->payload);
    if (list == nullptr || !*list) {
      return std::nullopt;
    }
    std::vector<Erased> out;
    out.reserve((*list)->size());
    for (const auto& element : **list) {
      out.push_back(box(element));
    }
    return out;
  }
  std::optional<std::vector<std::pair<Erased, Erased>>> getMap(const Erased& value) const override {
    const ToyNode* node = require(value);
    const auto* map = std::get_if<std::shared_ptr<const ToyMap>>(&node->payload);
    if (map == nullptr || !*map) {
      return std::nullopt;
    }
    std::vector<std::pair<Erased, Erased>> out;
    out.reserve((*map)->size());
    for (const auto& entry : **map) {
      out.emplace_back(box(toyString(entry.first)), box(entry.second));
    }
    return out;
  }

  // 玩具 DOM → 任意 ops：递归搬运，证明 convertTo 是"由源 ops 实现的通用遍历"。
  Erased convertTo(const ErasedOps& outOps, const Erased& input) const override {
    const ToyNode* node = require(input);
    if (std::holds_alternative<int64_t>(node->payload)) {
      return outOps.createInt(std::get<int64_t>(node->payload));
    }
    if (std::holds_alternative<bool>(node->payload)) {
      return outOps.createInt(std::get<bool>(node->payload) ? 1 : 0);
    }
    if (std::holds_alternative<double>(node->payload)) {
      return outOps.createString(std::to_string(std::get<double>(node->payload)));
    }
    if (std::holds_alternative<std::string>(node->payload)) {
      return outOps.createString(std::get<std::string>(node->payload));
    }
    const auto list = getList(input);
    if (list.has_value()) {
      std::vector<Erased> elements;
      elements.reserve(list->size());
      for (const Erased& element : *list) {
        elements.push_back(convertTo(outOps, element));
      }
      return outOps.createList(std::move(elements));
    }
    const auto map = getMap(input);
    if (!map.has_value()) {
      throw std::logic_error("ToyOps::convertTo: node is neither a list nor a map");
    }
    std::vector<std::pair<Erased, Erased>> entries;
    entries.reserve(map->size());
    for (const auto& entry : *map) {
      // 键和值都必须经由 outOps 重建 —— 直接把本 ops 的句柄递过去
      // 会让目标 ops 读到错误的类型（这是原型里最容易踩的坑）。
      entries.emplace_back(convertTo(outOps, entry.first), convertTo(outOps, entry.second));
    }
    return outOps.createMap(std::move(entries));
  }
};

// ---------------------------------------------------------------------------
// 被测工作负载：从一份小文档里解出一个"结构体"，两条路径语义完全相同。
// （对应真实 codec 解码：getMap → 逐字段 getString/getInt/getList/getMap）
// ---------------------------------------------------------------------------
struct Row {
  std::string id;
  int64_t severity = 0;
  std::vector<std::string> tags;
  int64_t nestedX = 0;
  int64_t nestedY = 0;
};

inline bool operator==(const Row& a, const Row& b) {
  return a.id == b.id && a.severity == b.severity && a.tags == b.tags &&
         a.nestedX == b.nestedX && a.nestedY == b.nestedY;
}

inline Row decodeDirect(const DirectOps& ops, const codec::JsonValue& doc) {
  Row row;
  // 注意：必须先把 getMap 的结果存进局部量再遍历 ——
  // `for (auto& e : ops.getMap(doc).value())` 绑定的是临时 optional 内部的引用，
  // 生命周期不会延长（真实 codec 代码同样是先存局部量再读）。
  const std::vector<std::pair<codec::JsonValue, codec::JsonValue>> entries =
      ops.getMap(doc).value();
  for (const auto& entry : entries) {
    const std::string key = ops.getString(entry.first).value();
    if (key == "id") {
      row.id = ops.getString(entry.second).value();
    } else if (key == "severity") {
      row.severity = ops.getInt(entry.second).value();
    } else if (key == "tags") {
      const std::vector<codec::JsonValue> tags = ops.getList(entry.second).value();
      for (const codec::JsonValue& tag : tags) {
        row.tags.push_back(ops.getString(tag).value());
      }
    } else if (key == "nested") {
      const std::vector<std::pair<codec::JsonValue, codec::JsonValue>> nested =
          ops.getMap(entry.second).value();
      for (const auto& inner : nested) {
        const std::string innerKey = ops.getString(inner.first).value();
        if (innerKey == "x") {
          row.nestedX = ops.getInt(inner.second).value();
        } else if (innerKey == "y") {
          row.nestedY = ops.getInt(inner.second).value();
        }
      }
    }
  }
  return row;
}

inline Row decodeErased(const ErasedOps& ops, const Erased& doc) {
  Row row;
  const std::vector<std::pair<Erased, Erased>> entries = ops.getMap(doc).value();
  for (const auto& entry : entries) {
    const std::string key = ops.getString(entry.first).value();
    if (key == "id") {
      row.id = ops.getString(entry.second).value();
    } else if (key == "severity") {
      row.severity = ops.getInt(entry.second).value();
    } else if (key == "tags") {
      const std::vector<Erased> tags = ops.getList(entry.second).value();
      for (const Erased& tag : tags) {
        row.tags.push_back(ops.getString(tag).value());
      }
    } else if (key == "nested") {
      const std::vector<std::pair<Erased, Erased>> nested = ops.getMap(entry.second).value();
      for (const auto& inner : nested) {
        const std::string innerKey = ops.getString(inner.first).value();
        if (innerKey == "x") {
          row.nestedX = ops.getInt(inner.second).value();
        } else if (innerKey == "y") {
          row.nestedY = ops.getInt(inner.second).value();
        }
      }
    }
  }
  return row;
}

}  // namespace proto
