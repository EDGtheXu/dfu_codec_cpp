#pragma once
// ===========================================================================
// codec_json.hpp —— JSON 入口头（nlohmann/json 支撑的 JsonOps）
//
// 本库按「每种 DynamicOps 一个入口头」组织，include 即用：
//
//   codec.hpp        核：擦除值句柄 Value、DynamicOps/MapLike/构造器接口、
//                    codec/MapCodec 层、DataResult/Lifecycle、诊断与 Number。
//                    **不认识任何具体格式，也不依赖任何序列化库。**
//   codec_json.hpp   本文件：JSON 的全部——JsonValue（DFU 的 JsonElement，基于
//                    nlohmann::ordered_json 的引用语义句柄）、JsonParseError、
//                    JsonOps（INSTANCE / COMPRESSED）以及它的 MapLike 与
//                    构造器实现。
//   codec_toml.hpp   TOML：TomlDocument、TomlOps、dumpToml（只依赖核 + tinytoml）。
//
// 与核的互操作入口（核里不能出现 JSON 的名字，因此都定义在这里）：
//   * `JsonValue::operator Value()` —— 隐式转换，于是
//     `codec.parse(JsonOps::INSTANCE, JsonValue::parse(text))` 照旧可写；
//   * `jsonView(const Value&)` —— 把 Value 取回成 JsonValue（标签不匹配时抛
//     std::logic_error）。
//
// 依赖：核 `codec.hpp` + nlohmann/json 单头文件（third_party/nlohmann/json.hpp，
// 由 CMake 的 `codec_json` 目标携带）。核**永不** include 本文件。
// ===========================================================================

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "codec.hpp"

namespace codec {

class JsonParseError : public std::runtime_error {
 public:
  explicit JsonParseError(const std::string& message) : std::runtime_error(message) {}
};

// ---------------------------------------------------------------------------
// JsonValue —— 动态的已序列化值（DFU 的 "JsonElement"）
//
// 由 nlohmann::ordered_json 支撑：解析、序列化与 DOM 都归 nlohmann/json；
// 这一层门面只保留 codec 层需要的那一小套 API
// （类 Gson 的类型化访问器、null 过滤、有序成员访问以及
// Gson 风格的相等性）。
//
// 与 Gson 的 JsonElement 一样，JsonValue 具有*引用*语义：一个句柄拥有它
// 创建时来自的那份文档（shared_ptr），并指向其中的一个节点，因此
// 复制句柄、读取成员或遍历数组都是 O(1)，且从不
// 复制 DOM。值一旦构建完成即不可变。
// ---------------------------------------------------------------------------

class JsonValue {
 public:
  enum class Type { Null, Boolean, Number, String, Array, Object };
  using Raw = nlohmann::ordered_json;
  using Array = std::vector<JsonValue>;
  using Object = std::vector<std::pair<std::string, JsonValue>>;

  JsonValue();
  JsonValue(const Raw& raw);  // NOLINT(google-explicit-constructor) -- 复制到一个新节点
  JsonValue(Raw&& raw);       // NOLINT(google-explicit-constructor) -- 取得所有权

  // 零拷贝视图：`owner` 保持文档存活，`node` 必须指向其中的一个
  // 节点（或根节点）。内部用它在不复制的情况下交出成员与
  // 元素；也便于一次性包装一份大型 nlohmann 文档，
  // 之后通过开销很小的句柄遍历它。
  JsonValue(std::shared_ptr<const Raw> owner, const Raw* node)
      : owner_(std::move(owner)), node_(node) {}

  static JsonValue null() { return JsonValue(); }
  static JsonValue boolean(bool value) { return JsonValue(Raw(value)); }
  static JsonValue number(const Number& value);
  static JsonValue number(int32_t value);
  static JsonValue number(int64_t value);
  static JsonValue number(double value);
  static JsonValue number(float value);
  static JsonValue string(std::string value);
  static JsonValue array(Array value);
  static JsonValue object(Object value);

  // 用 nlohmann 的解析器解析严格 JSON；格式错误的输入会抛出
  // JsonParseError（包装 nlohmann::json::parse_error）。
  static JsonValue parse(std::string_view text);

  // 该句柄所指的 nlohmann 节点，供想直接使用
  // nlohmann/json 的应用使用。
  const Raw& raw() const { return *node_; }

  Type type() const;
  bool isNull() const { return type() == Type::Null; }
  bool isBoolean() const { return type() == Type::Boolean; }
  bool isNumber() const { return type() == Type::Number; }
  bool isString() const { return type() == Type::String; }
  bool isArray() const { return type() == Type::Array; }
  bool isObject() const { return type() == Type::Object; }

  // 类型化访问器；当值的类型不对时，各自抛出 std::runtime_error
  // （Gson 会抛出 ClassCastException / IllegalStateException）。
  bool asBoolean() const;
  Number asNumber() const;
  const std::string& asString() const;
  Array asArray() const;
  Object asObject() const;

  size_t size() const { return node_->size(); }

  // 对象查找。`find` 会给出显式为 JSON null 的成员；`get` 则把它
  // 视为缺失，这也正是 JsonOps 的 MapLike 的做法。
  std::optional<JsonValue> find(std::string_view key) const;
  std::optional<JsonValue> get(std::string_view key) const;
  bool contains(std::string_view key) const { return find(key).has_value(); }

  std::string dump(bool pretty = false, int indent = 2) const;
  std::string typeName() const { return std::string(node_->type_name()); }

  // --- 与擦除句柄 Value 互操作（阶段 1 新增） -----------------------------
  //
  // 交出「所有者 + 节点」这一对：Value 的布局与 JsonValue 完全相同，
  // 因此装箱只是复制 shared_ptr（一次原子加一），既不分配也不复制 DOM。
  std::shared_ptr<const void> ownerHandle() const { return owner_; }
  const void* nodeHandle() const { return node_; }
  // 等价于 Value(*this)（隐式转换），显式写法更好读。
  // JsonValue → Value：隐式转换（装箱只复制所有者，零分配、零原子）。
  operator Value() const;  // NOLINT(google-explicit-constructor)

  Value toValue() const;

  // 深度相等。对象比较不区分顺序，数值比较则与
  // Gson 的 JsonPrimitive 一致（42 == 42.0）。
  bool equals(const JsonValue& other) const;
  // 深度相等，但还要求成员顺序完全一致。
  bool equalsOrdered(const JsonValue& other) const;
  bool operator==(const JsonValue& other) const { return equals(other); }
  bool operator!=(const JsonValue& other) const { return !equals(other); }

 private:
  std::shared_ptr<const Raw> owner_;
  const Raw* node_ = nullptr;
};

namespace detail {
inline const std::shared_ptr<const nlohmann::ordered_json>& nullNodeOwner() {
  static const std::shared_ptr<const nlohmann::ordered_json> owner =
      std::make_shared<const nlohmann::ordered_json>();
  return owner;
}
}  // namespace detail

namespace detail {

// JsonValue::asNumber() 的取值规则（整数 / 无符号 / 浮点），节点级实现：
// 供 JsonOps 与节点级比较复用，避免临时构造 JsonValue 句柄。
inline Number numberOf(const JsonValue::Raw& node) {
  if (node.is_number_integer()) {
    return Number::ofInt(node.get<int64_t>());
  }
  if (node.is_number_unsigned()) {
    const uint64_t unsignedValue = node.get<uint64_t>();
    if (unsignedValue <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      return Number::ofInt(static_cast<int64_t>(unsignedValue));
    }
    return Number::ofDouble(static_cast<double>(unsignedValue));
  }
  return Number::ofDouble(node.get<double>());
}

// 节点级深度相等：与 JsonValue::equals 的语义逐字对应（类型严格、
// 数值按 Gson 的 JsonPrimitive 规则、对象不区分顺序），但不构造任何句柄，
// 因此没有引用计数开销。
inline bool jsonNodesEqual(const JsonValue::Raw& left, const JsonValue::Raw& right) {
  const auto kind = [](const JsonValue::Raw& node) {
    switch (node.type()) {
      case JsonValue::Raw::value_t::boolean:
        return JsonValue::Type::Boolean;
      case JsonValue::Raw::value_t::number_integer:
      case JsonValue::Raw::value_t::number_unsigned:
      case JsonValue::Raw::value_t::number_float:
        return JsonValue::Type::Number;
      case JsonValue::Raw::value_t::string:
        return JsonValue::Type::String;
      case JsonValue::Raw::value_t::array:
        return JsonValue::Type::Array;
      case JsonValue::Raw::value_t::object:
        return JsonValue::Type::Object;
      case JsonValue::Raw::value_t::null:
      case JsonValue::Raw::value_t::discarded:
      case JsonValue::Raw::value_t::binary:
        return JsonValue::Type::Null;
    }
    return JsonValue::Type::Null;
  };
  const JsonValue::Type type = kind(left);
  if (type != kind(right)) {
    return false;
  }
  switch (type) {
    case JsonValue::Type::Null:
      return true;
    case JsonValue::Type::Boolean:
      return left.get<bool>() == right.get<bool>();
    case JsonValue::Type::Number:
      return numberOf(left).equals(numberOf(right));
    case JsonValue::Type::String:
      return left.get_ref<const std::string&>() == right.get_ref<const std::string&>();
    case JsonValue::Type::Array: {
      if (left.size() != right.size()) {
        return false;
      }
      for (size_t i = 0; i < left.size(); ++i) {
        if (!jsonNodesEqual(left[i], right[i])) {
          return false;
        }
      }
      return true;
    }
    case JsonValue::Type::Object: {
      if (left.size() != right.size()) {
        return false;
      }
      for (auto it = left.begin(); it != left.end(); ++it) {
        const auto theirs = right.find(it.key());
        if (theirs == right.end() || !jsonNodesEqual(it.value(), theirs.value())) {
          return false;
        }
      }
      return true;
    }
  }
  return false;
}

}  // namespace detail

inline JsonValue::JsonValue()
    : owner_(detail::nullNodeOwner()), node_(owner_.get()) {}

inline JsonValue::JsonValue(const Raw& raw)
    : owner_(std::make_shared<const Raw>(raw)), node_(owner_.get()) {}

inline JsonValue::JsonValue(Raw&& raw)
    : owner_(std::make_shared<const Raw>(std::move(raw))), node_(owner_.get()) {}

inline JsonValue JsonValue::number(const Number& value) {
  return value.isIntegral() ? JsonValue(Raw(value.longValue()))
                            : JsonValue(Raw(value.doubleValue()));
}
inline JsonValue JsonValue::number(int32_t value) {
  return JsonValue(Raw(static_cast<int64_t>(value)));
}
inline JsonValue JsonValue::number(int64_t value) { return JsonValue(Raw(value)); }
inline JsonValue JsonValue::number(double value) { return JsonValue(Raw(value)); }
inline JsonValue JsonValue::number(float value) {
  return JsonValue(Raw(static_cast<double>(value)));
}
inline JsonValue JsonValue::string(std::string value) {
  Raw raw = Raw::value_t::string;
  raw.get_ref<std::string&>() = std::move(value);
  return JsonValue(std::move(raw));
}

inline JsonValue JsonValue::array(Array value) {
  Raw raw = Raw::array();
  for (const JsonValue& element : value) {
    raw.push_back(element.raw());
  }
  return JsonValue(std::move(raw));
}

inline JsonValue JsonValue::object(Object value) {
  Raw raw = Raw::object();
  for (const auto& member : value) {
    // nlohmann 的有序对象会就地替换已存在的成员，与
    // Gson 由 LinkedTreeMap 支撑的 JsonObject#add 一致。
    raw[member.first] = member.second.raw();
  }
  return JsonValue(std::move(raw));
}

inline JsonValue::Type JsonValue::type() const {
  switch (node_->type()) {
    case Raw::value_t::null:
    case Raw::value_t::discarded:
      return Type::Null;
    case Raw::value_t::boolean:
      return Type::Boolean;
    case Raw::value_t::number_integer:
    case Raw::value_t::number_unsigned:
    case Raw::value_t::number_float:
      return Type::Number;
    case Raw::value_t::string:
      return Type::String;
    case Raw::value_t::array:
      return Type::Array;
    case Raw::value_t::object:
      return Type::Object;
    case Raw::value_t::binary:
      // nlohmann 的 binary 值不属于 codec 所操作的 JSON 数据
      // 模型；它们会被报告为 null。
      return Type::Null;
  }
  return Type::Null;
}

inline bool JsonValue::asBoolean() const {
  if (!isBoolean()) {
    throw std::runtime_error("JsonValue is not a boolean: " + dump());
  }
  return node_->get<bool>();
}

inline Number JsonValue::asNumber() const {
  if (!isNumber()) {
    throw std::runtime_error("JsonValue is not a number: " + dump());
  }
  return detail::numberOf(*node_);
}

inline const std::string& JsonValue::asString() const {
  if (!isString()) {
    throw std::runtime_error("JsonValue is not a string: " + dump());
  }
  return node_->get_ref<const std::string&>();
}

inline JsonValue::Array JsonValue::asArray() const {
  if (!isArray()) {
    throw std::runtime_error("JsonValue is not an array: " + dump());
  }
  // 各句柄共享这份文档，因此不会复制任何节点。
  Array out;
  out.reserve(node_->size());
  for (const Raw& element : *node_) {
    out.emplace_back(owner_, &element);
  }
  return out;
}

inline JsonValue::Object JsonValue::asObject() const {
  if (!isObject()) {
    throw std::runtime_error("JsonValue is not an object: " + dump());
  }
  Object out;
  out.reserve(node_->size());
  for (auto it = node_->begin(); it != node_->end(); ++it) {
    out.emplace_back(it.key(), JsonValue(owner_, &it.value()));
  }
  return out;
}

inline std::optional<JsonValue> JsonValue::find(std::string_view key) const {
  if (!isObject()) {
    return std::nullopt;
  }
  const auto it = node_->find(key);
  if (it == node_->end()) {
    return std::nullopt;
  }
  return JsonValue(owner_, &it.value());
}

inline std::optional<JsonValue> JsonValue::get(std::string_view key) const {
  const std::optional<JsonValue> found = find(key);
  if (!found.has_value() || found->isNull()) {
    return std::nullopt;
  }
  return found;
}

inline std::string JsonValue::dump(bool pretty, int indent) const {
  return pretty ? node_->dump(indent < 0 ? 2 : indent) : node_->dump();
}

inline bool JsonValue::equals(const JsonValue& other) const {
  // 语义与原先逐字一致，只是改为节点级比较（不构造句柄、不做引用计数）。
  return type() == other.type() && detail::jsonNodesEqual(*node_, *other.node_);
}

inline bool JsonValue::equalsOrdered(const JsonValue& other) const {
  if (type() != other.type()) {
    return false;
  }
  if (type() == Type::Object) {
    if (size() != other.size()) {
      return false;
    }
    auto mine = node_->begin();
    auto theirs = other.node_->begin();
    for (; mine != node_->end(); ++mine, ++theirs) {
      if (mine.key() != theirs.key()) {
        return false;
      }
      if (!JsonValue(owner_, &mine.value()).equalsOrdered(JsonValue(other.owner_, &theirs.value()))) {
        return false;
      }
    }
    return true;
  }
  if (type() == Type::Array) {
    if (size() != other.size()) {
      return false;
    }
    const Array mine = asArray();
    const Array theirs = other.asArray();
    for (size_t i = 0; i < mine.size(); ++i) {
      if (!mine[i].equalsOrdered(theirs[i])) {
        return false;
      }
    }
    return true;
  }
  return equals(other);
}

inline JsonValue JsonValue::parse(std::string_view text) {
  try {
    return JsonValue(Raw::parse(text.begin(), text.end()));
  } catch (const nlohmann::json::exception& error) {
    throw JsonParseError(error.what());
  }
}

inline std::ostream& operator<<(std::ostream& os, const JsonValue& value) {
  return os << value.dump();
}

// ---------------------------------------------------------------------------
// 与核的互操作：Value ↔ JsonValue
//
// 核里没有 JSON 的名字，所以这两个入口必须在这里定义（一个成员转换运算符、
// 一个自由函数）。
// ---------------------------------------------------------------------------

// JsonValue → Value：隐式（装箱只复制所有者，零分配、零原子）。
inline JsonValue::operator Value() const {
  return Value::of<JsonValue::Raw>(owner_, node_);
}

inline Value JsonValue::toValue() const { return Value(*this); }

inline JsonValue jsonView(const Value& value) {
  const JsonValue::Raw* raw = value.as<JsonValue::Raw>();
  if (raw == nullptr) {
    throw std::logic_error("jsonView(): the handle does not hold a JSON node");
  }
  return JsonValue(std::static_pointer_cast<const JsonValue::Raw>(value.owner), raw);
}

// Gson 的 JsonObject.add：已存在的成员会被原地替换（该对象
// 由 LinkedTreeMap 支撑），这正是 DFU 的 JsonOps 所依赖的行为。
// 同一个语义，但直接作用于 nlohmann 节点：写路径（builder / mergeToMap）
// 因此不必为每个成员构造 JsonValue 句柄。
inline void putRawMember(JsonValue::Raw& object, const std::string& key,
                         const JsonValue::Raw& value) {
  for (auto it = object.begin(); it != object.end(); ++it) {
    if (it.key() == key) {
      it.value() = value;  // 原地替换，保持成员顺序
      return;
    }
  }
  object[key] = value;  // 新成员追加在末尾
}

inline void putJsonMember(JsonValue::Object& members, const std::string& key,
                          const JsonValue& value) {
  for (auto& member : members) {
    if (member.first == key) {
      member.second = value;
      return;
    }
  }
  members.emplace_back(key, value);
}

// 由 JsonValue 对象支撑的 MapLike（JsonOps.getMap）。
class JsonObjectMapLike : public MapLike {
 public:
  explicit JsonObjectMapLike(JsonValue object)
      : owner_(object.ownerHandle()), node_(&object.raw()) {}
  JsonObjectMapLike(std::shared_ptr<const void> owner, const JsonValue::Raw* node)
      : owner_(std::move(owner)), node_(node) {}

  std::optional<Value> get(const Value& keyHandle) const override {
    // 只在节点上判断类型，不构造 JsonValue。
    const JsonValue::Raw* key = keyHandle.as<JsonValue::Raw>();
    if (key == nullptr || !key->is_string()) {
      return std::nullopt;
    }
    return get(key->get_ref<const std::string&>());
  }

  std::optional<Value> get(const std::string& key) const override {
    // JsonValue::get 的语义：显式的 JSON null 视为「缺失」。
    if (!node_->is_object()) {
      return std::nullopt;
    }
    const auto it = node_->find(key);
    if (it == node_->end() || it->is_null()) {
      return std::nullopt;
    }
    return Value::ofChild(owner_, &it.value(), tagOf<JsonValue::Raw>());
  }

  std::vector<std::pair<Value, Value>> entries() const override {
    std::vector<std::pair<Value, Value>> out;
    if (!node_->is_object()) {
      return out;
    }
    out.reserve(node_->size());
    for (auto it = node_->begin(); it != node_->end(); ++it) {
      // 键在 nlohmann 里是 std::string 而不是节点，因此装箱键要建一个节点；
      // 值只借子节点（一次引用计数拷贝）。
      out.emplace_back(Value(JsonValue::string(it.key())),
                       Value::ofChild(owner_, &it.value(), tagOf<JsonValue::Raw>()));
    }
    return out;
  }

  // 兼容入口：需要 JsonValue 视图时按需构造（读路径不再走它）。
  JsonValue object() const {
    return JsonValue(std::static_pointer_cast<const JsonValue::Raw>(owner_), node_);
  }

  std::string toString() const override { return "MapLike[" + node_->dump() + "]"; }

 private:
  std::shared_ptr<const void> owner_;
  const JsonValue::Raw* node_;
};

class ArrayListBuilder : public ListBuilder {
 public:
  // 累加器保存装箱后的句柄（每个元素一次引用计数拷贝，无法避免：
  // 句柄必须让文档存活），而不是 JsonValue——写路径因此不再构造 JSON 视图。
  using State = std::shared_ptr<std::vector<Value>>;

  explicit ArrayListBuilder(const DynamicOps& ops)
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

inline DataResult<Value> ArrayListBuilder::build(const Value& prefix) {
  // prefix 与被追加的元素都在节点层面处理，不构造 JsonValue。
  const JsonValue::Raw* prefixNode = prefix.as<JsonValue::Raw>();
  // 与原先一致：prefix 与 empty() 按 ops 的深度相等比较（不是节点身份），
  // 否则"另一个 null 节点"会被当成"不是列表"而报错。
  const bool prefixIsEmpty = ops_->valueEquals(prefix, ops_->empty());
  DataResult<Value> result =
      builder_.flatMap([&](const State& array) -> DataResult<Value> {
        if (prefixNode == nullptr) {
          return DataResult<Value>::error(
              "Cannot append a list to not a list: " + ops_->toString(prefix), prefix);
        }
        if (!prefixNode->is_array() && !prefixIsEmpty) {
          return DataResult<Value>::error("Cannot append a list to not a list: " + prefixNode->dump(),
                                              prefix);
        }
        JsonValue::Raw out = prefixNode->is_array() ? *prefixNode : JsonValue::Raw::array();
        for (const Value& element : *array) {
          const JsonValue::Raw* node = element.as<JsonValue::Raw>();
          if (node == nullptr) {
            return DataResult<Value>::error("Cannot append a non-JSON value to a list", prefix);
          }
          out.push_back(*node);
        }
        auto owner = std::make_shared<const JsonValue::Raw>(std::move(out));
        return DataResult<Value>::success(
            Value::of<JsonValue::Raw>(std::move(owner), owner.get()), Lifecycle::stable());
      });
  builder_ = DataResult<State>::success(initial(), Lifecycle::stable());
  return result;
}

// RecordBuilder.AbstractStringBuilder / JsonOps.JsonRecordBuilder 的移植。
class StringRecordBuilder
    : public AbstractRecordBuilder<std::shared_ptr<std::vector<std::pair<std::string, Value>>>> {
 public:
  // 与 ArrayListBuilder 同理：成员值是装箱句柄，写盘时才落进 DOM。
  using Members = std::vector<std::pair<std::string, Value>>;
  using State = std::shared_ptr<Members>;

  explicit StringRecordBuilder(const DynamicOps& ops) : AbstractRecordBuilder<State>(ops, initial()) {}

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
        // 编码失败会携带其来源成员。
        value.addPath(key));
    return *this;
  }

  RecordBuilder& add(const Value& key, const Value& value) override;
  RecordBuilder& add(const Value& key, const DataResult<Value>& value) override;
  RecordBuilder& add(const DataResult<Value>& key, const DataResult<Value>& value) override;

  static State initial() { return std::make_shared<Members>(); }

 protected:
  State initBuilder() const override { return initial(); }
  DataResult<Value> buildState(const State& state, const Value& prefix) override;
};

// 移植自 RecordBuilder.AbstractStringBuilder#add(T key, ...)。
inline RecordBuilder& StringRecordBuilder::add(const Value& key, const Value& value) {
  builder_ = ops().getStringValue(key).flatMap([this, value](const std::string& k) {
    add(k, value);
    return builder_;
  });
  return *this;
}

inline RecordBuilder& StringRecordBuilder::add(const Value& key,
                                              const DataResult<Value>& value) {
  builder_ = ops().getStringValue(key).flatMap([this, value](const std::string& k) {
    add(k, value);
    return builder_;
  });
  return *this;
}

inline RecordBuilder& StringRecordBuilder::add(const DataResult<Value>& key,
                                              const DataResult<Value>& value) {
  builder_ = key.flatMap([this](const Value& k) { return ops().getStringValue(k); })
                 .flatMap([this, value](const std::string& k) {
                   add(k, value);
                   return builder_;
                 });
  return *this;
}

inline DataResult<Value> StringRecordBuilder::buildState(const State& state,
                                                             const Value& prefix) {
  // 移植自 JsonOps.JsonRecordBuilder#build。prefix 与成员值都在节点上处理。
  const JsonValue::Raw* prefixNode = prefix.as<JsonValue::Raw>();
  if (prefixNode == nullptr) {
    return DataResult<Value>::error("mergeToMap called with not a map: " + ops_->toString(prefix),
                                    prefix);
  }
  const bool prefixIsEmpty = ops_->valueEquals(prefix, ops_->empty());
  if (!prefixIsEmpty && !prefixNode->is_object()) {
    // 非对象（也不是 null/空）的 prefix 要报错，与移植前一致。
    return DataResult<Value>::error("mergeToMap called with not a map: " + prefixNode->dump(),
                                    prefix);
  }
  JsonValue::Raw members =
      (prefixNode->is_object() && !prefixIsEmpty) ? *prefixNode : JsonValue::Raw::object();
  for (const auto& entry : *state) {
    const JsonValue::Raw* value = entry.second.as<JsonValue::Raw>();
    if (value == nullptr) {
      return DataResult<Value>::error("Cannot write a non-JSON value into a map", prefix);
    }
    putRawMember(members, entry.first, *value);
  }
  auto owner = std::make_shared<const JsonValue::Raw>(std::move(members));
  return DataResult<Value>::success(Value::of<JsonValue::Raw>(std::move(owner), owner.get()));
}

// ===========================================================================
// 5/8  json_ops.hpp -- JsonOps (INSTANCE / COMPRESSED)
// ===========================================================================

// Mojang DataFixerUpper `com.mojang.serialization` Codec API 的 C++17 移植。
//
// json_ops.hpp -- com.mojang.serialization.JsonOps 的移植（由
// JsonValue DOM 支撑，而非 Gson 的 JsonElement）。




class JsonOps : public DynamicOps {
 public:
  static const JsonOps INSTANCE;
  static const JsonOps COMPRESSED;

  explicit JsonOps(bool compressed) : compressed_(compressed), empty_(JsonValue::null()) {}

  // 重新暴露 JsonOps 特化的 DynamicOps 重载集合，使
  // `ops.getNumberValue(value, fallback)` / `ops.mergeToMap(map, mapLike)` 在
  // JsonOps 实例上仍能正常工作。
  using DynamicOps::getNumberValue;
  using DynamicOps::mergeToMap;

  // --- 基础类型 ---------------------------------------------------------
  // empty() 是"JSON null"这个不变值：缓存成成员，省掉每次调用都新建空值
  // （以及随之而来的引用计数）。
  Value empty() const override { return empty_; }

  // DFU 的 convertTo：把值重写成目标 ops 的表示（同一个 ops 时恒等）。
  // 由**源** ops 递归遍历自己的 DOM、逐节点调用目标 ops 的 createX；
  // JSON 的 null 映射为 outOps.empty()（保持全函数语义——真正的拒绝发生在
  // 写出前的校验里，例如 dumpToml）。
  Value convertTo(const DynamicOps& outOps, const Value& input) const override {
    if (&outOps == this) {
      return input;
    }
    return convertNode(outOps, *requireNode(input));
  }

  // ops 自己的名字（诊断用）。
  std::string toString() const { return "JSON"; }

  // 把值渲染成文本（错误消息用）：JSON 就是 dump。
  std::string toString(const Value& input) const override { return requireNode(input)->dump(); }

  // 句柄表示同一个值吗？基类靠它实现 DFU 的 `prefix.equals(empty())`。
  // 节点级深度相等，语义与 JsonValue::equals 逐字对应，但不构造句柄。
  bool valueEquals(const Value& left, const Value& right) const override {
    const JsonValue::Raw* a = left.as<JsonValue::Raw>();
    const JsonValue::Raw* b = right.as<JsonValue::Raw>();
    if (a == nullptr || b == nullptr) {
      return a == nullptr && b == nullptr && left.node == right.node && left.tag == right.tag;
    }
    return detail::jsonNodesEqual(*a, *b);
  }

  DataResult<Number> getNumberValue(const Value& input) const override {
    const JsonValue::Raw* node = requireNode(input);
    if (node->is_number()) {
      return DataResult<Number>::success(detail::numberOf(*node));
    }
    if (node->is_boolean()) {
      return DataResult<Number>::success(Number::ofInt(node->get<bool>() ? 1 : 0));
    }
    if (compressed_ && node->is_string()) {
      const std::string& text = node->get_ref<const std::string&>();
      char* end = nullptr;
      const long long value = std::strtoll(text.c_str(), &end, 10);
      if (end != nullptr && *end == '\0' && !text.empty()) {
        return DataResult<Number>::success(Number::ofInt(static_cast<int64_t>(value)));
      }
      return DataResult<Number>::error("Not a number: NumberFormatException " + text);
    }
    return DataResult<Number>::error("Not a number: " + node->dump());
  }

  Value createNumeric(const Number& value) const override {
    return box(value.isIntegral() ? JsonValue::Raw(value.longValue())
                                  : JsonValue::Raw(value.doubleValue()));
  }

  DataResult<bool> getBooleanValue(const Value& input) const override {
    const JsonValue::Raw* node = requireNode(input);
    if (node->is_boolean()) {
      return DataResult<bool>::success(node->get<bool>());
    }
    if (node->is_number()) {
      return DataResult<bool>::success(detail::numberOf(*node).byteValue() != 0);
    }
    return DataResult<bool>::error("Not a boolean: " + node->dump());
  }

  Value createBoolean(bool value) const override { return box(JsonValue::Raw(value)); }

  DataResult<std::string> getStringValue(const Value& input) const override {
    const JsonValue::Raw* node = requireNode(input);
    if (node->is_string()) {
      return DataResult<std::string>::success(node->get_ref<const std::string&>());
    }
    if (compressed_ && node->is_number()) {
      return DataResult<std::string>::success(detail::numberOf(*node).toString());
    }
    return DataResult<std::string>::error("Not a string: " + node->dump());
  }

  Value createString(const std::string& value) const override {
    return box(JsonValue::Raw(value));
  }

  // --- list/map 构造 ---------------------------------------------
  DataResult<Value> mergeToList(const Value& list, const Value& value) const override {
    const JsonValue::Raw* listNode = requireNode(list);
    const JsonValue::Raw* valueNode = requireNode(value);
    if (!listNode->is_array() && !valueEquals(list, empty_)) {
      return DataResult<Value>::error("mergeToList called with not a list: " + listNode->dump(),
                                      list);
    }
    JsonValue::Raw out = listNode->is_array() ? *listNode : JsonValue::Raw::array();
    out.push_back(*valueNode);
    return DataResult<Value>::success(box(std::move(out)));
  }

  DataResult<Value> mergeToList(const Value& list,
                                    const std::vector<Value>& values) const override {
    const JsonValue::Raw* listNode = requireNode(list);
    if (!listNode->is_array() && !valueEquals(list, empty_)) {
      return DataResult<Value>::error("mergeToList called with not a list: " + listNode->dump(),
                                      list);
    }
    JsonValue::Raw out = listNode->is_array() ? *listNode : JsonValue::Raw::array();
    for (const Value& value : values) {
      out.push_back(*requireNode(value));
    }
    return DataResult<Value>::success(box(std::move(out)));
  }

  DataResult<Value> mergeToMap(const Value& map, const Value& key,
                                   const Value& value) const override {
    const JsonValue::Raw* mapNode = requireNode(map);
    const JsonValue::Raw* keyNode = requireNode(key);
    const JsonValue::Raw* valueNode = requireNode(value);
    if (!mapNode->is_object() && !valueEquals(map, empty_)) {
      return DataResult<Value>::error("mergeToMap called with not a map: " + mapNode->dump(), map);
    }
    const std::optional<std::string> keyString = asKeyString(*keyNode);
    if (!keyString.has_value()) {
      return DataResult<Value>::error("key is not a string: " + keyNode->dump(), map);
    }
    JsonValue::Raw out = mapNode->is_object() ? *mapNode : JsonValue::Raw::object();
    putRawMember(out, *keyString, *valueNode);
    return DataResult<Value>::success(box(std::move(out)));
  }

  DataResult<Value> mergeToMap(
      const Value& map,
      const std::vector<std::pair<Value, Value>>& values) const override {
    const JsonValue::Raw* mapNode = requireNode(map);
    if (!mapNode->is_object() && !valueEquals(map, empty_)) {
      return DataResult<Value>::error("mergeToMap called with not a map: " + mapNode->dump(), map);
    }
    JsonValue::Raw out = mapNode->is_object() ? *mapNode : JsonValue::Raw::object();
    std::vector<JsonValue::Raw> missed;
    for (const auto& entry : values) {
      const JsonValue::Raw* keyNode = requireNode(entry.first);
      const std::optional<std::string> keyString = asKeyString(*keyNode);
      if (keyString.has_value()) {
        putRawMember(out, *keyString, *requireNode(entry.second));
      } else {
        missed.push_back(*keyNode);
      }
    }
    if (!missed.empty()) {
      JsonValue::Raw missedArray = JsonValue::Raw::array();
      for (const JsonValue::Raw& key : missed) {
        missedArray.push_back(key);
      }
      return DataResult<Value>::error("some keys are not strings: " + missedArray.dump(),
                                          Value(JsonValue(std::move(out))));
    }
    return DataResult<Value>::success(box(std::move(out)));
  }

  // --- map 访问 ---------------------------------------------------------
  DataResult<std::vector<std::pair<Value, Value>>> getMapValues(
      const Value& input) const override {
    const JsonValue::Raw* node = requireNode(input);
    if (!node->is_object()) {
      return DataResult<std::vector<std::pair<Value, Value>>>::error("Not a JSON object: " +
                                                                         node->dump());
    }
    std::vector<std::pair<Value, Value>> out;
    out.reserve(node->size());
    for (auto it = node->begin(); it != node->end(); ++it) {
      out.emplace_back(box(JsonValue::Raw(it.key())),
                       Value::ofChild(input.owner, &it.value(), tagOf<JsonValue::Raw>()));
    }
    return DataResult<std::vector<std::pair<Value, Value>>>::success(std::move(out));
  }

  DataResult<MapLikePtr> getMap(const Value& input) const override {
    const JsonValue::Raw* node = requireNode(input);
    if (!node->is_object()) {
      return DataResult<MapLikePtr>::error("Not a JSON object: " + node->dump());
    }
    return DataResult<MapLikePtr>::success(std::make_shared<JsonObjectMapLike>(input.owner, node));
  }

  bool isStringKey(const Value& key) const override {
    const JsonValue::Raw* node = key.as<JsonValue::Raw>();
    return node != nullptr && node->is_string();
  }

  Value createMap(const std::vector<std::pair<Value, Value>>& entries) const override {
    JsonValue::Raw out = JsonValue::Raw::object();
    for (const auto& entry : entries) {
      const std::optional<std::string> keyString = asKeyString(*requireNode(entry.first));
      if (keyString.has_value()) {
        putRawMember(out, *keyString, *requireNode(entry.second));
      }
    }
    return box(std::move(out));
  }

  // --- list 访问 --------------------------------------------------------
  DataResult<std::vector<Value>> getStream(const Value& input) const override {
    const JsonValue::Raw* node = requireNode(input);
    if (!node->is_array()) {
      return DataResult<std::vector<Value>>::error("Not a json array: " + node->dump());
    }
    std::vector<Value> out;
    out.reserve(node->size());
    for (auto it = node->begin(); it != node->end(); ++it) {
      // 元素只借子节点：一次引用计数拷贝（旧实现先造 JsonValue 再装箱，两次）。
      out.push_back(Value::ofChild(input.owner, &*it, tagOf<JsonValue::Raw>()));
    }
    return DataResult<std::vector<Value>>::success(std::move(out));
  }

  DataResult<std::vector<Value>> getList(const Value& input) const override {
    return getStream(input);
  }

  Value createList(const std::vector<Value>& values) const override {
    JsonValue::Raw out = JsonValue::Raw::array();
    for (const Value& value : values) {
      out.push_back(*requireNode(value));
    }
    return box(std::move(out));
  }

  Value remove(const Value& input, const std::string& key) const override {
    const JsonValue::Raw* node = requireNode(input);
    if (!node->is_object()) {
      return input;
    }
    JsonValue::Raw out = JsonValue::Raw::object();
    for (auto it = node->begin(); it != node->end(); ++it) {
      if (it.key() != key) {
        out[it.key()] = it.value();
      }
    }
    return box(std::move(out));
  }

  // --- 行为 ----------------------------------------------------------
  bool compressMaps() const override { return compressed_; }

  std::shared_ptr<ListBuilder> listBuilder() const override {
    return std::make_shared<ArrayListBuilder>(*this);
  }

  std::shared_ptr<RecordBuilder> mapBuilder() const override {
    return std::make_shared<StringRecordBuilder>(*this);
  }

 private:
  // 借出节点：只读标签、不复制所有者。标签不匹配时抛，与 jsonView() 同样的
  // 异常类型与措辞（Value 已不再有 asJson() 成员——核里没有 JSON 的名字）。
  static const JsonValue::Raw* requireNode(const Value& value) {
    const JsonValue::Raw* node = value.as<JsonValue::Raw>();
    if (node == nullptr) {
      throw std::logic_error("jsonView(): the handle does not hold a JSON node");
    }
    return node;
  }

  // 把刚建好的节点装箱：一次分配、零引用计数操作（所有者直接 move 进去）。
  static Value box(JsonValue::Raw&& node) {
    auto owner = std::make_shared<const JsonValue::Raw>(std::move(node));
    return Value::of<JsonValue::Raw>(std::move(owner), owner.get());
  }

  // 以 JSON 节点为源、逐节点导出到目标 ops（convertTo 的递归实现）。
  static Value convertNode(const DynamicOps& outOps, const JsonValue::Raw& node) {
    switch (node.type()) {
      case JsonValue::Raw::value_t::boolean:
        return outOps.createBoolean(node.get<bool>());
      case JsonValue::Raw::value_t::number_integer:
      case JsonValue::Raw::value_t::number_unsigned:
      case JsonValue::Raw::value_t::number_float:
        return outOps.createNumeric(detail::numberOf(node));
      case JsonValue::Raw::value_t::string:
        return outOps.createString(node.get_ref<const std::string&>());
      case JsonValue::Raw::value_t::array: {
        std::vector<Value> elements;
        for (auto it = node.begin(); it != node.end(); ++it) {
          elements.push_back(convertNode(outOps, *it));
        }
        return outOps.createList(std::move(elements));
      }
      case JsonValue::Raw::value_t::object: {
        std::vector<std::pair<Value, Value>> entries;
        for (auto it = node.begin(); it != node.end(); ++it) {
          entries.emplace_back(outOps.createString(it.key()), convertNode(outOps, it.value()));
        }
        return outOps.createMap(std::move(entries));
      }
      case JsonValue::Raw::value_t::null:
      case JsonValue::Raw::value_t::discarded:
      case JsonValue::Raw::value_t::binary:
        return outOps.empty();
    }
    return outOps.empty();
  }

  // Java 的 `key.getAsString()`：字符串，或压缩模式下的数字。
  std::optional<std::string> asKeyString(const JsonValue::Raw& key) const {
    if (key.is_string()) {
      return key.get_ref<const std::string&>();
    }
    if (compressed_ && key.is_number()) {
      return detail::numberOf(key).toString();
    }
    return std::nullopt;
  }

  bool compressed_ = false;
  // empty() 的不变值（构造一次，之后每次返回一次引用计数拷贝）。
  const Value empty_;
};

inline const JsonOps JsonOps::INSTANCE{false};
inline const JsonOps JsonOps::COMPRESSED{true};


}  // namespace codec
