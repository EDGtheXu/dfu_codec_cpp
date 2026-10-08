// Mojang DataFixerUpper `com.mojang.serialization` Codec API 的 C++17 移植。
//
//                              codec.hpp
//                  单头文件、header-only 库
//
// 对 DataFixerUpper 6.0.8（com.mojang:datafixerupper）中 Codec / MapCodec /
// RecordCodecBuilder / DynamicOps / DataResult 机制的忠实移植，
// 也就是 Minecraft 用来序列化与反序列化从注册表条目到世界数据
// 的一切内容的那套机制。
//
// 用法
// -----
//     #include "codec.hpp"
//
// 一切都在 `namespace codec` 中；CMake 目标是 `codec`
// （别名 `codec::codec`），一个 INTERFACE 库，因此无需构建任何东西。
//
//     Codec<RiskDef> riskDefCodec = record<RiskDef>(
//         fieldOf("id",  &RiskDef::id,  codecs::String),
//         fieldOf("vid", &RiskDef::vid, codecs::String),
//         optionalFieldOf("condition", &RiskDef::condition, ConditionCodec));
//
//     DataResult<RiskDef> decoded = riskDefCodec.parse(JsonOps::INSTANCE,
//                                                       JsonValue::parse(text));
//     DataResult<JsonValue> encoded = riskDefCodec.encodeStart(JsonOps::INSTANCE, value);
//
// 依赖
// ------------
//   * C++17
//   * nlohmann/json 3.12.0 —— JsonValue 背后的 JSON 解析器/序列化器/DOM
//     （只有下面的 JsonValue 一节用到它；codec 层从不会看到它）。
//     用 scripts/fetch_deps.ps1 把它拉到 third_party/。
//
// 内容
// --------
// 各小节按依赖顺序排列，并以这个单文件所取代的
// 各分区头文件命名；形如 "see dynamic_ops.hpp" 的注释
// 指的是下面同名的小节。
//
//   1. json          JsonValue（基于 nlohmann）+ Number（java.lang.Number）
//   2. lifecycle     Lifecycle
//   3. data_result   DataResult、PartialResult 语义、Unit
//   4. dynamic_ops   DynamicOps、MapLike、RecordBuilder、ListBuilder、KeyCompressor
//   5. json_ops      JsonOps（INSTANCE / COMPRESSED）
//   6. codec         Encoder、Decoder、MapEncoder、MapDecoder、Codec、MapCodec
//   7. codecs        基础与组合 codec、范围校验、recursive、dispatch
//   8. record_codec  RecordCodecBuilder：record<>、fieldOf、optionalFieldOf、forGetter
//
// 行为、对 DFU 的有意偏离以及各测试层都记录在
// README.md（README_zh.md）中。参考 Java 源码不属于这个
// 库；scripts/decompile_reference.ps1 可以复现它们以供研究。

#pragma once

// --- 标准库 --------------------------------------------------------------------
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

// --- 可选的诊断设施（按可用性启用，缺失时自动降级） ------------------------------
//
// 两者都是标准库的一部分，不是平台 API：
//   * <source_location>：C++20 起提供 std::source_location；
//   * <stacktrace>：C++23 起提供 std::stacktrace。
// 早于它们的标准里，源码位置退回到 MSVC/GCC/Clang 都支持的
// __builtin_FILE()/__builtin_LINE()；再没有就退化为「无位置信息」。
// 也就是说：任何编译器都能构建，位置信息则按能力逐级降级。
#if defined(__has_include)
#if __has_include(<version>)
#include <version>  // 提供 __cpp_lib_* 特性宏
#endif

#if __has_include(<source_location>) && defined(__cpp_lib_source_location)
#include <source_location>
#define CODEC_HAS_SOURCE_LOCATION 1
#endif

#if __has_include(<stacktrace>) && defined(__cpp_lib_stacktrace)
#include <stacktrace>
#define CODEC_HAS_STACKTRACE 1
#endif
#endif  // __has_include

// 捕获 codec 构造位置的三种途径，按可用性从优到劣；都不满足时
// SourceLocation::current() 返回「无位置」，构建与行为不受影响。
#if !defined(CODEC_HAS_SOURCE_LOCATION) && \
    (defined(_MSC_VER) || defined(__GNUC__) || defined(__clang__))
#define CODEC_HAS_BUILTIN_FILE 1
#endif

// 想让 report() 附带真正的 C++ 调用栈（而不是 codec 调用链），
// 在编译时定义 CODEC_RECORD_STACKTRACE，并且要用 C++23 或更新标准
// （需要 <stacktrace>）。代价：每次产生错误都会抓一次栈，
// 因此对「存在但非法的可选字段」这种会被吞掉的错误也会付费，
// 所以默认关闭。
#if defined(CODEC_HAS_STACKTRACE) && defined(CODEC_RECORD_STACKTRACE)
#define CODEC_CAPTURES_STACKTRACE 1
#endif

// --- 第三方 --------------------------------------------------------------------
#include <nlohmann/json.hpp>

// ===========================================================================
// 1/8  json.hpp -- JsonValue（基于 nlohmann）+ Number（java.lang.Number）
// ===========================================================================

// Mojang DataFixerUpper `com.mojang.serialization` Codec API 的 C++17 移植。
//
// json.hpp —— 用作序列化形式的动态值类型（DFU 的 DynamicOps<T> 中的 "T"）
// 加上对应 java.lang.Number 的数值包装类型。
//
// 在 DataFixerUpper 中，序列化值类型是 Gson 的 JsonElement（见
// JsonOps）。本移植改为存储 nlohmann/json 文档
// （<https://github.com/nlohmann/json>），具体是 nlohmann::ordered_json，
// 从而让对象与 Gson 基于 LinkedTreeMap 的 JsonObject 一样保持插入顺序，
// 并且对成员赋值是原地替换。
//
// `JsonValue` 是 nlohmann::ordered_json 之上的一层薄封装，具有引用语义：
//   * codec 层只与 DynamicOps/JsonValue 打交道，从不接触 nlohmann，
//   * 应用可以直接传入 nlohmann 文档（JsonValue 有来自
//     nlohmann::ordered_json 的隐式构造函数），再用 raw() 取回，
//     因此解析器与序列化器都是 nlohmann 的。



namespace codec {

// ---------------------------------------------------------------------------
// Number —— 与 java.lang.Number 对应，用于 DynamicOps.getNumberValue()、
// DynamicOps.createNumeric() 以及各基础类型 codec（`Number::intValue()`）。
// ---------------------------------------------------------------------------
class Number {
 public:
  Number() = default;

  static Number ofInt(int64_t value) {
    Number n;
    n.integral_ = true;
    n.i_ = value;
    return n;
  }
  static Number ofDouble(double value) {
    Number n;
    n.integral_ = false;
    n.d_ = value;
    return n;
  }

  bool isIntegral() const { return integral_; }

  int64_t longValue() const {
    if (integral_) {
      return i_;
    }
    if (std::isnan(d_)) {
      return 0;
    }
    constexpr double kMax = 9223372036854775807.0;   // 2^63 - 1
    constexpr double kMin = -9223372036854775808.0;  // -2^63
    if (d_ >= kMax) {
      return std::numeric_limits<int64_t>::max();
    }
    if (d_ <= kMin) {
      return std::numeric_limits<int64_t>::min();
    }
    return static_cast<int64_t>(d_);
  }
  // Java 语义：对（已经截断过的）long 值做窄化截断。
  int32_t intValue() const { return static_cast<int32_t>(longValue()); }
  int16_t shortValue() const { return static_cast<int16_t>(longValue()); }
  int8_t byteValue() const { return static_cast<int8_t>(longValue()); }
  float floatValue() const { return integral_ ? static_cast<float>(i_) : static_cast<float>(d_); }
  double doubleValue() const { return integral_ ? static_cast<double>(i_) : d_; }
  bool booleanValue() const { return byteValue() != 0; }

  std::string toString() const;

  // Gson 的 JsonPrimitive 数值相等性：integral/integral 时比较 long
  // 值，否则比较 double。
  bool equals(const Number& other) const {
    if (integral_ && other.integral_) {
      return i_ == other.i_;
    }
    if (integral_ == other.integral_) {
      return d_ == other.d_ || (std::isnan(d_) && std::isnan(other.d_));
    }
    const double a = doubleValue();
    const double b = other.doubleValue();
    return a == b || (std::isnan(a) && std::isnan(b));
  }
  bool operator==(const Number& other) const { return equals(other); }
  bool operator!=(const Number& other) const { return !equals(other); }

 private:
  bool integral_ = true;
  int64_t i_ = 0;
  double d_ = 0.0;
};

namespace detail {

// 能往返的最短十进制表示，且不依赖
// std::to_chars(double)（C++17 并不要求它）。
inline std::string doubleToString(double value) {
  if (std::isnan(value)) {
    return "NaN";
  }
  if (std::isinf(value)) {
    return value > 0 ? "Infinity" : "-Infinity";
  }
  char buffer[64];
  for (int precision = 15; precision <= 17; ++precision) {
    std::snprintf(buffer, sizeof(buffer), "%.*g", precision, value);
    if (std::strtod(buffer, nullptr) == value) {
      break;
    }
  }
  std::string text(buffer);
  // 让该值仍能辨认出是浮点字面量（Gson 会输出 1.0）。
  if (text.find_first_of(".eE") == std::string::npos && text.find_first_of("nN") == std::string::npos) {
    text += ".0";
  }
  return text;
}

}  // namespace detail

inline std::string Number::toString() const {
  return integral_ ? std::to_string(i_) : detail::doubleToString(d_);
}

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
  if (node_->is_number_integer()) {
    return Number::ofInt(node_->get<int64_t>());
  }
  if (node_->is_number_unsigned()) {
    const uint64_t unsignedValue = node_->get<uint64_t>();
    if (unsignedValue <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      return Number::ofInt(static_cast<int64_t>(unsignedValue));
    }
    return Number::ofDouble(static_cast<double>(unsignedValue));
  }
  return Number::ofDouble(node_->get<double>());
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
  const Type kind = type();
  if (kind != other.type()) {
    return false;
  }
  switch (kind) {
    case Type::Null:
      return true;
    case Type::Boolean:
      return asBoolean() == other.asBoolean();
    case Type::Number:
      return asNumber().equals(other.asNumber());
    case Type::String:
      return asString() == other.asString();
    case Type::Array: {
      if (size() != other.size()) {
        return false;
      }
      const Array mine = asArray();
      const Array theirs = other.asArray();
      for (size_t i = 0; i < mine.size(); ++i) {
        if (!mine[i].equals(theirs[i])) {
          return false;
        }
      }
      return true;
    }
    case Type::Object: {
      // 不区分顺序，与 Gson 的 JsonObject（LinkedTreeMap）相等性一致。
      if (size() != other.size()) {
        return false;
      }
      for (auto it = node_->begin(); it != node_->end(); ++it) {
        const auto theirs = other.node_->find(it.key());
        if (theirs == other.node_->end()) {
          return false;
        }
        if (!JsonValue(owner_, &it.value()).equals(JsonValue(other.owner_, &theirs.value()))) {
          return false;
        }
      }
      return true;
    }
  }
  return false;
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

}  // namespace codec


// ===========================================================================
// 2/8  lifecycle.hpp -- Lifecycle
// ===========================================================================

// Mojang DataFixerUpper `com.mojang.serialization` Codec API 的 C++17 移植。
//
// lifecycle.hpp -- com.mojang.serialization.Lifecycle 的移植。


namespace codec {

// Lifecycle 记录一条（反）序列化路径是否触及了任何
// 不保证永远稳定的东西。它精确对应 DFU 的 Lifecycle，
// 包括「experimental 胜出」的组合规则，以及
// 「`since` 最小的 deprecated 值胜出」规则。
class Lifecycle {
 public:
  enum class Kind { Stable, Experimental, Deprecated };

  Lifecycle() = default;

  static Lifecycle stable() { return Lifecycle(Kind::Stable, 0); }
  static Lifecycle experimental() { return Lifecycle(Kind::Experimental, 0); }
  static Lifecycle deprecated(int since) { return Lifecycle(Kind::Deprecated, since); }

  Kind kind() const { return kind_; }
  bool isStable() const { return kind_ == Kind::Stable; }
  bool isExperimental() const { return kind_ == Kind::Experimental; }
  bool isDeprecated() const { return kind_ == Kind::Deprecated; }
  int since() const { return since_; }

  // Lifecycle.add(other)
  Lifecycle add(const Lifecycle& other) const {
    if (kind_ == Kind::Experimental || other.kind_ == Kind::Experimental) {
      return experimental();
    }
    if (kind_ == Kind::Deprecated) {
      if (other.kind_ == Kind::Deprecated && other.since_ < since_) {
        return other;
      }
      return *this;
    }
    if (other.kind_ == Kind::Deprecated) {
      return other;
    }
    return stable();
  }

  std::string toString() const {
    switch (kind_) {
      case Kind::Stable:
        return "Stable";
      case Kind::Experimental:
        return "Experimental";
      case Kind::Deprecated:
        return "Deprecated[" + std::to_string(since_) + "]";
    }
    return "Unknown";
  }

  bool operator==(const Lifecycle& other) const {
    return kind_ == other.kind_ && (kind_ != Kind::Deprecated || since_ == other.since_);
  }
  bool operator!=(const Lifecycle& other) const { return !(*this == other); }

 private:
  Lifecycle(Kind kind, int since) : kind_(kind), since_(since) {}

  Kind kind_ = Kind::Experimental;
  int since_ = 0;
};

}  // namespace codec


// ===========================================================================
// 3/8  data_result.hpp -- DataResult、PartialResult 语义、Unit
// ===========================================================================

// Mojang DataFixerUpper `com.mojang.serialization` Codec API 的 C++17 移植。
//
// data_result.hpp -- com.mojang.serialization.DataResult 的移植。
//
// DFU 把结果建模为 Either<R, PartialResult<R>>：成功时携带一个值，
// 失败时携带一条消息，外加（可选地）一个部分解码的值。
// C++ 移植保持完全相同的形状：
//
//   value_   engaged  <=> Either.left（成功）或 PartialResult.partialResult
//   error_   engaged  <=> Either.right（失败）
//
// 因此 `result()` 就是 Java 的 `result()`（仅成功值），`error()` 就是 Java 的
// `error()`（消息），而 `resultOrPartial()` / `getOrThrow(allowPartial, ...)`
// 返回失败解码的部分值。



namespace codec {

// Java 的 Consumer<String> / UnaryOperator<String>。
using ErrorHandler = std::function<void(const std::string&)>;
using StringUnaryOperator = std::function<std::string(const std::string&)>;

template <class R>
class DataResult;

namespace detail {

// apply2/apply2stable/apply3 与 record codec builder 共用的 applicative 组合；
// 定义在 DataResult 之后。
template <class F, class... Ts>
auto combineAll(const Lifecycle& base, F function, const DataResult<Ts>&... results)
    -> DataResult<std::invoke_result_t<F, const Ts&...>>;

}  // namespace detail

// ---------------------------------------------------------------------------
// 错误路径 -- DFU 中没有对应实现的新增
//
// DFU 的错误消息不携带位置信息：发生在
// `{"risks":[...,{"condition":{"or":[{"op":1}]}}]}` 内部的失败只会给出
// `Not a string: 1`。本移植把位置与消息一并记录下来，因此
//
//     result.message()    -> "Not a string: 1"                        （DFU 文本）
//     result.location()   -> "risks[3].condition.or[0].op"
//     result.describe()   -> "risks[3].condition.or[0].op: Not a string: 1"
//
// 因此 `message()` 与 DFU 保持逐字节一致，而 `describe()` 是
// 展示给用户的诊断形式。段由容器在错误向外传播时追加
// （fieldOf 加上键，ListCodec 加上 [i]，record 构建器在编码时
// 加上字段名），因此 `path` 按叶子优先存储、
// 按根优先渲染。
struct PathSegment {
  bool isIndex = false;
  int32_t position = 0;
  std::string key;

  static PathSegment field(std::string name) {
    PathSegment segment;
    segment.key = std::move(name);
    return segment;
  }
  static PathSegment element(int32_t position) {
    PathSegment segment;
    segment.isIndex = true;
    segment.position = position;
    return segment;
  }

  std::string render() const {
    return isIndex ? "[" + std::to_string(position) + "]" : key;
  }
  bool operator==(const PathSegment& other) const {
    return isIndex == other.isIndex && position == other.position && key == other.key;
  }
  bool operator!=(const PathSegment& other) const { return !(*this == other); }
};

// 源码位置：编译器在调用点生成的编译期字面量。它不分配内存、不依赖
// 调试信息（PDB/DWARF），因此在 Release 构建里同样可用——这正是它比
// 栈回溯更适合给 codec 调用链做标注的原因。
//
// 三级来源，按可用性自动选择：标准库 std::source_location（C++20）→
// MSVC/GCC/Clang 通用的 __builtin_FILE()/__builtin_LINE() → 无位置。
struct SourceLocation {
  const char* file = nullptr;
  std::uint_least32_t line = 0;

  constexpr bool valid() const noexcept { return file != nullptr; }
  constexpr explicit operator bool() const noexcept { return valid(); }

#if defined(CODEC_HAS_SOURCE_LOCATION)
  // 默认实参在*调用点*求值，因此这里拿到的正是构造该 codec 的那一行。
  static SourceLocation current(
      const std::source_location& location = std::source_location::current()) {
    return SourceLocation{location.file_name(),
                          static_cast<std::uint_least32_t>(location.line())};
  }
#elif defined(CODEC_HAS_BUILTIN_FILE)
  static constexpr SourceLocation current(const char* file = __builtin_FILE(),
                                          std::uint_least32_t line = __builtin_LINE()) {
    return SourceLocation{file, line};
  }
#else
  static constexpr SourceLocation current() { return SourceLocation{}; }
#endif

  // 两种可点击写法：GCC/Clang 诊断风格 `file:line`，MSVC 诊断风格
  // `file(line)`（后者也正是 MSVC 自家 <stacktrace> 的输出格式）。
  std::string renderGnu() const {
    return valid() ? std::string(file) + ":" + std::to_string(line) : std::string();
  }
  std::string renderMsvc() const {
    return valid() ? std::string(file) + "(" + std::to_string(line) + ")" : std::string();
  }
};

// report() 里栈帧的排版风格。native 表示按当前编译器选：MSVC 工具链用
// msvc（与它自家的诊断、<stacktrace> 输出一致），其余用 gnu（gdb/lldb
// 风格）。两种写法都带「文件:行号」，CLion 等 IDE 都能点开。
enum class FrameStyle { native, msvc, gnu };

#if defined(_MSC_VER)
inline constexpr FrameStyle kNativeFrameStyle = FrameStyle::msvc;
#else
inline constexpr FrameStyle kNativeFrameStyle = FrameStyle::gnu;
#endif

// codec 调用链上的一帧：codec 自己的短名，加上构造它的位置
// （位置未知时只打印名字）。
struct Frame {
  std::string codec;
  SourceLocation where;

  bool operator==(const Frame& other) const {
    if (codec != other.codec || where.line != other.where.line) {
      return false;
    }
    return std::string_view(where.file ? where.file : "") ==
           std::string_view(other.where.file ? other.where.file : "");
  }
  bool operator!=(const Frame& other) const { return !(*this == other); }
};

// 一次失败：它在何处被发现、DFU 为它给出的消息，以及处理过它的 codec
// 调用链（叶子优先，类似栈回溯）。在多个位置失败的结果
// 会持有多个部分，`message()` 正是用「; 」把它们连接起来，
// 与 DFU 完全一致。
struct ErrorPart {
  std::vector<PathSegment> path;  // 叶子优先
  std::vector<Frame> frames;      // 叶子优先：失败的 codec，然后是它的调用方
  std::string message;
#if defined(CODEC_CAPTURES_STACKTRACE)
  // 真正的 C++ 调用栈（标准库 <stacktrace>），只在定义了
  // CODEC_RECORD_STACKTRACE 时记录。
  std::stacktrace trace;
#endif
};

inline std::string renderPath(const std::vector<PathSegment>& path) {
  std::string out;
  for (auto it = path.rbegin(); it != path.rend(); ++it) {
    if (it->isIndex) {
      out += it->render();
    } else {
      if (!out.empty()) {
        out += ".";
      }
      out += it->key;
    }
  }
  return out;
}

// 对 DataResult<?> 的类型擦除视图，供 RecordBuilder/ListBuilder 的
// `withErrorsFrom` 使用（在 DFU 中它们接受 DataResult<?>）。
class DataResultBase {
 public:
  virtual ~DataResultBase() = default;

  bool isSuccess() const { return errors_.empty(); }
  bool isError() const { return !errors_.empty(); }
  // 有值可用时为 true：要么是成功值，要么是失败的
  // 部分值。
  bool hasValue() const { return valuePresent(); }

  // DFU 的消息：把各局部消息用「; 」连接，不含任何位置信息。
  std::string message() const {
    if (errors_.empty()) {
      throw std::logic_error("DataResult::message() called on a successful result");
    }
    std::string out;
    for (const ErrorPart& part : errors_) {
      if (!out.empty()) {
        out += "; ";
      }
      out += part.message;
    }
    return out;
  }

  // 失败的 JSON 路径，例如 `risks[3].condition.or[0].op`。当失败
  // 没有位置、或各部分失败的位置不同时为空。
  std::string location() const {
    if (errors_.empty() || errors_.front().path.empty()) {
      return {};
    }
    for (const ErrorPart& part : errors_) {
      if (part.path != errors_.front().path) {
        return {};
      }
    }
    return renderPath(errors_.front().path);
  }

  // 每个部分为 `location: message`，用「; 」连接 -- 适合在单行上报的
  // 形式（诊断回调接收的就是这个）。
  std::string describe() const {
    std::string out;
    for (const ErrorPart& part : errors_) {
      if (!out.empty()) {
        out += "; ";
      }
      const std::string path = renderPath(part.path);
      if (!path.empty()) {
        out += path;
        out += ": ";
      }
      out += part.message;
    }
    return out;
  }

  // 完整的多行诊断：每一次失败及其位置、产生它的 codec 调用链，
  // 以及（启用 CODEC_RECORD_STACKTRACE 时）标准库抓取的 C++ 调用栈。
  //
  // 栈帧按「IDE 能识别成可点击链接」的形式渲染，默认跟随编译器：
  //
  //   msvc（MSVC 工具链，与它自家的诊断、<stacktrace> 输出同形）
  //     risks[3].condition.or[0].op: Not a string: 1
  //       0> D:\...\codec.hpp(3295): String
  //       1> D:\...\risk_def.hpp(88): optional[op]
  //
  //   gnu（gdb/lldb 风格）
  //     risks[3].condition.or[0].op: Not a string: 1
  //       #0 String at D:\...\codec.hpp:3295
  //       #1 optional[op] at D:\...\risk_def.hpp:88
  //
  // 两种写法都含「文件:行号」，因此 CLion 的「Analyze Stack Trace」
  // 与控制台超链接都能直接跳到构造该 codec 的那一行；
  // 位置未知时只打印 codec 名字。
  std::string report(FrameStyle style = FrameStyle::native) const {
    if (style == FrameStyle::native) {
      style = kNativeFrameStyle;
    }
    std::string out;
    for (size_t i = 0; i < errors_.size(); ++i) {
      const ErrorPart& part = errors_[i];
      if (i != 0) {
        out += "\n";
      }
      const std::string path = renderPath(part.path);
      if (!path.empty()) {
        out += path;
        out += ": ";
      }
      out += part.message;

      int index = 0;
      for (const Frame& frame : part.frames) {
        const std::string number = std::to_string(index);
        if (style == FrameStyle::msvc) {
          const std::string where = frame.where.renderMsvc();
          out += "\n  " + number + "> ";
          if (!where.empty()) {
            out += where;
            out += ": ";
          }
          out += frame.codec;
        } else {
          out += "\n  #" + number + " " + frame.codec;
          const std::string where = frame.where.renderGnu();
          if (!where.empty()) {
            out += " at ";
            out += where;
          }
        }
        ++index;
      }

#if defined(CODEC_CAPTURES_STACKTRACE)
      // 标准库 <stacktrace> 自己的排版：MSVC 是 `0> file(line): module!func`，
      // libstdc++/libc++ 是 `   0# func at file:line`。原样缩进贴出来，
      // 让 IDE 用它们熟悉的格式解析。
      if (part.trace.size() != 0) {
        out += "\n  stacktrace:";
        const std::string trace = std::to_string(part.trace);
        size_t start = 0;
        while (start < trace.size()) {
          const size_t end = trace.find('\n', start);
          out += "\n    ";
          out += trace.substr(start, end == std::string::npos ? std::string::npos : end - start);
          if (end == std::string::npos) {
            break;
          }
          start = end + 1;
        }
      }
#endif
    }
    return out;
  }

  // 只要 codec 名字的调用链（不含位置与消息），便于程序化比较。
  std::vector<std::string> frameNames() const {
    std::vector<std::string> names;
    for (const ErrorPart& part : errors_) {
      for (const Frame& frame : part.frames) {
        names.push_back(frame.codec);
      }
    }
    return names;
  }

  const std::vector<ErrorPart>& errors() const { return errors_; }
  const Lifecycle& lifecycle() const { return lifecycle_; }

 protected:
  virtual bool valuePresent() const = 0;

  // 不在前面加任何东西：容器用 DataResult::addPath 追加自己的段。
  std::vector<ErrorPart> errors_;
  Lifecycle lifecycle_;
};

// 把失败变成异常——错误传播的最跨平台方式：只用标准库
// （<stdexcept>），任何编译器、任何标准版本都一样；而且不需要
// 源码位置或调试信息就能工作。
//
// 在 CLion 等 IDE 里对 CodecError 下断点，即可在抛出点停下并看到
// 真正的调用栈；异常对象本身携带位置与 codec 调用链，所以即使不调试
// 也能知道是哪个字段、哪个 codec 出的错。
//
// 它派生自 std::runtime_error，因此既有的
// catch (const std::exception&) / catch (const std::runtime_error&)
// 继续有效。what() 是单行的「位置: 消息」，report() 给出带 codec
// 调用链的完整诊断。
class CodecError : public std::runtime_error {
 public:
  explicit CodecError(const DataResultBase& result)
      : std::runtime_error(result.describe()),
        report_(result.report()),
        location_(result.location()),
        errors_(result.errors()),
        hasPartial_(result.hasValue()) {}

  // 带位置与 codec 调用链的多行诊断（与 DataResult::report() 相同）。
  const std::string& report() const noexcept { return report_; }
  // 失败位置；多处失败且位置不一致时为空。
  const std::string& location() const noexcept { return location_; }
  // 每个失败部分：路径 + codec 帧 + 消息。
  const std::vector<ErrorPart>& errors() const noexcept { return errors_; }
  // 异常里是否还带着可用的部分值（DFU 的 partial result）。
  bool hasPartial() const noexcept { return hasPartial_; }

 private:
  std::string report_;
  std::string location_;
  std::vector<ErrorPart> errors_;
  bool hasPartial_ = false;
};

template <class R>
class DataResult : public DataResultBase {
 public:
  using value_type = R;

  DataResult() = default;

  // --- 工厂函数 (DataResult.success / DataResult.error) -------------------
  static DataResult success(R value, Lifecycle lifecycle = Lifecycle::experimental()) {
    DataResult result;
    result.value_ = std::move(value);
    result.lifecycle_ = lifecycle;
    return result;
  }

  // 构造一个失败部分；启用 CODEC_RECORD_STACKTRACE（且标准库提供
  // <stacktrace>）时同时抓取当前的 C++ 调用栈。捕获点就在错误产生的
  // 地方，因此栈里既有 codec 内部的调用帧，也有用户调用 parse() 的
  // 那一行——这是唯一能拿到「用户那一帧」的时机。
  static ErrorPart makeErrorPart(std::string message) {
    ErrorPart part;
    part.message = std::move(message);
#if defined(CODEC_CAPTURES_STACKTRACE)
    // 限制深度：错误路径上不值得抓满整条栈。
    part.trace = std::stacktrace::current(0, 32);
#endif
    return part;
  }

  static DataResult error(std::string message) {
    return errorNoPartial(std::move(message), Lifecycle::experimental());
  }

  static DataResult error(std::string message, R partial,
                          Lifecycle lifecycle = Lifecycle::experimental()) {
    DataResult result;
    result.value_ = std::move(partial);
    result.errors_.push_back(makeErrorPart(std::move(message)));
    result.lifecycle_ = lifecycle;
    return result;
  }

  static DataResult errorNoPartial(std::string message,
                                   Lifecycle lifecycle = Lifecycle::experimental()) {
    DataResult result;
    result.errors_.push_back(makeErrorPart(std::move(message)));
    result.lifecycle_ = lifecycle;
    return result;
  }

  static DataResult errorPartial(std::string message, std::optional<R> partial,
                                 Lifecycle lifecycle = Lifecycle::experimental()) {
    DataResult result;
    result.value_ = std::move(partial);
    result.errors_.push_back(makeErrorPart(std::move(message)));
    result.lifecycle_ = lifecycle;
    return result;
  }

  // 携带已经构建好的部分的失败（供 record codec 构建器使用，其
  // 各字段分别上报自己的位置与帧）。
  static DataResult errorParts(std::vector<ErrorPart> errors, std::optional<R> partial,
                               Lifecycle lifecycle) {
    DataResult result;
    result.value_ = std::move(partial);
    result.errors_ = std::move(errors);
    result.lifecycle_ = lifecycle;
    return result;
  }

  // 给每个部分附加一个位置段。容器在错误向外传播时调用它，
  // 因此最内层的失败最终带有完整路径。
  //
  // string_view 与下标重载把 PathSegment 的构造放在错误分支*内部*，
  // 从而让成功的解码（热路径）不产生分配：
  // 只要没有真正失败，就不会复制任何键。
  DataResult addPath(PathSegment segment) const& {
    if (isSuccess()) {
      return *this;
    }
    DataResult out = *this;
    for (ErrorPart& part : out.errors_) {
      part.path.push_back(segment);
    }
    return out;
  }
  DataResult addPath(PathSegment segment) && {
    if (isError()) {
      for (ErrorPart& part : errors_) {
        part.path.push_back(segment);
      }
    }
    return std::move(*this);
  }
  DataResult addPath(std::string_view key) const& {
    return isSuccess() ? *this : addPath(PathSegment::field(std::string(key)));
  }
  DataResult addPath(std::string_view key) && {
    if (isError()) {
      for (ErrorPart& part : errors_) {
        part.path.push_back(PathSegment::field(std::string(key)));
      }
    }
    return std::move(*this);
  }
  DataResult addPath(int32_t index) const& {
    return isSuccess() ? *this : addPath(PathSegment::element(index));
  }
  DataResult addPath(int32_t index) && {
    if (isError()) {
      for (ErrorPart& part : errors_) {
        part.path.push_back(PathSegment::element(index));
      }
    }
    return std::move(*this);
  }

  // 给每个部分附加一个 codec 帧。每个 codec 工厂都在自己的失败路径上
  // 调用它，因此各部分最终携带处理过该值的 codec 调用链
  // -- 解码的「栈」，最内层在前。与 addPath 一样，
  // 只有在出现错误时才会真正构造帧对象。
  //
  // `where` 是构造该 codec 的位置（工厂函数的默认实参在调用点取值），
  // 于是 report() 里每一帧都能点回源码；跨平台由 SourceLocation 负责。
  DataResult addFrame(std::string_view frame, SourceLocation where = {}) const& {
    if (isSuccess()) {
      return *this;
    }
    DataResult out = *this;
    for (ErrorPart& part : out.errors_) {
      part.frames.push_back(Frame{std::string(frame), where});
    }
    return out;
  }
  DataResult addFrame(std::string_view frame, SourceLocation where = {}) && {
    if (isError()) {
      for (ErrorPart& part : errors_) {
        part.frames.push_back(Frame{std::string(frame), where});
      }
    }
    return std::move(*this);
  }

  // --- 访问器 ----------------------------------------------------------
  // Java 的 result()：成功结果的值，否则为空。
  const std::optional<R>& result() const { return isError() ? kNoValue : value_; }
  // 结果成功、或失败但带有部分值时存储的值。
  const std::optional<R>& valueOrPartial() const { return value_; }
  bool hasPartial() const { return isError() && value_.has_value(); }

  const R& value() const {
    if (!value_) {
      throw std::logic_error("DataResult::value() called on a result without a value");
    }
    return *value_;
  }

  R getOrThrow(bool allowPartial, const ErrorHandler& onError) const {
    if (isError()) {
      const std::string text = describe();
      onError(text);
      if (allowPartial && value_) {
        return *value_;
      }
      // DFU 抛 RuntimeException；这里抛 CodecError（同样是
      // std::runtime_error 的派生类，但额外带了位置与 codec 调用链）。
      throw CodecError(*this);
    }
    return *value_;
  }

  // 失败即抛 CodecError（新增）：不关心部分值时的最简入口，
  // 也是「让调试器显示栈」的跨平台做法。
  R throwIfError() const {
    if (isError()) {
      throw CodecError(*this);
    }
    return *value_;
  }

  std::optional<R> resultOrPartial(const ErrorHandler& onError) const {
    if (isError()) {
      onError(describe());
    }
    return value_;
  }

  // --- 变换 ----------------------------------------------------
  template <class F>
  using MapValue = std::invoke_result_t<F, const R&>;

  template <class F>
  DataResult<MapValue<F>> map(F function) const {
    using S = MapValue<F>;
    DataResult<S> out;
    out.lifecycle_ = lifecycle_;
    out.errors_ = errors_;
    if (value_) {
      out.value_ = std::invoke(function, *value_);
    }
    return out;
  }

  template <class F>
  using FlatMapValue = typename std::invoke_result_t<F, const R&>::value_type;

  template <class F>
  DataResult<FlatMapValue<F>> flatMap(F function) const {
    using S = FlatMapValue<F>;
    if (!isError()) {
      DataResult<S> second = std::invoke(function, *value_);
      second.lifecycle_ = lifecycle_.add(second.lifecycle_);
      return second;
    }
    if (!value_) {
      // 失败且没有部分值：没有值可以喂给这个函数。
      DataResult<S> out;
      out.errors_ = errors_;
      out.lifecycle_ = lifecycle_;
      return out;
    }
    DataResult<S> second = std::invoke(function, *value_);
    DataResult<S> out;
    out.lifecycle_ = lifecycle_.add(second.lifecycle_);
    out.value_ = second.value_;
    out.errors_ = errors_;
    if (second.isError()) {
      out.errors_.insert(out.errors_.end(), second.errors_.begin(), second.errors_.end());
    }
    return out;
  }

  // Java 的 DataResult.apply2 / apply2stable / apply3（DataResult 上的
  // Applicative 实例）。Lifecycle 基值是被指向函数的 lifecycle；
  // DFU 为 apply2/apply3 使用 experimental 点，为 apply2stable 使用
  // stable 点。
  template <class F, class R2>
  DataResult<std::invoke_result_t<F, const R&, const R2&>> apply2(
      F function, const DataResult<R2>& second) const {
    return detail::combineAll(Lifecycle::experimental(), std::move(function), *this, second);
  }

  template <class F, class R2>
  DataResult<std::invoke_result_t<F, const R&, const R2&>> apply2stable(
      F function, const DataResult<R2>& second) const {
    return detail::combineAll(Lifecycle::stable(), std::move(function), *this, second);
  }

  template <class F, class R2, class R3>
  DataResult<std::invoke_result_t<F, const R&, const R2&, const R3&>> apply3(
      F function, const DataResult<R2>& second, const DataResult<R3>& third) const {
    return detail::combineAll(Lifecycle::experimental(), std::move(function), *this, second, third);
  }

  // 仅对失败的结果有意义（DFU 的 setPartial）。
  DataResult setPartial(R partial) const {
    DataResult out = *this;
    if (out.isError()) {
      out.value_ = std::move(partial);
    }
    return out;
  }

  DataResult setPartialFrom(std::function<R()> supplier) const {
    DataResult out = *this;
    if (out.isError()) {
      out.value_ = supplier();
    }
    return out;
  }

  DataResult mapError(const StringUnaryOperator& function) const {
    DataResult out = *this;
    if (out.isError()) {
      // DFU 会把该操作符应用在整个消息上；位置与 codec 帧在
      // 每个部分都共享它们时会保留下来，否则会被丢弃
      // （否则会造成误导）。
      const std::vector<PathSegment> path = sharedPath();
      const std::vector<Frame> frames = sharedFrames();
      ErrorPart part{path, frames, function(message())};
#if defined(CODEC_CAPTURES_STACKTRACE)
      // 消息被重写了，但栈仍然是产生这次失败的那个栈。
      if (!out.errors_.empty()) {
        part.trace = out.errors_.front().trace;
      }
#endif
      out.errors_.clear();
      out.errors_.push_back(std::move(part));
    }
    return out;
  }

  DataResult setLifecycle(const Lifecycle& lifecycle) const {
    DataResult out = *this;
    out.lifecycle_ = lifecycle;
    return out;
  }

  DataResult addLifecycle(const Lifecycle& lifecycle) const {
    DataResult out = *this;
    out.lifecycle_ = out.lifecycle_.add(lifecycle);
    return out;
  }

  // 当存在部分值时，把部分失败转成成功。
  DataResult promotePartial(const ErrorHandler& onError) const {
    if (!isError()) {
      return *this;
    }
    onError(describe());
    if (value_) {
      return DataResult::success(*value_, lifecycle_);
    }
    return *this;
  }

  template <class>
  friend class DataResult;

 private:
  static const std::optional<R> kNoValue;

  // 每个部分共享的路径，它们不同时为空。
  std::vector<PathSegment> sharedPath() const {
    if (errors_.empty() || errors_.front().path.empty()) {
      return {};
    }
    for (const ErrorPart& part : errors_) {
      if (part.path != errors_.front().path) {
        return {};
      }
    }
    return errors_.front().path;
  }

  // 每个部分共享的 codec 帧，它们不同时为空。
  std::vector<Frame> sharedFrames() const {
    if (errors_.empty() || errors_.front().frames.empty()) {
      return {};
    }
    for (const ErrorPart& part : errors_) {
      if (part.frames != errors_.front().frames) {
        return {};
      }
    }
    return errors_.front().frames;
  }

  bool valuePresent() const override { return value_.has_value(); }

  std::optional<R> value_;
};

template <class R>
const std::optional<R> DataResult<R>::kNoValue{};

namespace detail {

// ---------------------------------------------------------------------------
// combineAll -- 把 DFU 的 Applicative.super.ap2 调用链推广到 N 个操作数：
//   * 所有操作数都成功 -> success(f(values...))
//   * 否则             -> 失败：按操作数顺序收集失败的部分
//                         （message() 把它们的文本用「; 」连接，
//                         describe() 给每个加上位置前缀），
//                         当每个操作数都有部分值时携带一个部分值
//   * lifecycle        -> base.add(op1.lifecycle).add(op2.lifecycle)...
// ---------------------------------------------------------------------------
template <class F, class... Ts>
auto combineAll(const Lifecycle& base, F function, const DataResult<Ts>&... results)
    -> DataResult<std::invoke_result_t<F, const Ts&...>> {
  using R = std::invoke_result_t<F, const Ts&...>;

  Lifecycle lifecycle = base;
  bool allSuccess = true;
  bool allValues = true;
  std::vector<ErrorPart> errors;

  const auto step = [&](const DataResultBase& result) {
    lifecycle = lifecycle.add(result.lifecycle());
    allSuccess = allSuccess && result.isSuccess();
    allValues = allValues && result.hasValue();
    if (result.isError()) {
      errors.insert(errors.end(), result.errors().begin(), result.errors().end());
    }
  };
  (void)std::initializer_list<int>{(step(results), 0)...};

  if (allSuccess) {
    return DataResult<R>::success(std::invoke(function, *results.valueOrPartial()...), lifecycle);
  }
  std::optional<R> partial;
  if (allValues) {
    partial = std::invoke(function, *results.valueOrPartial()...);
  }
  return DataResult<R>::errorParts(std::move(errors), std::move(partial), lifecycle);
}

// Java 的 AbstractBuilder.withErrorsFrom：`builder.flatMap(b -> result.map(r -> b))`。
template <class S>
DataResult<S> propagateErrors(const DataResult<S>& builder, const DataResultBase& result) {
  if (result.isSuccess()) {
    return builder;
  }
  std::vector<ErrorPart> errors;
  if (builder.isSuccess()) {
    errors = result.errors();
    return DataResult<S>::errorParts(std::move(errors), builder.value(),
                                     builder.lifecycle().add(result.lifecycle()));
  }
  if (builder.hasValue()) {
    errors = builder.errors();
    errors.insert(errors.end(), result.errors().begin(), result.errors().end());
    return DataResult<S>::errorParts(std::move(errors), builder.value(),
                                     builder.lifecycle().add(result.lifecycle()));
  }
  return builder;
}

}  // namespace detail

// `Unit` -- DFU 的 com.mojang.datafixers.util.Unit，供不携带任何信息的
// codec 使用的单元类型（Codec::EMPTY、list/map 累加）。
struct Unit {
  bool operator==(const Unit&) const { return true; }
  bool operator!=(const Unit&) const { return false; }
};

}  // namespace codec


// ===========================================================================
// 4/8  dynamic_ops.hpp -- DynamicOps、MapLike、RecordBuilder、ListBuilder、KeyCompressor
// ===========================================================================

// Mojang DataFixerUpper `com.mojang.serialization` Codec API 的 C++17 移植。
//
// dynamic_ops.hpp -- DynamicOps、MapLike、RecordBuilder、ListBuilder、
// KeyCompressor 与 Compressable 的移植。
//
// DFU 的 DynamicOps<T> 对序列化后的值类型（JsonElement、
// NbtTag……）是泛型的。本移植只用一个统一的值类型（JsonValue），因此
// 类型参数被消去，DynamicOps 变成面向 JsonValue 的抽象类。
// JsonOps 是唯一随库提供的具体实现；由于值类型是统一的，
// 这里的 DynamicOps::convertTo 是恒等变换，而在
// DFU 中它会把 DOM 重写成目标 ops 的表示。



namespace codec {

class DynamicOps;
class MapLike;
class KeyCompressor;

using MapLikePtr = std::shared_ptr<const MapLike>;

// Gson 的 JsonObject.add：已存在的成员会被原地替换（该对象
// 由 LinkedTreeMap 支撑），这正是 DFU 的 JsonOps 所依赖的行为。
inline void putJsonMember(JsonValue::Object& members, const std::string& key, const JsonValue& value) {
  for (auto& member : members) {
    if (member.first == key) {
      member.second = value;
      return;
    }
  }
  members.emplace_back(key, value);
}

// ---------------------------------------------------------------------------
// MapLike<T> -- 对象之上的只读视图。
//
// 忠实复刻自 JsonOps 的怪癖：`get` 把显式的 JSON null 视为「缺失」，
// 而 `entries` 也会产出 null 成员。
// ---------------------------------------------------------------------------
class MapLike {
 public:
  virtual ~MapLike() = default;

  virtual std::optional<JsonValue> get(const JsonValue& key) const = 0;
  virtual std::optional<JsonValue> get(const std::string& key) const = 0;
  virtual std::vector<std::pair<JsonValue, JsonValue>> entries() const = 0;
  virtual std::string toString() const = 0;
};

// 由 JsonValue 对象支撑的 MapLike（JsonOps.getMap）。
class JsonObjectMapLike : public MapLike {
 public:
  explicit JsonObjectMapLike(JsonValue object) : object_(std::move(object)) {}

  std::optional<JsonValue> get(const JsonValue& key) const override {
    if (!key.isString()) {
      return std::nullopt;
    }
    return get(key.asString());
  }

  std::optional<JsonValue> get(const std::string& key) const override {
    // JsonValue::get 已经把显式的 JSON null 视为「缺失」。
    return object_.get(key);
  }

  std::vector<std::pair<JsonValue, JsonValue>> entries() const override {
    std::vector<std::pair<JsonValue, JsonValue>> out;
    if (!object_.isObject()) {
      return out;
    }
    out.reserve(object_.size());
    for (const auto& entry : object_.asObject()) {
      out.emplace_back(JsonValue::string(entry.first), entry.second);
    }
    return out;
  }

  const JsonValue& object() const { return object_; }

  std::string toString() const override { return "MapLike[" + object_.dump() + "]"; }

 private:
  JsonValue object_;
};

// ---------------------------------------------------------------------------
// KeyCompressor<T> -- 把键映射为稠密下标（在 compressMaps() 时使用）。
//
// DFU 中由 fastutil 支撑的映射对未知键返回 0；本移植返回 -1，
// 调用方把它视为「缺失」，而不是默默读取下标 0。
// ---------------------------------------------------------------------------
class KeyCompressor {
 public:
  KeyCompressor(const DynamicOps& ops, const std::vector<JsonValue>& keys);

  const JsonValue* decompress(int key) const {
    if (key < 0 || static_cast<size_t>(key) >= decompress_.size()) {
      return nullptr;
    }
    return &decompress_[static_cast<size_t>(key)];
  }

  int compress(const std::string& key) const;
  int compress(const JsonValue& key) const;

  int size() const { return static_cast<int>(decompress_.size()); }

 private:
  const DynamicOps* ops_;
  std::vector<JsonValue> decompress_;
  std::unordered_map<std::string, int> compressByValue_;
  std::unordered_map<std::string, int> compressByString_;
};

// MapDecoder::compressedDecode 使用的 MapLike：压缩列表按键
// 下标寻址（DFU 中 MapDecoder.compressedDecode 里的匿名 MapLike）。
class CompressedMapLike : public MapLike {
 public:
  CompressedMapLike(const KeyCompressor& compressor, std::vector<JsonValue> entries)
      : compressor_(&compressor), entries_(std::move(entries)) {}

  std::optional<JsonValue> get(const JsonValue& key) const override {
    const int index = compressor_->compress(key);
    return at(index);
  }

  std::optional<JsonValue> get(const std::string& key) const override {
    const int index = compressor_->compress(key);
    return at(index);
  }

  std::vector<std::pair<JsonValue, JsonValue>> entries() const override {
    std::vector<std::pair<JsonValue, JsonValue>> out;
    for (size_t i = 0; i < entries_.size(); ++i) {
      const JsonValue* key = compressor_->decompress(static_cast<int>(i));
      if (key == nullptr || entries_[i].isNull()) {
        continue;
      }
      out.emplace_back(*key, entries_[i]);
    }
    return out;
  }

  std::string toString() const override {
    std::string out = "MapLike[";
    bool first = true;
    for (const auto& entry : entries()) {
      if (!first) {
        out += ", ";
      }
      first = false;
      out += entry.first.dump() + "=" + entry.second.dump();
    }
    return out + "]";
  }

 private:
  std::optional<JsonValue> at(int index) const {
    if (index < 0 || static_cast<size_t>(index) >= entries_.size()) {
      return std::nullopt;
    }
    const JsonValue& value = entries_[static_cast<size_t>(index)];
    if (value.isNull()) {
      return std::nullopt;
    }
    return value;
  }

  const KeyCompressor* compressor_;
  std::vector<JsonValue> entries_;
};

// ---------------------------------------------------------------------------
// ListBuilder<T>
// ---------------------------------------------------------------------------
class ListBuilder {
 public:
  virtual ~ListBuilder() = default;

  virtual const DynamicOps& ops() const = 0;
  virtual ListBuilder& add(const JsonValue& value) = 0;
  virtual ListBuilder& add(const DataResult<JsonValue>& value) = 0;
  virtual ListBuilder& withErrorsFrom(const DataResultBase& result) = 0;
  virtual ListBuilder& mapError(const StringUnaryOperator& onError) = 0;
  virtual DataResult<JsonValue> build(const JsonValue& prefix) = 0;

  DataResult<JsonValue> build(const DataResult<JsonValue>& prefix) {
    return prefix.flatMap([this](const JsonValue& value) { return build(value); });
  }
};

// JsonOps.ArrayBuilder 的移植（JsonOps.listBuilder() 返回的构建器）。
//
// 累加器放在 shared_ptr 之后持有：DFU 的构建器
// （ImmutableList.Builder、JsonObject……）是*可变对象*，由
// applicative 调用链按引用携带，因此 `map`/`apply2stable` 只复制指针，
// 追加保持 O(1)。若把容器内联存储，则每个元素都要复制
// 整个已累加的列表（编码复杂度呈平方级）。
class ArrayListBuilder : public ListBuilder {
 public:
  using State = std::shared_ptr<JsonValue::Array>;

  explicit ArrayListBuilder(const DynamicOps& ops)
      : ops_(&ops), builder_(DataResult<State>::success(initial(), Lifecycle::stable())) {}

  const DynamicOps& ops() const override { return *ops_; }

  ListBuilder& add(const JsonValue& value) override {
    builder_ = builder_.map([value](const State& state) {
      state->push_back(value);
      return state;
    });
    return *this;
  }

  ListBuilder& add(const DataResult<JsonValue>& value) override {
    builder_ = builder_.apply2stable(
        [](const State& state, const JsonValue& element) {
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

  DataResult<JsonValue> build(const JsonValue& prefix) override;

 private:
  static State initial() { return std::make_shared<JsonValue::Array>(); }

  const DynamicOps* ops_;
  DataResult<State> builder_;
};

// ---------------------------------------------------------------------------
// RecordBuilder<T>
// ---------------------------------------------------------------------------
class RecordBuilder {
 public:
  virtual ~RecordBuilder() = default;

  virtual const DynamicOps& ops() const = 0;
  virtual RecordBuilder& add(const JsonValue& key, const JsonValue& value) = 0;
  virtual RecordBuilder& add(const JsonValue& key, const DataResult<JsonValue>& value) = 0;
  virtual RecordBuilder& add(const DataResult<JsonValue>& key, const DataResult<JsonValue>& value) = 0;
  virtual RecordBuilder& add(const std::string& key, const JsonValue& value);
  virtual RecordBuilder& add(const std::string& key, const DataResult<JsonValue>& value);
  virtual RecordBuilder& withErrorsFrom(const DataResultBase& result) = 0;
  virtual RecordBuilder& setLifecycle(const Lifecycle& lifecycle) = 0;
  virtual RecordBuilder& mapError(const StringUnaryOperator& onError) = 0;
  virtual DataResult<JsonValue> build(const JsonValue& prefix) = 0;

  DataResult<JsonValue> build(const DataResult<JsonValue>& prefix) {
    return prefix.flatMap([this](const JsonValue& value) { return build(value); });
  }
};

// RecordBuilder.AbstractBuilder<T, R> 的移植。
template <class State>
class AbstractRecordBuilder : public RecordBuilder {
 public:
  const DynamicOps& ops() const override { return *ops_; }

  RecordBuilder& withErrorsFrom(const DataResultBase& result) override {
    builder_ = detail::propagateErrors(builder_, result);
    return *this;
  }

  RecordBuilder& setLifecycle(const Lifecycle& lifecycle) override {
    builder_ = builder_.setLifecycle(lifecycle);
    return *this;
  }

  RecordBuilder& mapError(const StringUnaryOperator& onError) override {
    builder_ = builder_.mapError(onError);
    return *this;
  }

  DataResult<JsonValue> build(const JsonValue& prefix) override {
    DataResult<JsonValue> result =
        builder_.flatMap([&](const State& state) { return buildState(state, prefix); });
    builder_ = DataResult<State>::success(initBuilder(), Lifecycle::stable());
    return result;
  }

 protected:
  AbstractRecordBuilder(const DynamicOps& ops, State initial)
      : ops_(&ops), builder_(DataResult<State>::success(std::move(initial), Lifecycle::stable())) {}

  virtual State initBuilder() const = 0;
  virtual DataResult<JsonValue> buildState(const State& state, const JsonValue& prefix) = 0;

  const DynamicOps* ops_;
  DataResult<State> builder_;
};

// RecordBuilder.AbstractUniversalBuilder / RecordBuilder.MapBuilder 的移植。
class UniversalRecordBuilder
    : public AbstractRecordBuilder<std::shared_ptr<std::vector<std::pair<JsonValue, JsonValue>>>> {
 public:
  using Members = std::vector<std::pair<JsonValue, JsonValue>>;
  using State = std::shared_ptr<Members>;

  explicit UniversalRecordBuilder(const DynamicOps& ops) : AbstractRecordBuilder<State>(ops, initial()) {}

  RecordBuilder& add(const JsonValue& key, const JsonValue& value) override {
    builder_ = builder_.map([key, value](const State& state) {
      state->emplace_back(key, value);
      return state;
    });
    return *this;
  }

  RecordBuilder& add(const JsonValue& key, const DataResult<JsonValue>& value) override {
    builder_ = builder_.apply2stable(
        [key](const State& state, const JsonValue& element) {
          state->emplace_back(key, element);
          return state;
        },
        // 编码失败会携带其来源成员：`severity: Unmapped E value`。
        key.isString() ? value.addPath(key.asString()) : value);
    return *this;
  }

  RecordBuilder& add(const DataResult<JsonValue>& key, const DataResult<JsonValue>& value) override {
    const auto entry = key.apply2stable(
        [](const JsonValue& k, const JsonValue& v) { return std::make_pair(k, v); }, value);
    builder_ = builder_.apply2stable(
        [](const State& state, const std::pair<JsonValue, JsonValue>& pair) {
          state->push_back(pair);
          return state;
        },
        entry);
    return *this;
  }

  static State initial() { return std::make_shared<Members>(); }

 protected:
  State initBuilder() const override { return initial(); }
  DataResult<JsonValue> buildState(const State& state, const JsonValue& prefix) override;
};

// RecordBuilder.AbstractStringBuilder / JsonOps.JsonRecordBuilder 的移植。
class StringRecordBuilder
    : public AbstractRecordBuilder<std::shared_ptr<std::vector<std::pair<std::string, JsonValue>>>> {
 public:
  using Members = std::vector<std::pair<std::string, JsonValue>>;
  using State = std::shared_ptr<Members>;

  explicit StringRecordBuilder(const DynamicOps& ops) : AbstractRecordBuilder<State>(ops, initial()) {}

  RecordBuilder& add(const std::string& key, const JsonValue& value) override {
    builder_ = builder_.map([key, value](const State& state) {
      putJsonMember(*state, key, value);
      return state;
    });
    return *this;
  }

  RecordBuilder& add(const std::string& key, const DataResult<JsonValue>& value) override {
    builder_ = builder_.apply2stable(
        [key](const State& state, const JsonValue& element) {
          putJsonMember(*state, key, element);
          return state;
        },
        // 编码失败会携带其来源成员。
        value.addPath(key));
    return *this;
  }

  RecordBuilder& add(const JsonValue& key, const JsonValue& value) override;
  RecordBuilder& add(const JsonValue& key, const DataResult<JsonValue>& value) override;
  RecordBuilder& add(const DataResult<JsonValue>& key, const DataResult<JsonValue>& value) override;

  static State initial() { return std::make_shared<Members>(); }

 protected:
  State initBuilder() const override { return initial(); }
  DataResult<JsonValue> buildState(const State& state, const JsonValue& prefix) override;
};

// MapEncoder.makeCompressedBuilder 的 CompressedRecordBuilder 的移植。
class CompressedRecordBuilder
    : public AbstractRecordBuilder<std::shared_ptr<std::vector<std::optional<JsonValue>>>> {
 public:
  using Slots = std::vector<std::optional<JsonValue>>;
  using State = std::shared_ptr<Slots>;

  CompressedRecordBuilder(const DynamicOps& ops, KeyCompressor compressor)
      : AbstractRecordBuilder<State>(ops, emptySlots(compressor.size())),
        compressor_(std::move(compressor)) {}

  static State emptySlots(int size) {
    return std::make_shared<Slots>(static_cast<size_t>(size), std::nullopt);
  }

  RecordBuilder& add(const JsonValue& key, const JsonValue& value) override {
    builder_ = builder_.map([this, key, value](const State& state) {
      assign(key, value, state);
      return state;
    });
    return *this;
  }

  RecordBuilder& add(const JsonValue& key, const DataResult<JsonValue>& value) override {
    builder_ = builder_.apply2stable(
        [this, key](const State& state, const JsonValue& element) {
          assign(key, element, state);
          return state;
        },
        // 压缩记录会保留键名以便诊断（槽位下标
        // 对读者毫无意义）。
        key.isString() ? value.addPath(key.asString()) : value);
    return *this;
  }

  RecordBuilder& add(const DataResult<JsonValue>& key, const DataResult<JsonValue>& value) override {
    const auto entry = key.apply2stable(
        [](const JsonValue& k, const JsonValue& v) { return std::make_pair(k, v); }, value);
    builder_ = builder_.apply2stable(
        [this](const State& state, const std::pair<JsonValue, JsonValue>& pair) {
          assign(pair.first, pair.second, state);
          return state;
        },
        entry);
    return *this;
  }

 protected:
  State initBuilder() const override { return emptySlots(compressor_.size()); }
  DataResult<JsonValue> buildState(const State& state, const JsonValue& prefix) override;

 private:
  // Java 写入一个由压缩器确定大小的稠密列表；从未被写入的条目
  // 保持为 null，这正是压缩解码器读回时
  // 视为「缺失」的东西。
  void assign(const JsonValue& key, const JsonValue& value, const State& state) const {
    const int index = compressor_.compress(key);
    if (index >= 0 && static_cast<size_t>(index) < state->size()) {
      (*state)[static_cast<size_t>(index)] = value;
    }
  }

  KeyCompressor compressor_;
};

// ---------------------------------------------------------------------------
// DynamicOps<T>
// ---------------------------------------------------------------------------
class DynamicOps {
 public:
  virtual ~DynamicOps() = default;

  // --- 基础类型 ---------------------------------------------------------
  virtual JsonValue empty() const = 0;

  virtual JsonValue emptyMap() const { return createMap({}); }
  virtual JsonValue emptyList() const { return createList({}); }

  // 在 DFU 中，这里会把值重写成目标 ops 的表示形式；
  // 本移植的值类型是通用的，因此 JsonOps 把它实现为恒等变换。
  virtual JsonValue convertTo(const DynamicOps& outOps, const JsonValue& input) const = 0;

  virtual DataResult<Number> getNumberValue(const JsonValue& input) const = 0;

  Number getNumberValue(const JsonValue& input, const Number& defaultValue) const {
    const DataResult<Number> result = getNumberValue(input);
    return result.result().has_value() ? *result.result() : defaultValue;
  }

  virtual JsonValue createNumeric(const Number& value) const = 0;

  virtual JsonValue createByte(int8_t value) const { return createNumeric(Number::ofInt(value)); }
  virtual JsonValue createShort(int16_t value) const { return createNumeric(Number::ofInt(value)); }
  virtual JsonValue createInt(int32_t value) const { return createNumeric(Number::ofInt(value)); }
  virtual JsonValue createLong(int64_t value) const { return createNumeric(Number::ofInt(value)); }
  virtual JsonValue createFloat(float value) const { return createNumeric(Number::ofDouble(value)); }
  virtual JsonValue createDouble(double value) const { return createNumeric(Number::ofDouble(value)); }

  virtual DataResult<bool> getBooleanValue(const JsonValue& input) const {
    return getNumberValue(input).map([](const Number& number) { return number.booleanValue(); });
  }
  virtual JsonValue createBoolean(bool value) const { return createByte(value ? 1 : 0); }

  virtual DataResult<std::string> getStringValue(const JsonValue& input) const = 0;
  virtual JsonValue createString(const std::string& value) const = 0;

  // --- 列表/映射构造 ---------------------------------------------
  virtual DataResult<JsonValue> mergeToList(const JsonValue& list, const JsonValue& value) const = 0;

  virtual DataResult<JsonValue> mergeToList(const JsonValue& list,
                                            const std::vector<JsonValue>& values) const {
    DataResult<JsonValue> result = DataResult<JsonValue>::success(list);
    for (const JsonValue& value : values) {
      result = result.flatMap([&](const JsonValue& current) { return mergeToList(current, value); });
    }
    return result;
  }

  virtual DataResult<JsonValue> mergeToMap(const JsonValue& map, const JsonValue& key,
                                           const JsonValue& value) const = 0;

  virtual DataResult<JsonValue> mergeToMap(
      const JsonValue& map, const std::vector<std::pair<JsonValue, JsonValue>>& values) const {
    DataResult<JsonValue> result = DataResult<JsonValue>::success(map);
    for (const auto& entry : values) {
      result = result.flatMap(
          [&](const JsonValue& current) { return mergeToMap(current, entry.first, entry.second); });
    }
    return result;
  }

  virtual DataResult<JsonValue> mergeToMap(const JsonValue& map, const MapLike& values) const {
    DataResult<JsonValue> result = DataResult<JsonValue>::success(map);
    for (const auto& entry : values.entries()) {
      result = result.flatMap(
          [&](const JsonValue& current) { return mergeToMap(current, entry.first, entry.second); });
    }
    return result;
  }

  virtual DataResult<JsonValue> mergeToPrimitive(const JsonValue& prefix,
                                                 const JsonValue& value) const {
    if (!(prefix == empty())) {
      return DataResult<JsonValue>::error(
          "Do not know how to append a primitive value " + value.dump() + " to " + prefix.dump(),
          value);
    }
    return DataResult<JsonValue>::success(value);
  }

  // --- 映射访问 ---------------------------------------------------------
  virtual DataResult<std::vector<std::pair<JsonValue, JsonValue>>> getMapValues(
      const JsonValue& input) const = 0;

  virtual DataResult<MapLikePtr> getMap(const JsonValue& input) const {
    if (!input.isObject()) {
      return DataResult<MapLikePtr>::error("Not a JSON object: " + input.dump());
    }
    return DataResult<MapLikePtr>::success(std::make_shared<JsonObjectMapLike>(input));
  }

  virtual JsonValue createMap(const std::vector<std::pair<JsonValue, JsonValue>>& entries) const = 0;

  // --- 列表访问 --------------------------------------------------------
  virtual DataResult<std::vector<JsonValue>> getStream(const JsonValue& input) const = 0;

  virtual DataResult<std::vector<JsonValue>> getList(const JsonValue& input) const {
    return getStream(input);
  }

  virtual JsonValue createList(const std::vector<JsonValue>& values) const = 0;

  virtual JsonValue remove(const JsonValue& input, const std::string& key) const = 0;

  // --- 泛型访问 -----------------------------------------------------
  virtual bool compressMaps() const { return false; }

  DataResult<JsonValue> get(const JsonValue& input, const std::string& key) const {
    return getGeneric(input, createString(key));
  }

  DataResult<JsonValue> getGeneric(const JsonValue& input, const JsonValue& key) const {
    return getMap(input).flatMap([&](const MapLikePtr& map) -> DataResult<JsonValue> {
      const std::optional<JsonValue> value = map->get(key);
      if (!value.has_value()) {
        return DataResult<JsonValue>::error("No element " + key.dump() + " in the map " +
                                            input.dump());
      }
      return DataResult<JsonValue>::success(*value);
    });
  }

  JsonValue set(const JsonValue& input, const std::string& key, const JsonValue& value) const {
    const DataResult<JsonValue> result = mergeToMap(input, createString(key), value);
    return result.result().has_value() ? *result.result() : input;
  }

  JsonValue update(const JsonValue& input, const std::string& key,
                   const std::function<JsonValue(const JsonValue&)>& function) const {
    const DataResult<JsonValue> result =
        get(input, key).map([&](const JsonValue& value) { return set(input, key, function(value)); });
    return result.result().has_value() ? *result.result() : input;
  }

  // --- 构建器 -----------------------------------------------------------
  virtual std::shared_ptr<ListBuilder> listBuilder() const;
  virtual std::shared_ptr<RecordBuilder> mapBuilder() const;

  // --- 转换辅助方法 -------------------------------------------------
  JsonValue convertList(const DynamicOps& outOps, const JsonValue& input) const {
    const DataResult<std::vector<JsonValue>> stream = getStream(input);
    std::vector<JsonValue> converted;
    if (stream.result().has_value()) {
      converted.reserve(stream.result()->size());
      for (const JsonValue& element : *stream.result()) {
        converted.push_back(convertTo(outOps, element));
      }
    }
    return outOps.createList(converted);
  }

  JsonValue convertMap(const DynamicOps& outOps, const JsonValue& input) const {
    const DataResult<std::vector<std::pair<JsonValue, JsonValue>>> entries = getMapValues(input);
    std::vector<std::pair<JsonValue, JsonValue>> converted;
    if (entries.result().has_value()) {
      converted.reserve(entries.result()->size());
      for (const auto& entry : *entries.result()) {
        converted.emplace_back(convertTo(outOps, entry.first), convertTo(outOps, entry.second));
      }
    }
    return outOps.createMap(converted);
  }
};

// ---------------------------------------------------------------------------
// 需要 DynamicOps 完整定义的类外定义
// ---------------------------------------------------------------------------
inline RecordBuilder& RecordBuilder::add(const std::string& key, const JsonValue& value) {
  return add(ops().createString(key), value);
}

inline RecordBuilder& RecordBuilder::add(const std::string& key,
                                        const DataResult<JsonValue>& value) {
  return add(ops().createString(key), value);
}

// 移植自 RecordBuilder.AbstractStringBuilder#add(T key, ...)。
inline RecordBuilder& StringRecordBuilder::add(const JsonValue& key, const JsonValue& value) {
  builder_ = ops().getStringValue(key).flatMap([this, value](const std::string& k) {
    add(k, value);
    return builder_;
  });
  return *this;
}

inline RecordBuilder& StringRecordBuilder::add(const JsonValue& key,
                                              const DataResult<JsonValue>& value) {
  builder_ = ops().getStringValue(key).flatMap([this, value](const std::string& k) {
    add(k, value);
    return builder_;
  });
  return *this;
}

inline RecordBuilder& StringRecordBuilder::add(const DataResult<JsonValue>& key,
                                              const DataResult<JsonValue>& value) {
  builder_ = key.flatMap([this](const JsonValue& k) { return ops().getStringValue(k); })
                 .flatMap([this, value](const std::string& k) {
                   add(k, value);
                   return builder_;
                 });
  return *this;
}

inline DataResult<JsonValue> ArrayListBuilder::build(const JsonValue& prefix) {
  DataResult<JsonValue> result =
      builder_.flatMap([&](const State& array) -> DataResult<JsonValue> {
        if (!prefix.isArray() && !(prefix == ops_->empty())) {
          return DataResult<JsonValue>::error("Cannot append a list to not a list: " + prefix.dump(),
                                              prefix);
        }
        JsonValue::Array out;
        if (!(prefix == ops_->empty())) {
          out = prefix.asArray();
        }
        out.insert(out.end(), array->begin(), array->end());
        return DataResult<JsonValue>::success(JsonValue::array(std::move(out)), Lifecycle::stable());
      });
  builder_ = DataResult<State>::success(initial(), Lifecycle::stable());
  return result;
}

inline DataResult<JsonValue> UniversalRecordBuilder::buildState(const State& state,
                                                                const JsonValue& prefix) {
  return ops_->mergeToMap(prefix, *state);
}

inline DataResult<JsonValue> StringRecordBuilder::buildState(const State& state,
                                                             const JsonValue& prefix) {
  // 移植自 JsonOps.JsonRecordBuilder#build。
  if (prefix.isNull() || prefix == ops_->empty()) {
    JsonValue::Object members;
    members.reserve(state->size());
    for (const auto& entry : *state) {
      members.emplace_back(entry.first, entry.second);
    }
    return DataResult<JsonValue>::success(JsonValue::object(std::move(members)));
  }
  if (!prefix.isObject()) {
    return DataResult<JsonValue>::error("mergeToMap called with not a map: " + prefix.dump(), prefix);
  }
  JsonValue::Object members = prefix.asObject();
  for (const auto& entry : *state) {
    putJsonMember(members, entry.first, entry.second);
  }
  return DataResult<JsonValue>::success(JsonValue::object(std::move(members)));
}

inline DataResult<JsonValue> CompressedRecordBuilder::buildState(const State& state,
                                                                const JsonValue& prefix) {
  std::vector<JsonValue> values;
  values.reserve(state->size());
  for (const std::optional<JsonValue>& entry : *state) {
    values.push_back(entry.has_value() ? *entry : JsonValue::null());
  }
  return ops_->mergeToList(prefix, values);
}

inline std::shared_ptr<ListBuilder> DynamicOps::listBuilder() const {
  return std::make_shared<ArrayListBuilder>(*this);
}

inline std::shared_ptr<RecordBuilder> DynamicOps::mapBuilder() const {
  return std::make_shared<UniversalRecordBuilder>(*this);
}

inline KeyCompressor::KeyCompressor(const DynamicOps& ops, const std::vector<JsonValue>& keys)
    : ops_(&ops) {
  for (const JsonValue& key : keys) {
    const std::string identity = key.dump();
    if (compressByValue_.count(identity) != 0) {
      continue;
    }
    const int next = size();
    compressByValue_.emplace(identity, next);
    const DataResult<std::string> asString = ops.getStringValue(key);
    if (asString.result().has_value()) {
      compressByString_.emplace(*asString.result(), next);
    }
    decompress_.push_back(key);
  }
}

inline int KeyCompressor::compress(const std::string& key) const {
  const auto found = compressByString_.find(key);
  if (found != compressByString_.end()) {
    return found->second;
  }
  return compress(ops_->createString(key));
}

inline int KeyCompressor::compress(const JsonValue& key) const {
  const auto found = compressByValue_.find(key.dump());
  return found == compressByValue_.end() ? -1 : found->second;
}

}  // namespace codec


// ===========================================================================
// 5/8  json_ops.hpp -- JsonOps (INSTANCE / COMPRESSED)
// ===========================================================================

// Mojang DataFixerUpper `com.mojang.serialization` Codec API 的 C++17 移植。
//
// json_ops.hpp -- com.mojang.serialization.JsonOps 的移植（由
// JsonValue DOM 支撑，而非 Gson 的 JsonElement）。



namespace codec {

class JsonOps : public DynamicOps {
 public:
  static const JsonOps INSTANCE;
  static const JsonOps COMPRESSED;

  explicit JsonOps(bool compressed) : compressed_(compressed) {}

  // 重新暴露 JsonOps 特化的 DynamicOps 重载集合，使
  // `ops.getNumberValue(value, fallback)` / `ops.mergeToMap(map, mapLike)` 在
  // JsonOps 实例上仍能正常工作。
  using DynamicOps::getNumberValue;
  using DynamicOps::mergeToMap;

  // --- 基础类型 ---------------------------------------------------------
  JsonValue empty() const override { return JsonValue::null(); }

  JsonValue convertTo(const DynamicOps& outOps, const JsonValue& input) const override {
    (void)outOps;
    // 移植版的序列化值类型是通用的（它扮演
    // JsonElement 的角色），因此在各 ops 之间转换是恒等操作。
    return input;
  }

  DataResult<Number> getNumberValue(const JsonValue& input) const override {
    if (input.isNumber()) {
      return DataResult<Number>::success(input.asNumber());
    }
    if (input.isBoolean()) {
      return DataResult<Number>::success(Number::ofInt(input.asBoolean() ? 1 : 0));
    }
    if (compressed_ && input.isString()) {
      const std::string& text = input.asString();
      char* end = nullptr;
      const long long value = std::strtoll(text.c_str(), &end, 10);
      if (end != nullptr && *end == '\0' && !text.empty()) {
        return DataResult<Number>::success(Number::ofInt(static_cast<int64_t>(value)));
      }
      return DataResult<Number>::error("Not a number: NumberFormatException " + text);
    }
    return DataResult<Number>::error("Not a number: " + input.dump());
  }

  JsonValue createNumeric(const Number& value) const override { return JsonValue::number(value); }

  DataResult<bool> getBooleanValue(const JsonValue& input) const override {
    if (input.isBoolean()) {
      return DataResult<bool>::success(input.asBoolean());
    }
    if (input.isNumber()) {
      return DataResult<bool>::success(input.asNumber().byteValue() != 0);
    }
    return DataResult<bool>::error("Not a boolean: " + input.dump());
  }

  JsonValue createBoolean(bool value) const override { return JsonValue::boolean(value); }

  DataResult<std::string> getStringValue(const JsonValue& input) const override {
    if (input.isString()) {
      return DataResult<std::string>::success(input.asString());
    }
    if (compressed_ && input.isNumber()) {
      return DataResult<std::string>::success(input.asNumber().toString());
    }
    return DataResult<std::string>::error("Not a string: " + input.dump());
  }

  JsonValue createString(const std::string& value) const override {
    return JsonValue::string(value);
  }

  // --- list/map 构造 ---------------------------------------------
  DataResult<JsonValue> mergeToList(const JsonValue& list, const JsonValue& value) const override {
    if (!list.isArray() && !(list == empty())) {
      return DataResult<JsonValue>::error("mergeToList called with not a list: " + list.dump(), list);
    }
    JsonValue::Array out = list.isArray() ? list.asArray() : JsonValue::Array{};
    out.push_back(value);
    return DataResult<JsonValue>::success(JsonValue::array(std::move(out)));
  }

  DataResult<JsonValue> mergeToList(const JsonValue& list,
                                    const std::vector<JsonValue>& values) const override {
    if (!list.isArray() && !(list == empty())) {
      return DataResult<JsonValue>::error("mergeToList called with not a list: " + list.dump(), list);
    }
    JsonValue::Array out = list.isArray() ? list.asArray() : JsonValue::Array{};
    out.insert(out.end(), values.begin(), values.end());
    return DataResult<JsonValue>::success(JsonValue::array(std::move(out)));
  }

  DataResult<JsonValue> mergeToMap(const JsonValue& map, const JsonValue& key,
                                   const JsonValue& value) const override {
    if (!map.isObject() && !(map == empty())) {
      return DataResult<JsonValue>::error("mergeToMap called with not a map: " + map.dump(), map);
    }
    const std::optional<std::string> keyString = asKeyString(key);
    if (!keyString.has_value()) {
      return DataResult<JsonValue>::error("key is not a string: " + key.dump(), map);
    }
    JsonValue::Object out = map.isObject() ? map.asObject() : JsonValue::Object{};
    putJsonMember(out, *keyString, value);
    return DataResult<JsonValue>::success(JsonValue::object(std::move(out)));
  }

  DataResult<JsonValue> mergeToMap(
      const JsonValue& map,
      const std::vector<std::pair<JsonValue, JsonValue>>& values) const override {
    if (!map.isObject() && !(map == empty())) {
      return DataResult<JsonValue>::error("mergeToMap called with not a map: " + map.dump(), map);
    }
    JsonValue::Object out = map.isObject() ? map.asObject() : JsonValue::Object{};
    std::vector<JsonValue> missed;
    for (const auto& entry : values) {
      const std::optional<std::string> keyString = asKeyString(entry.first);
      if (keyString.has_value()) {
        putJsonMember(out, *keyString, entry.second);
      } else {
        missed.push_back(entry.first);
      }
    }
    if (!missed.empty()) {
      return DataResult<JsonValue>::error("some keys are not strings: " + JsonValue::array(missed).dump(),
                                          JsonValue::object(out));
    }
    return DataResult<JsonValue>::success(JsonValue::object(std::move(out)));
  }

  // --- map 访问 ---------------------------------------------------------
  DataResult<std::vector<std::pair<JsonValue, JsonValue>>> getMapValues(
      const JsonValue& input) const override {
    if (!input.isObject()) {
      return DataResult<std::vector<std::pair<JsonValue, JsonValue>>>::error("Not a JSON object: " +
                                                                            input.dump());
    }
    std::vector<std::pair<JsonValue, JsonValue>> out;
    out.reserve(input.size());
    for (const auto& entry : input.asObject()) {
      out.emplace_back(JsonValue::string(entry.first), entry.second);
    }
    return DataResult<std::vector<std::pair<JsonValue, JsonValue>>>::success(std::move(out));
  }

  JsonValue createMap(const std::vector<std::pair<JsonValue, JsonValue>>& entries) const override {
    JsonValue::Object out;
    out.reserve(entries.size());
    for (const auto& entry : entries) {
      const std::optional<std::string> keyString = asKeyString(entry.first);
      if (keyString.has_value()) {
        putJsonMember(out, *keyString, entry.second);
      }
    }
    return JsonValue::object(std::move(out));
  }

  // --- list 访问 --------------------------------------------------------
  DataResult<std::vector<JsonValue>> getStream(const JsonValue& input) const override {
    if (!input.isArray()) {
      return DataResult<std::vector<JsonValue>>::error("Not a json array: " + input.dump());
    }
    return DataResult<std::vector<JsonValue>>::success(input.asArray());
  }

  DataResult<std::vector<JsonValue>> getList(const JsonValue& input) const override {
    return getStream(input);
  }

  JsonValue createList(const std::vector<JsonValue>& values) const override {
    return JsonValue::array(values);
  }

  JsonValue remove(const JsonValue& input, const std::string& key) const override {
    if (!input.isObject()) {
      return input;
    }
    JsonValue::Object out;
    for (const auto& entry : input.asObject()) {
      if (entry.first != key) {
        out.push_back(entry);
      }
    }
    return JsonValue::object(std::move(out));
  }

  // --- 行为 ----------------------------------------------------------
  bool compressMaps() const override { return compressed_; }

  std::shared_ptr<ListBuilder> listBuilder() const override {
    return std::make_shared<ArrayListBuilder>(*this);
  }

  std::shared_ptr<RecordBuilder> mapBuilder() const override {
    return std::make_shared<StringRecordBuilder>(*this);
  }

  std::string toString() const { return "JSON"; }

 private:
  // Java 的 `key.getAsString()`：字符串，或压缩模式下的数字。
  std::optional<std::string> asKeyString(const JsonValue& key) const {
    if (key.isString()) {
      return key.asString();
    }
    if (compressed_ && key.isNumber()) {
      return key.asNumber().toString();
    }
    return std::nullopt;
  }

  bool compressed_ = false;
};

inline const JsonOps JsonOps::INSTANCE{false};
inline const JsonOps JsonOps::COMPRESSED{true};

}  // namespace codec


// ===========================================================================
// 6/8  codec.hpp -- Encoder, Decoder, MapEncoder, MapDecoder, Codec, MapCodec
// ===========================================================================

// Mojang DataFixerUpper `com.mojang.serialization` Codec API 的 C++17 移植。
//
// codec.hpp -- Encoder、Decoder、MapEncoder、MapDecoder、Codec 与
// MapCodec 的移植，包括各组合子方法（xmap / flatXmap / comapFlatMap /
// orElse / mapResult / fieldOf / optionalFieldOf / promotePartial / ...）。
//
// Java 接口基于匿名实现；C++ 移植则用 std::function 包装保持
// 相同的组合模型，因此 Codec 与 MapCodec
// 是可拷贝的值类型，既能存入容器，也能
// 被 lambda 捕获。



namespace codec {

template <class A>
class Codec;
template <class A>
class Encoder;
template <class A>
class Decoder;
template <class A>
class MapCodec;
template <class A>
class MapEncoder;
template <class A>
class MapDecoder;
template <class O, class F>
class RecordField;
template <class O, class F>
class GetterField;

// DFU 的 Codec.ResultFunction / MapCodec.ResultFunction。
template <class A>
struct CodecResultFunction {
  std::function<DataResult<std::pair<A, JsonValue>>(
      const DynamicOps&, const JsonValue&, const DataResult<std::pair<A, JsonValue>>&)>
      apply;
  std::function<DataResult<JsonValue>(const DynamicOps&, const A&, const DataResult<JsonValue>&)>
      coApply;
};

template <class A>
struct MapResultFunction {
  std::function<DataResult<A>(const DynamicOps&, const MapLike&, const DataResult<A>&)> apply;
  std::function<RecordBuilder&(const DynamicOps&, const A&, RecordBuilder&)> coApply;
};

// ===========================================================================
// Encoder<A>（com.mojang.serialization.Encoder）
// ===========================================================================
template <class A>
class Encoder {
 public:
  using Fn = std::function<DataResult<JsonValue>(const A&, const DynamicOps&, const JsonValue&)>;

  Encoder() = default;
  explicit Encoder(Fn fn) : fn_(std::move(fn)) {}

  bool valid() const { return static_cast<bool>(fn_); }

  DataResult<JsonValue> encode(const A& input, const DynamicOps& ops,
                               const JsonValue& prefix) const {
    return fn_(input, ops, prefix);
  }

  DataResult<JsonValue> encodeStart(const DynamicOps& ops, const A& input) const {
    return fn_(input, ops, ops.empty());
  }

  MapEncoder<A> fieldOf(const std::string& name) const;

  template <class B>
  Encoder<B> comap(std::function<A(const B&)> function) const {
    Fn fn = fn_;
    return Encoder<B>([fn, function](const B& input, const DynamicOps& ops,
                                     const JsonValue& prefix) {
      return fn(function(input), ops, prefix);
    });
  }

  template <class B>
  Encoder<B> flatComap(std::function<DataResult<A>(const B&)> function) const {
    Fn fn = fn_;
    return Encoder<B>([fn, function](const B& input, const DynamicOps& ops,
                                     const JsonValue& prefix) {
      return function(input).flatMap(
          [&](const A& mapped) { return fn(mapped, ops, prefix); });
    });
  }

  Encoder<A> withLifecycle(const Lifecycle& lifecycle) const {
    Fn fn = fn_;
    return Encoder<A>([fn, lifecycle](const A& input, const DynamicOps& ops,
                                      const JsonValue& prefix) {
      return fn(input, ops, prefix).setLifecycle(lifecycle);
    });
  }

  // Encoder.empty()——不写入任何内容的 MapEncoder。
  static MapEncoder<A> empty();

  static Encoder<A> error(std::string message) {
    return Encoder<A>([message](const A&, const DynamicOps&, const JsonValue&) {
      return DataResult<JsonValue>::error(message);
    });
  }

 private:
  Fn fn_;
};

// ===========================================================================
// Decoder<A>（com.mojang.serialization.Decoder）
// ===========================================================================
template <class A>
class Decoder {
 public:
  using Fn =
      std::function<DataResult<std::pair<A, JsonValue>>(const DynamicOps&, const JsonValue&)>;

  Decoder() = default;
  explicit Decoder(Fn fn) : fn_(std::move(fn)) {}

  bool valid() const { return static_cast<bool>(fn_); }

  DataResult<std::pair<A, JsonValue>> decode(const DynamicOps& ops,
                                             const JsonValue& input) const {
    return fn_(ops, input);
  }

  DataResult<A> parse(const DynamicOps& ops, const JsonValue& input) const {
    return decode(ops, input).map([](const std::pair<A, JsonValue>& pair) { return pair.first; });
  }

  MapDecoder<A> fieldOf(const std::string& name) const;

  template <class B>
  Decoder<B> map(std::function<B(const A&)> function) const {
    Fn fn = fn_;
    return Decoder<B>([fn, function](const DynamicOps& ops, const JsonValue& input) {
      return fn(ops, input).map([&](const std::pair<A, JsonValue>& pair) {
        return std::make_pair(function(pair.first), pair.second);
      });
    });
  }

  template <class B>
  Decoder<B> flatMap(std::function<DataResult<B>(const A&)> function) const {
    Fn fn = fn_;
    return Decoder<B>([fn, function](const DynamicOps& ops, const JsonValue& input) {
      return fn(ops, input).flatMap([&](const std::pair<A, JsonValue>& pair) {
        return function(pair.first).map([&](const B& mapped) {
          return std::make_pair(mapped, pair.second);
        });
      });
    });
  }

  Decoder<A> promotePartial(const ErrorHandler& onError) const {
    Fn fn = fn_;
    return Decoder<A>([fn, onError](const DynamicOps& ops, const JsonValue& input) {
      return fn(ops, input).promotePartial(onError);
    });
  }

  Decoder<A> withLifecycle(const Lifecycle& lifecycle) const {
    Fn fn = fn_;
    return Decoder<A>([fn, lifecycle](const DynamicOps& ops, const JsonValue& input) {
      return fn(ops, input).setLifecycle(lifecycle);
    });
  }

  // Decoder.unit——忽略输入并产出 `value` 的 MapDecoder。
  static MapDecoder<A> unit(A value);

  static Decoder<A> error(std::string message) {
    return Decoder<A>([message](const DynamicOps&, const JsonValue&) {
      return DataResult<std::pair<A, JsonValue>>::error(message);
    });
  }

 private:
  Fn fn_;
};

// ===========================================================================
// MapEncoder<A>（com.mojang.serialization.MapEncoder）
// ===========================================================================
template <class A>
class MapEncoder {
 public:
  using Fn = std::function<RecordBuilder&(const A&, const DynamicOps&, RecordBuilder&)>;
  using KeysFn = std::function<std::vector<JsonValue>(const DynamicOps&)>;

  MapEncoder() = default;
  MapEncoder(Fn fn, KeysFn keys) : fn_(std::move(fn)), keys_(std::move(keys)) {}

  bool valid() const { return static_cast<bool>(fn_); }

  RecordBuilder& encode(const A& input, const DynamicOps& ops, RecordBuilder& prefix) const {
    return fn_(input, ops, prefix);
  }

  std::vector<JsonValue> keys(const DynamicOps& ops) const {
    return keys_ ? keys_(ops) : std::vector<JsonValue>{};
  }

  // MapEncoder.compressedBuilder：遵循 DynamicOps.compressMaps()。
  std::shared_ptr<RecordBuilder> compressedBuilder(const DynamicOps& ops) const;

  Encoder<A> encoder() const {
    MapEncoder<A> self = *this;
    return Encoder<A>([self](const A& input, const DynamicOps& ops, const JsonValue& prefix) {
      return self.encode(input, ops, *self.compressedBuilder(ops)).build(prefix);
    });
  }

  template <class B>
  MapEncoder<B> comap(std::function<A(const B&)> function) const {
    MapEncoder<A> self = *this;
    return MapEncoder<B>(
        [self, function](const B& input, const DynamicOps& ops,
                         RecordBuilder& prefix) -> RecordBuilder& {
          return self.encode(function(input), ops, prefix);
        },
        [self](const DynamicOps& ops) { return self.keys(ops); });
  }

  template <class B>
  MapEncoder<B> flatComap(std::function<DataResult<A>(const B&)> function) const {
    MapEncoder<A> self = *this;
    return MapEncoder<B>(
        [self, function](const B& input, const DynamicOps& ops,
                         RecordBuilder& prefix) -> RecordBuilder& {
          const DataResult<A> mapped = function(input);
          RecordBuilder& builder = prefix.withErrorsFrom(mapped);
          if (!mapped.result().has_value()) {
            return builder;
          }
          return self.encode(*mapped.result(), ops, builder);
        },
        [self](const DynamicOps& ops) { return self.keys(ops); });
  }

  MapEncoder<A> withLifecycle(const Lifecycle& lifecycle) const {
    MapEncoder<A> self = *this;
    return MapEncoder<A>(
        [self, lifecycle](const A& input, const DynamicOps& ops,
                          RecordBuilder& prefix) -> RecordBuilder& {
          return self.encode(input, ops, prefix).setLifecycle(lifecycle);
        },
        [self](const DynamicOps& ops) { return self.keys(ops); });
  }

  // MapEncoder.empty()（DFU：Encoder.empty）——不写入任何内容，也没有键。
  static MapEncoder<A> empty() {
    return MapEncoder<A>(
        [](const A&, const DynamicOps&, RecordBuilder& prefix) -> RecordBuilder& {
          return prefix;
        },
        [](const DynamicOps&) { return std::vector<JsonValue>{}; });
  }

 private:
  Fn fn_;
  KeysFn keys_;
};

// ===========================================================================
// MapDecoder<A>（com.mojang.serialization.MapDecoder）
// ===========================================================================
template <class A>
class MapDecoder {
 public:
  using Fn = std::function<DataResult<A>(const DynamicOps&, const MapLike&)>;
  using KeysFn = std::function<std::vector<JsonValue>(const DynamicOps&)>;

  MapDecoder() = default;
  MapDecoder(Fn fn, KeysFn keys) : fn_(std::move(fn)), keys_(std::move(keys)) {}

  bool valid() const { return static_cast<bool>(fn_); }

  DataResult<A> decode(const DynamicOps& ops, const MapLike& input) const { return fn_(ops, input); }

  std::vector<JsonValue> keys(const DynamicOps& ops) const {
    return keys_ ? keys_(ops) : std::vector<JsonValue>{};
  }

  // MapDecoder.compressedDecode：当 ops 要求 map 压缩时读取压缩的键列表，
  // 否则从对象视图解码。
  DataResult<A> compressedDecode(const DynamicOps& ops, const JsonValue& input) const {
    if (ops.compressMaps()) {
      const DataResult<std::vector<JsonValue>> listResult = ops.getList(input);
      if (listResult.isError()) {
        return DataResult<A>::error("Input is not a list");
      }
      const KeyCompressor compressor(ops, keys(ops));
      const CompressedMapLike map(compressor, *listResult.result());
      return fn_(ops, map);
    }
    return ops.getMap(input).setLifecycle(Lifecycle::stable()).flatMap(
        [&](const MapLikePtr& map) { return fn_(ops, *map); });
  }

  Decoder<A> decoder() const {
    MapDecoder<A> self = *this;
    return Decoder<A>([self](const DynamicOps& ops, const JsonValue& input) {
      return self.compressedDecode(ops, input).map([&input](const A& value) {
        return std::make_pair(value, input);
      });
    });
  }

  template <class B>
  MapDecoder<B> map(std::function<B(const A&)> function) const {
    MapDecoder<A> self = *this;
    return MapDecoder<B>(
        [self, function](const DynamicOps& ops, const MapLike& input) {
          return self.decode(ops, input).map(function);
        },
        [self](const DynamicOps& ops) { return self.keys(ops); });
  }

  template <class B>
  MapDecoder<B> flatMap(std::function<DataResult<B>(const A&)> function) const {
    MapDecoder<A> self = *this;
    return MapDecoder<B>(
        [self, function](const DynamicOps& ops, const MapLike& input) {
          return self.decode(ops, input).flatMap(function);
        },
        [self](const DynamicOps& ops) { return self.keys(ops); });
  }

  MapDecoder<A> withLifecycle(const Lifecycle& lifecycle) const {
    MapDecoder<A> self = *this;
    return MapDecoder<A>(
        [self, lifecycle](const DynamicOps& ops, const MapLike& input) {
          return self.decode(ops, input).setLifecycle(lifecycle);
        },
        [self](const DynamicOps& ops) { return self.keys(ops); });
  }

  // Decoder.unit：忽略输入，总是产出 `value`，没有任何键。
  static MapDecoder<A> unit(A value) {
    return MapDecoder<A>(
        [value](const DynamicOps&, const MapLike&) { return DataResult<A>::success(value); },
        [](const DynamicOps&) { return std::vector<JsonValue>{}; });
  }

 private:
  Fn fn_;
  KeysFn keys_;
};

// ===========================================================================
// MapCodec<A>（com.mojang.serialization.MapCodec）
// ===========================================================================
template <class A>
class MapCodec {
 public:
  MapCodec() = default;
  MapCodec(MapEncoder<A> encoder, MapDecoder<A> decoder, std::string name)
      : encoder_(std::move(encoder)), decoder_(std::move(decoder)), name_(std::move(name)) {}

  static MapCodec of(MapEncoder<A> encoder, MapDecoder<A> decoder, std::string name) {
    return MapCodec(std::move(encoder), std::move(decoder), std::move(name));
  }

  const MapEncoder<A>& encoder() const { return encoder_; }
  const MapDecoder<A>& decoder() const { return decoder_; }
  const std::string& name() const { return name_; }
  bool valid() const { return encoder_.valid() || decoder_.valid(); }

  // MapCodec.keys = Stream.concat(encoder.keys(ops), decoder.keys(ops))
  std::vector<JsonValue> keys(const DynamicOps& ops) const {
    std::vector<JsonValue> out = encoder_.keys(ops);
    const std::vector<JsonValue> decoderKeys = decoder_.keys(ops);
    out.insert(out.end(), decoderKeys.begin(), decoderKeys.end());
    return out;
  }

  DataResult<A> decode(const DynamicOps& ops, const MapLike& input) const {
    return decoder_.decode(ops, input);
  }

  RecordBuilder& encode(const A& input, const DynamicOps& ops, RecordBuilder& prefix) const {
    return encoder_.encode(input, ops, prefix);
  }

  DataResult<A> compressedDecode(const DynamicOps& ops, const JsonValue& input) const {
    return decoder_.compressedDecode(ops, input);
  }

  std::shared_ptr<RecordBuilder> compressedBuilder(const DynamicOps& ops) const {
    return encoder_.compressedBuilder(ops);
  }

  KeyCompressor compressor(const DynamicOps& ops) const { return KeyCompressor(ops, keys(ops)); }

  // MapCodec.codec()——把该 map codec 包装成一个完整的 Codec。
  Codec<A> codec() const;

  // MapCodec.forGetter——构建仅 getter 的 record 字段（record_codec.hpp）。
  template <class O>
  GetterField<O, A> forGetter(std::function<A(const O&)> getter) const;

  MapCodec<A> fieldOf(const std::string& name) const { return codec().fieldOf(name); }

  template <class S>
  MapCodec<S> xmap(std::function<S(const A&)> to, std::function<A(const S&)> from) const {
    return MapCodec<S>::of(encoder_.comap(from), decoder_.map(to), name_ + "[xmapped]");
  }

  template <class S>
  MapCodec<S> flatXmap(std::function<DataResult<S>(const A&)> to,
                       std::function<DataResult<A>(const S&)> from) const {
    return MapCodec<S>::of(encoder_.flatComap(from), decoder_.flatMap(to), name_ + "[flatXmapped]");
  }

  MapCodec<A> withLifecycle(const Lifecycle& lifecycle) const {
    return MapCodec<A>(encoder_.withLifecycle(lifecycle), decoder_.withLifecycle(lifecycle), name_);
  }

  MapCodec<A> stable() const { return withLifecycle(Lifecycle::stable()); }
  MapCodec<A> deprecated(int since) const { return withLifecycle(Lifecycle::deprecated(since)); }

  MapCodec<A> mapResult(const MapResultFunction<A>& function) const {
    const MapEncoder<A> encoder = encoder_;
    const MapDecoder<A> decoder = decoder_;
    return MapCodec<A>(
        MapEncoder<A>(
            [encoder, function](const A& input, const DynamicOps& ops,
                                RecordBuilder& prefix) -> RecordBuilder& {
              return function.coApply(ops, input, encoder.encode(input, ops, prefix));
            },
            [encoder](const DynamicOps& ops) { return encoder.keys(ops); }),
        MapDecoder<A>(
            [decoder, function](const DynamicOps& ops, const MapLike& input) {
              return function.apply(ops, input, decoder.decode(ops, input));
            },
            [decoder](const DynamicOps& ops) { return decoder.keys(ops); }),
        name_ + "[mapResult]");
  }

  MapCodec<A> orElse(A value) const {
    MapResultFunction<A> function;
    function.apply = [value](const DynamicOps&, const MapLike&, const DataResult<A>& result) {
      const std::optional<A> resolved = result.result();
      return DataResult<A>::success(resolved.has_value() ? *resolved : value);
    };
    function.coApply = [](const DynamicOps&, const A&, RecordBuilder& prefix) -> RecordBuilder& {
      return prefix;
    };
    return mapResult(function);
  }

  MapCodec<A> orElseGet(std::function<A()> supplier) const {
    MapResultFunction<A> function;
    function.apply = [supplier](const DynamicOps&, const MapLike&, const DataResult<A>& result) {
      const std::optional<A> resolved = result.result();
      return DataResult<A>::success(resolved.has_value() ? *resolved : supplier());
    };
    function.coApply = [](const DynamicOps&, const A&, RecordBuilder& prefix) -> RecordBuilder& {
      return prefix;
    };
    return mapResult(function);
  }

  MapCodec<A> setPartial(std::function<A()> supplier) const {
    MapResultFunction<A> function;
    function.apply = [supplier](const DynamicOps&, const MapLike&, const DataResult<A>& result) {
      return result.setPartialFrom(supplier);
    };
    function.coApply = [](const DynamicOps&, const A&, RecordBuilder& prefix) -> RecordBuilder& {
      return prefix;
    };
    return mapResult(function);
  }

  // MapCodec.unit——不读取任何内容就解码为 `value`。
  static MapCodec<A> unit(A value) {
    return MapCodec<A>::of(MapEncoder<A>::empty(), MapDecoder<A>::unit(std::move(value)),
                           "UnitMapCodec");
  }

  static MapCodec<A> unitOf(std::function<A()> supplier) {
    return MapCodec<A>::of(
        MapEncoder<A>::empty(),
        MapDecoder<A>(
            [supplier](const DynamicOps&, const MapLike&) {
              return DataResult<A>::success(supplier());
            },
            [](const DynamicOps&) { return std::vector<JsonValue>{}; }),
        "UnitMapCodec");
  }

 private:
  MapEncoder<A> encoder_;
  MapDecoder<A> decoder_;
  std::string name_;
};

// ===========================================================================
// Codec<A>（com.mojang.serialization.Codec）
// ===========================================================================
template <class A>
class Codec {
 public:
  using value_type = A;

  Codec() = default;
  Codec(Encoder<A> encoder, Decoder<A> decoder, std::string name)
      : encoder_(std::move(encoder)), decoder_(std::move(decoder)), name_(std::move(name)) {}

  // 从 MapCodec 隐式转换（DFU 的 MapCodec.MapCodecCodec）。
  Codec(const MapCodec<A>& mapCodec)  // NOLINT(google-explicit-constructor)
      : encoder_(mapCodec.encoder().encoder()),
        decoder_(mapCodec.decoder().decoder()),
        name_(mapCodec.name()),
        mapCodec_(std::make_shared<const MapCodec<A>>(mapCodec)) {}

  static Codec of(Encoder<A> encoder, Decoder<A> decoder, std::string name) {
    return Codec(std::move(encoder), std::move(decoder), std::move(name));
  }

  bool valid() const { return encoder_.valid() && decoder_.valid(); }

  const Encoder<A>& encoder() const { return encoder_; }
  const Decoder<A>& decoder() const { return decoder_; }
  const std::string& name() const { return name_; }

  // 当此 codec 由 MapCodec 支撑时非 null（供 dispatch 使用）。
  const MapCodec<A>* mapCodec() const { return mapCodec_.get(); }

  DataResult<std::pair<A, JsonValue>> decode(const DynamicOps& ops, const JsonValue& input) const {
    return decoder_.decode(ops, input);
  }

  DataResult<A> parse(const DynamicOps& ops, const JsonValue& input) const {
    return decoder_.parse(ops, input);
  }

  DataResult<JsonValue> encode(const A& input, const DynamicOps& ops,
                               const JsonValue& prefix) const {
    return encoder_.encode(input, ops, prefix);
  }

  DataResult<JsonValue> encodeStart(const DynamicOps& ops, const A& input) const {
    return encoder_.encodeStart(ops, input);
  }

  // --- 结构化字段 ---------------------------------------------------------
  MapCodec<A> fieldOf(const std::string& name) const {
    return MapCodec<A>::of(encoder_.fieldOf(name), decoder_.fieldOf(name),
                           "Field[" + name + ": " + name_ + "]");
  }

  MapCodec<std::optional<A>> optionalFieldOf(const std::string& name) const;

  MapCodec<A> optionalFieldOf(const std::string& name, A defaultValue) const;

  // 会传播错误的对应版本（新增）：存在但无效的值会连同其位置一起失败，
  // 而不是被丢弃。
  MapCodec<std::optional<A>> optionalFieldOfStrict(const std::string& name) const;

  MapCodec<A> optionalFieldOfStrict(const std::string& name, A defaultValue) const;

  Codec<std::vector<A>> listOf() const;

  // --- 映射 ---------------------------------------------------------------
  template <class S>
  Codec<S> xmap(std::function<S(const A&)> to, std::function<A(const S&)> from) const {
    return Codec<S>::of(encoder_.comap(from), decoder_.map(to), name_ + "[xmapped]");
  }

  template <class S>
  Codec<S> comapFlatMap(std::function<DataResult<S>(const A&)> to,
                        std::function<A(const S&)> from) const {
    return Codec<S>::of(encoder_.comap(from), decoder_.flatMap(to), name_ + "[comapFlatMapped]");
  }

  template <class S>
  Codec<S> flatComapMap(std::function<S(const A&)> to,
                        std::function<DataResult<A>(const S&)> from) const {
    return Codec<S>::of(encoder_.flatComap(from), decoder_.map(to), name_ + "[flatComapMapped]");
  }

  template <class S>
  Codec<S> flatXmap(std::function<DataResult<S>(const A&)> to,
                    std::function<DataResult<A>(const S&)> from) const {
    return Codec<S>::of(encoder_.flatComap(from), decoder_.flatMap(to), name_ + "[flatXmapped]");
  }

  // --- 生命周期 -----------------------------------------------------------
  Codec<A> withLifecycle(const Lifecycle& lifecycle) const {
    return Codec<A>(encoder_.withLifecycle(lifecycle), decoder_.withLifecycle(lifecycle), name_);
  }
  Codec<A> stable() const { return withLifecycle(Lifecycle::stable()); }
  Codec<A> deprecated(int since) const { return withLifecycle(Lifecycle::deprecated(since)); }

  Codec<A> promotePartial(const ErrorHandler& onError) const {
    return Codec<A>(encoder_, decoder_.promotePartial(onError), name_);
  }

  // --- 结果处理 -----------------------------------------------------------
  Codec<A> mapResult(const CodecResultFunction<A>& function) const {
    const Encoder<A> encoder = encoder_;
    const Decoder<A> decoder = decoder_;
    return Codec<A>(
        Encoder<A>([encoder, function](const A& input, const DynamicOps& ops,
                                       const JsonValue& prefix) {
          return function.coApply(ops, input, encoder.encode(input, ops, prefix));
        }),
        Decoder<A>([decoder, function](const DynamicOps& ops, const JsonValue& input) {
          return function.apply(ops, input, decoder.decode(ops, input));
        }),
        name_ + "[mapResult]");
  }

  Codec<A> orElse(A value) const {
    CodecResultFunction<A> function;
    function.apply = [value](const DynamicOps&, const JsonValue& input,
                             const DataResult<std::pair<A, JsonValue>>& result) {
      const std::optional<std::pair<A, JsonValue>> resolved = result.result();
      return DataResult<std::pair<A, JsonValue>>::success(
          resolved.has_value() ? *resolved : std::make_pair(value, input));
    };
    function.coApply = [](const DynamicOps&, const A&,
                          const DataResult<JsonValue>& result) { return result; };
    return mapResult(function);
  }

  Codec<A> orElseGet(std::function<A()> supplier) const {
    CodecResultFunction<A> function;
    function.apply = [supplier](const DynamicOps&, const JsonValue& input,
                                const DataResult<std::pair<A, JsonValue>>& result) {
      const std::optional<std::pair<A, JsonValue>> resolved = result.result();
      return DataResult<std::pair<A, JsonValue>>::success(
          resolved.has_value() ? *resolved : std::make_pair(supplier(), input));
    };
    function.coApply = [](const DynamicOps&, const A&,
                          const DataResult<JsonValue>& result) { return result; };
    return mapResult(function);
  }

  // --- dispatch（定义于 codecs.hpp） --------------------------------------
  // `where` 同样是默认实参：dispatch 帧由此点回用户构造它的那一行。
  template <class E, class TypeFn, class CodecFn>
  Codec<E> partialDispatch(const std::string& typeKey, TypeFn type, CodecFn codec,
                           SourceLocation where = SourceLocation::current()) const;

  template <class E, class TypeFn, class CodecFn>
  Codec<E> dispatch(const std::string& typeKey, TypeFn type, CodecFn codec,
                    SourceLocation where = SourceLocation::current()) const;

  template <class E, class TypeFn, class CodecFn>
  Codec<E> dispatch(TypeFn type, CodecFn codec,
                    SourceLocation where = SourceLocation::current()) const {
    return dispatch<E>(std::string("type"), std::move(type), std::move(codec), where);
  }

  template <class E, class TypeFn, class CodecFn>
  MapCodec<E> dispatchMap(const std::string& typeKey, TypeFn type, CodecFn codec,
                          SourceLocation where = SourceLocation::current()) const;

  template <class E, class TypeFn, class CodecFn>
  MapCodec<E> dispatchMap(TypeFn type, CodecFn codec,
                          SourceLocation where = SourceLocation::current()) const {
    return dispatchMap<E>(std::string("type"), std::move(type), std::move(codec), where);
  }

  // Codec.unit -- 一个不编码任何内容、解码为 `value` 的 codec。
  static Codec<A> unit(A value) { return MapCodec<A>::unit(std::move(value)).codec(); }

  // Codec.EMPTY -- 空操作的 map codec。
  static Codec<Unit> empty() {
    return MapCodec<Unit>::of(MapEncoder<Unit>::empty(),
                              MapDecoder<Unit>::unit(Unit{}), "EmptyCodec")
        .codec();
  }

 private:
  Encoder<A> encoder_;
  Decoder<A> decoder_;
  std::string name_;
  std::shared_ptr<const MapCodec<A>> mapCodec_;
};

// ===========================================================================
// 类外定义
// ===========================================================================
template <class A>
inline MapEncoder<A> Encoder<A>::empty() {
  return MapEncoder<A>::empty();
}

template <class A>
inline MapDecoder<A> Decoder<A>::unit(A value) {
  return MapDecoder<A>::unit(std::move(value));
}

template <class A>
inline MapEncoder<A> Encoder<A>::fieldOf(const std::string& name) const {
  const Encoder<A> self = *this;
  return MapEncoder<A>(
      [self, name](const A& input, const DynamicOps& ops, RecordBuilder& prefix) -> RecordBuilder& {
        return prefix.add(name, self.encodeStart(ops, input));
      },
      [name](const DynamicOps& ops) { return std::vector<JsonValue>{ops.createString(name)}; });
}

template <class A>
inline MapDecoder<A> Decoder<A>::fieldOf(const std::string& name) const {
  const Decoder<A> self = *this;
  return MapDecoder<A>(
      [self, name](const DynamicOps& ops, const MapLike& input) -> DataResult<A> {
        const std::optional<JsonValue> value = input.get(name);
        if (!value.has_value()) {
          // 这里也附上位置信息，这样缺失的键会读作
          // `risks[3].severity: No key severity in MapLike[...]`。
          return DataResult<A>::error("No key " + name + " in " + input.toString()).addPath(name);
        }
        return self.parse(ops, *value).addPath(name);
      },
      [name](const DynamicOps& ops) { return std::vector<JsonValue>{ops.createString(name)}; });
}

template <class A>
inline std::shared_ptr<RecordBuilder> MapEncoder<A>::compressedBuilder(
    const DynamicOps& ops) const {
  if (ops.compressMaps()) {
    return std::make_shared<CompressedRecordBuilder>(ops, KeyCompressor(ops, keys(ops)));
  }
  return ops.mapBuilder();
}

template <class A>
inline Codec<A> MapCodec<A>::codec() const {
  return Codec<A>(*this);
}

// optionalField(name, codec) -- MapCodec<Optional<A>> (OptionalFieldCodec)。
//
// 保留 DFU 的行为：存在但无效的值会被当作缺失，因此错误
// （及其位置）会被丢弃。如果你想要的不是这种行为，见下面的
// optionalFieldStrict。
template <class A>
MapCodec<std::optional<A>> optionalField(const std::string& name, Codec<A> elementCodec) {
  return MapCodec<std::optional<A>>::of(
      MapEncoder<std::optional<A>>(
          [name, elementCodec](const std::optional<A>& input, const DynamicOps& ops,
                              RecordBuilder& prefix) -> RecordBuilder& {
            if (!input.has_value()) {
              return prefix;
            }
            return prefix.add(name, elementCodec.encodeStart(ops, *input));
          },
          [name](const DynamicOps& ops) { return std::vector<JsonValue>{ops.createString(name)}; }),
      MapDecoder<std::optional<A>>(
          [name, elementCodec](const DynamicOps& ops, const MapLike& input) {
            const std::optional<JsonValue> value = input.get(name);
            if (!value.has_value()) {
              return DataResult<std::optional<A>>::success(std::optional<A>{});
            }
            const DataResult<A> parsed = elementCodec.parse(ops, *value);
            if (parsed.result().has_value()) {
              return DataResult<std::optional<A>>::success(std::optional<A>(*parsed.result()));
            }
            // 存在但无效的可选字段会被当作缺失。
            return DataResult<std::optional<A>>::success(std::optional<A>{});
          },
          [name](const DynamicOps& ops) { return std::vector<JsonValue>{ops.createString(name)}; }),
      "OptionalFieldCodec[" + name + ": " + elementCodec.name() + "]");
}

// optionalFieldStrict(name, codec) -- DFU 中没有对应实现的新增功能。
//
// 与 optionalField 形状相同，区别在于存在但无效的值会被视作
// *失败*，并携带该字段的位置，而不是被静默丢弃：
//
//     {"severity": 1}   ->  ok            (optionalField)
//                       ->  severity: Not a string: 1   (optionalFieldStrict)
//
// 凡是非法值不能被忽略的地方都可以用它——校验器、安全规则，
// 以及任何否则会把「字段被丢弃」解码成「字段没问题」的
// 场合。
template <class A>
MapCodec<std::optional<A>> optionalFieldStrict(const std::string& name, Codec<A> elementCodec,
                                              SourceLocation where = SourceLocation::current()) {
  return MapCodec<std::optional<A>>::of(
      MapEncoder<std::optional<A>>(
          [name, elementCodec](const std::optional<A>& input, const DynamicOps& ops,
                              RecordBuilder& prefix) -> RecordBuilder& {
            if (!input.has_value()) {
              return prefix;
            }
            return prefix.add(name, elementCodec.encodeStart(ops, *input));
          },
          [name](const DynamicOps& ops) { return std::vector<JsonValue>{ops.createString(name)}; }),
      MapDecoder<std::optional<A>>(
          [name, elementCodec, where](const DynamicOps& ops,
                                      const MapLike& input) -> DataResult<std::optional<A>> {
            const std::optional<JsonValue> value = input.get(name);
            if (!value.has_value()) {
              return DataResult<std::optional<A>>::success(std::optional<A>{});
            }
            DataResult<std::optional<A>> parsed =
                elementCodec.parse(ops, *value)
                    .map([](const A& value) { return std::optional<A>(value); });
            if (parsed.isError()) {
              parsed = parsed.addPath(name).addFrame("optional[" + name + "]", where);
            }
            return parsed;
          },
          [name](const DynamicOps& ops) { return std::vector<JsonValue>{ops.createString(name)}; }),
      "StrictOptionalFieldCodec[" + name + ": " + elementCodec.name() + "]");
}

template <class A>
inline MapCodec<std::optional<A>> Codec<A>::optionalFieldOf(const std::string& name) const {
  return optionalField(name, *this);
}

template <class A>
inline MapCodec<A> Codec<A>::optionalFieldOf(const std::string& name, A defaultValue) const {
  // DFU: optionalField(name, this).xmap(o -> o.orElse(defaultValue),
  //                                     a -> a.equals(defaultValue) ? empty() : of(a))
  return optionalField(name, *this).template xmap<A>(
      std::function<A(const std::optional<A>&)>([defaultValue](const std::optional<A>& value) {
        return value.has_value() ? *value : defaultValue;
      }),
      std::function<std::optional<A>(const A&)>([defaultValue](const A& value) {
        return value == defaultValue ? std::optional<A>{} : std::optional<A>(value);
      }));
}

// Codec::optionalFieldOfStrict -- 会传播错误的对应版本（新增）。
template <class A>
inline MapCodec<std::optional<A>> Codec<A>::optionalFieldOfStrict(const std::string& name) const {
  return optionalFieldStrict(name, *this);
}

template <class A>
inline MapCodec<A> Codec<A>::optionalFieldOfStrict(const std::string& name, A defaultValue) const {
  return optionalFieldStrict(name, *this).template xmap<A>(
      std::function<A(const std::optional<A>&)>([defaultValue](const std::optional<A>& value) {
        return value.has_value() ? *value : defaultValue;
      }),
      std::function<std::optional<A>(const A&)>([defaultValue](const A& value) {
        return value == defaultValue ? std::optional<A>{} : std::optional<A>(value);
      }));
}

}  // namespace codec


// ===========================================================================
// 7/8  codecs.hpp -- 基础类型 + 组合 codec、范围检查、递归、dispatch
// ===========================================================================

// Mojang DataFixerUpper `com.mojang.serialization` Codec API 的 C++17 移植。
//
// codecs.hpp -- 基础类型 codec（Codec.BOOL/INT/String/...），以及
// 组合 codec：ListCodec、EitherCodec、PairCodec、UnboundedMapCodec、
// KeyDispatchCodec、范围检查器，还有递归/惰性 codec 辅助工具。



namespace codec {

// ---------------------------------------------------------------------------
// Either<L, R> -- com.mojang.datafixers.util.Either
// ---------------------------------------------------------------------------
template <class L, class R>
class Either {
 public:
  Either() = default;

  static Either left(L value) {
    Either result;
    result.value_.template emplace<1>(std::move(value));
    return result;
  }
  static Either right(R value) {
    Either result;
    result.value_.template emplace<2>(std::move(value));
    return result;
  }

  bool isLeft() const { return value_.index() == 1; }
  bool isRight() const { return value_.index() == 2; }

  const L& left() const { return std::get<1>(value_); }
  const R& right() const { return std::get<2>(value_); }

  std::optional<L> leftValue() const {
    return isLeft() ? std::optional<L>(std::get<1>(value_)) : std::nullopt;
  }
  std::optional<R> rightValue() const {
    return isRight() ? std::optional<R>(std::get<2>(value_)) : std::nullopt;
  }

  template <class LeftFn, class RightFn>
  auto map(LeftFn leftFunction, RightFn rightFunction) const
      -> std::invoke_result_t<LeftFn, const L&> {
    return isLeft() ? leftFunction(std::get<1>(value_)) : rightFunction(std::get<2>(value_));
  }

  bool operator==(const Either& other) const { return value_ == other.value_; }
  bool operator!=(const Either& other) const { return !(*this == other); }

 private:
  std::variant<std::monostate, L, R> value_;
};

// ---------------------------------------------------------------------------
// 基础类型 codec -- PrimitiveCodec<A>
// ---------------------------------------------------------------------------
namespace detail {

// `where` 由默认实参在调用点取得：每个 codec() 单例的定义行，
// 于是 report() 里的 `Int` 帧能点回下面这行定义。
template <class A, class ReadFn, class WriteFn>
Codec<A> primitiveCodec(std::string name, ReadFn read, WriteFn write,
                        SourceLocation where = SourceLocation::current()) {
  Encoder<A> encoder([write, name, where](const A& input, const DynamicOps& ops,
                                          const JsonValue& prefix) {
    return ops.mergeToPrimitive(prefix, write(ops, input)).addFrame(name, where);
  });
  Decoder<A> decoder([read, name, where](const DynamicOps& ops, const JsonValue& input) {
    return read(ops, input)
        .map([&](const A& value) { return std::make_pair(value, ops.empty()); })
        .addFrame(name, where);
  });
  return Codec<A>::of(std::move(encoder), std::move(decoder), std::move(name));
}

}  // namespace detail

namespace codecs {

// Codec.BOOL
inline const Codec<bool> Bool = detail::primitiveCodec<bool>(
    "Bool", [](const DynamicOps& ops, const JsonValue& input) { return ops.getBooleanValue(input); },
    [](const DynamicOps& ops, const bool& value) { return ops.createBoolean(value); });

// Codec.BYTE
inline const Codec<int8_t> Byte = detail::primitiveCodec<int8_t>(
    "Byte",
    [](const DynamicOps& ops, const JsonValue& input) {
      return ops.getNumberValue(input).map([](const Number& n) { return n.byteValue(); });
    },
    [](const DynamicOps& ops, const int8_t& value) { return ops.createByte(value); });

// Codec.SHORT
inline const Codec<int16_t> Short = detail::primitiveCodec<int16_t>(
    "Short",
    [](const DynamicOps& ops, const JsonValue& input) {
      return ops.getNumberValue(input).map([](const Number& n) { return n.shortValue(); });
    },
    [](const DynamicOps& ops, const int16_t& value) { return ops.createShort(value); });

// Codec.INT
inline const Codec<int32_t> Int = detail::primitiveCodec<int32_t>(
    "Int",
    [](const DynamicOps& ops, const JsonValue& input) {
      return ops.getNumberValue(input).map([](const Number& n) { return n.intValue(); });
    },
    [](const DynamicOps& ops, const int32_t& value) { return ops.createInt(value); });

// Codec.LONG
inline const Codec<int64_t> Long = detail::primitiveCodec<int64_t>(
    "Long",
    [](const DynamicOps& ops, const JsonValue& input) {
      return ops.getNumberValue(input).map([](const Number& n) { return n.longValue(); });
    },
    [](const DynamicOps& ops, const int64_t& value) { return ops.createLong(value); });

// Codec.FLOAT
inline const Codec<float> Float = detail::primitiveCodec<float>(
    "Float",
    [](const DynamicOps& ops, const JsonValue& input) {
      return ops.getNumberValue(input).map([](const Number& n) { return n.floatValue(); });
    },
    [](const DynamicOps& ops, const float& value) { return ops.createFloat(value); });

// Codec.DOUBLE
inline const Codec<double> Double = detail::primitiveCodec<double>(
    "Double",
    [](const DynamicOps& ops, const JsonValue& input) {
      return ops.getNumberValue(input).map([](const Number& n) { return n.doubleValue(); });
    },
    [](const DynamicOps& ops, const double& value) { return ops.createDouble(value); });

// Codec.STRING
inline const Codec<std::string> String = detail::primitiveCodec<std::string>(
    "String",
    [](const DynamicOps& ops, const JsonValue& input) { return ops.getStringValue(input); },
    [](const DynamicOps& ops, const std::string& value) { return ops.createString(value); });

// Codec.PASSTHROUGH -- 把原始动态值原样透传。
inline const Codec<JsonValue> Passthrough = Codec<JsonValue>::of(
    Encoder<JsonValue>([](const JsonValue& input, const DynamicOps& ops, const JsonValue& prefix) {
      if (prefix == ops.empty()) {
        return DataResult<JsonValue>::success(input, Lifecycle::experimental());
      }
      if (input.isObject()) {
        const JsonObjectMapLike map(input);
        return ops.mergeToMap(prefix, map);
      }
      if (input.isArray()) {
        return ops.mergeToList(prefix, input.asArray());
      }
      return DataResult<JsonValue>::error(
          "Don't know how to merge " + prefix.dump() + " and " + input.dump(), prefix,
          Lifecycle::experimental());
    }),
    Decoder<JsonValue>([](const DynamicOps& ops, const JsonValue& input) {
      return DataResult<std::pair<JsonValue, JsonValue>>::success(
          std::make_pair(input, ops.empty()));
    }),
    "passthrough");

// Codec.EMPTY
inline const Codec<Unit> Empty = Codec<Unit>::empty();

// ---------------------------------------------------------------------------
// stringEnum -- DFU 中没有对应实现的新增功能
//
// 通过一张名字表在枚举与其 JSON *名字*之间双向映射，这正是
// Minecraft 用 StringRepresentable.fromEnum 做的事（那个辅助方法位于
// Minecraft，不在 DataFixerUpper 中，所以这里没有什么可移植的）。它用
// Codec::flatXmap 实现，因此像其他 codec 一样可组合：可以用于
// 字段、列表、dispatch、可选字段……
//
//     enum class Severity { Low, Medium, Critical };
//     const Codec<Severity> SeverityCodec = codecs::stringEnum<Severity>(
//         {{"low", Severity::Low},
//          {"medium", Severity::Medium},
//          {"critical", Severity::Critical}},
//         "Severity");
//
// 未知的名字会失败并给出 `Unknown Severity: "fatal"`；不在表中的值
// 编码时会失败并给出 `Unmapped Severity value`。`E` 必须支持
// 判等；该表会被复制一次，并由两个方向共享。
//
// 另一个方向——数字，或以数字序列化的枚举——不需要
// 辅助工具：`Int.xmap<E>(toEnum, toInt)`，或者在映射可能失败时用
// `Int.flatXmap<E>(...)`。四种组合见 test/unit/string_and_enum_test.cpp。
template <class E>
Codec<E> stringEnum(std::vector<std::pair<std::string, E>> values, std::string name = "enum") {
  const auto table =
      std::make_shared<const std::vector<std::pair<std::string, E>>>(std::move(values));
  return String.flatXmap<E>(
      [table, name](const std::string& text) -> DataResult<E> {
        for (const auto& entry : *table) {
          if (entry.first == text) {
            return DataResult<E>::success(entry.second);
          }
        }
        return DataResult<E>::error("Unknown " + name + ": \"" + text + "\"");
      },
      [table, name](const E& value) -> DataResult<std::string> {
        for (const auto& entry : *table) {
          if (entry.second == value) {
            return DataResult<std::string>::success(entry.first);
          }
        }
        return DataResult<std::string>::error("Unmapped " + name + " value");
      });
}

}  // namespace codecs

// ---------------------------------------------------------------------------
// ListCodec
// ---------------------------------------------------------------------------
template <class A>
Codec<std::vector<A>> listOf(const Codec<A>& elementCodec,
                             SourceLocation where = SourceLocation::current()) {
  Encoder<std::vector<A>> encoder([elementCodec, where](const std::vector<A>& input,
                                                        const DynamicOps& ops,
                                                        const JsonValue& prefix) {
    const std::shared_ptr<ListBuilder> builder = ops.listBuilder();
    int32_t index = 0;
    for (const A& element : input) {
      // 编码失败同样保留元素的位置。
      builder->add(elementCodec.encodeStart(ops, element).addPath(index));
      ++index;
    }
    return builder->build(prefix).addFrame("list", where);
  });

  Decoder<std::vector<A>> decoder(
      [elementCodec, where](const DynamicOps& ops,
                            const JsonValue& input)
          -> DataResult<std::pair<std::vector<A>, JsonValue>> {
    return ops.getList(input)
        .setLifecycle(Lifecycle::stable())
        .flatMap([&](const std::vector<JsonValue>& values)
                     -> DataResult<std::pair<std::vector<A>, JsonValue>> {
          std::vector<A> elements;
          std::vector<JsonValue> failed;
          DataResult<Unit> result = DataResult<Unit>::success(Unit{}, Lifecycle::stable());
          for (size_t i = 0; i < values.size(); ++i) {
            const JsonValue& value = values[i];
            // 失败的元素用它的下标定位：`or[0]: ...`。
            const DataResult<std::pair<A, JsonValue>> element =
                elementCodec.decode(ops, value).addPath(static_cast<int32_t>(i));
            if (element.isError()) {
              failed.push_back(value);
            }
            result = result.apply2stable(
                [&](const Unit& unit, const std::pair<A, JsonValue>& decoded) {
                  elements.push_back(decoded.first);
                  return unit;
                },
                element);
          }
          const JsonValue errors = ops.createList(failed);
          const std::pair<std::vector<A>, JsonValue> pair(elements, errors);
          return result.map([&](const Unit&) { return pair; })
              .setPartial(pair)
              .addFrame("list", where);
        });
  });

  return Codec<std::vector<A>>::of(Encoder<std::vector<A>>(std::move(encoder)),
                                   Decoder<std::vector<A>>(std::move(decoder)),
                                   "ListCodec[" + elementCodec.name() + "]");
}

template <class A>
inline Codec<std::vector<A>> Codec<A>::listOf() const {
  return codec::listOf(*this);
}

// ---------------------------------------------------------------------------
// EitherCodec
// ---------------------------------------------------------------------------
template <class F, class S>
Codec<Either<F, S>> either(const Codec<F>& first, const Codec<S>& second,
                           SourceLocation where = SourceLocation::current()) {
  Encoder<Either<F, S>> encoder([first, second](const Either<F, S>& input,
                                               const DynamicOps& ops,
                                               const JsonValue& prefix) {
    return input.isLeft() ? first.encode(input.left(), ops, prefix)
                          : second.encode(input.right(), ops, prefix);
  });

  Decoder<Either<F, S>> decoder([first, second, where](const DynamicOps& ops,
                                                       const JsonValue& input) {
    const DataResult<std::pair<Either<F, S>, JsonValue>> firstRead =
        first.decode(ops, input).map([](const std::pair<F, JsonValue>& pair) {
          return std::make_pair(Either<F, S>::left(pair.first), pair.second);
        });
    if (firstRead.result().has_value()) {
      return firstRead;
    }
    return second.decode(ops, input)
        .map([](const std::pair<S, JsonValue>& pair) {
          return std::make_pair(Either<F, S>::right(pair.first), pair.second);
        })
        .addFrame("either", where);
  });

  return Codec<Either<F, S>>::of(Encoder<Either<F, S>>(std::move(encoder)),
                                 Decoder<Either<F, S>>(std::move(decoder)),
                                 "EitherCodec[" + first.name() + ", " + second.name() + "]");
}

// ---------------------------------------------------------------------------
// PairCodec
// ---------------------------------------------------------------------------
template <class F, class S>
Codec<std::pair<F, S>> pair(const Codec<F>& first, const Codec<S>& second,
                            SourceLocation where = SourceLocation::current()) {
  Encoder<std::pair<F, S>> encoder([first, second](const std::pair<F, S>& value,
                                                  const DynamicOps& ops,
                                                  const JsonValue& rest) {
    return second.encode(value.second, ops, rest).flatMap(
        [&](const JsonValue& encoded) { return first.encode(value.first, ops, encoded); });
  });

  Decoder<std::pair<F, S>> decoder([first, second, where](const DynamicOps& ops,
                                                          const JsonValue& input) {
    return first.decode(ops, input)
        .flatMap([&](const std::pair<F, JsonValue>& p1) {
          return second.decode(ops, p1.second).map([&](const std::pair<S, JsonValue>& p2) {
            return std::make_pair(std::make_pair(p1.first, p2.first), p2.second);
          });
        })
        .addFrame("pair", where);
  });

  return Codec<std::pair<F, S>>::of(Encoder<std::pair<F, S>>(std::move(encoder)),
                                    Decoder<std::pair<F, S>>(std::move(decoder)),
                                    "PairCodec[" + first.name() + ", " + second.name() + "]");
}

// ---------------------------------------------------------------------------
// UnboundedMapCodec (BaseMapCodec)
// ---------------------------------------------------------------------------
template <class K, class V>
Codec<std::vector<std::pair<K, V>>> unboundedMap(const Codec<K>& keyCodec,
                                                 const Codec<V>& elementCodec,
                                                 SourceLocation where =
                                                     SourceLocation::current()) {
  using Entry = std::pair<K, V>;
  using Entries = std::vector<Entry>;

  Encoder<Entries> encoder([keyCodec, elementCodec, where](const Entries& input,
                                                           const DynamicOps& ops,
                                                           const JsonValue& prefix) {
    const std::shared_ptr<RecordBuilder> builder = ops.mapBuilder();
    for (const Entry& entry : input) {
      builder->add(keyCodec.encodeStart(ops, entry.first), elementCodec.encodeStart(ops, entry.second));
    }
    return builder->build(prefix).addFrame("unboundedMap", where);
  });

  Decoder<Entries> decoder([keyCodec, elementCodec, where](const DynamicOps& ops,
                                                           const JsonValue& input)
      -> DataResult<std::pair<Entries, JsonValue>> {
    return ops.getMap(input).setLifecycle(Lifecycle::stable()).flatMap(
        [&](const MapLikePtr& map) -> DataResult<std::pair<Entries, JsonValue>> {
          Entries elements;
          std::vector<std::pair<JsonValue, JsonValue>> failed;
          DataResult<Unit> result = DataResult<Unit>::success(Unit{}, Lifecycle::stable());
          for (const auto& entry : map->entries()) {
            // 用键定位出错的条目（非字符串键则用它的位置）。
            const bool stringKey = entry.first.isString();
            const std::string_view keyText = stringKey ? entry.first.asString() : std::string_view();
            const int32_t entryIndex = static_cast<int32_t>(failed.size());
            DataResult<K> key = keyCodec.parse(ops, entry.first);
            DataResult<V> value = elementCodec.parse(ops, entry.second);
            if (stringKey) {
              key = key.addPath(keyText);
              value = value.addPath(keyText);
            } else {
              key = key.addPath(entryIndex);
              value = value.addPath(entryIndex);
            }
            const DataResult<Entry> decoded = key.apply2stable(
                [](const K& k, const V& v) { return Entry(k, v); }, value);
            if (decoded.isError()) {
              failed.push_back(entry);
            }
            result = result.apply2stable(
                [&](const Unit& unit, const Entry& decodedEntry) {
                  // DFU 使用 ImmutableMap（它会拒绝重复键）；本移植
                  // 保留插入顺序，采用后写覆盖（last-wins）语义。
                  for (Entry& existing : elements) {
                    if (existing.first == decodedEntry.first) {
                      existing.second = decodedEntry.second;
                      return unit;
                    }
                  }
                  elements.push_back(decodedEntry);
                  return unit;
                },
                decoded);
          }
          const JsonValue errors = ops.createMap(failed);
          const std::pair<Entries, JsonValue> pair(elements, errors);
          return result.map([&](const Unit&) { return pair; })
              .setPartial(pair)
              .mapError([&](const std::string& message) {
                return message + " missed input: " + errors.dump();
              })
              .addFrame("unboundedMap", where);
        });
  });

  return Codec<Entries>::of(Encoder<Entries>(std::move(encoder)), Decoder<Entries>(std::move(decoder)),
                            "UnboundedMapCodec[" + keyCodec.name() + " -> " + elementCodec.name() + "]");
}

// ---------------------------------------------------------------------------
// 范围校验器（Codec.checkRange / intRange / floatRange / doubleRange）
// ---------------------------------------------------------------------------
inline Codec<int32_t> intRange(int32_t minInclusive, int32_t maxInclusive) {
  const auto checker = [minInclusive, maxInclusive](const int32_t& value) -> DataResult<int32_t> {
    if (value >= minInclusive && value <= maxInclusive) {
      return DataResult<int32_t>::success(value);
    }
    return DataResult<int32_t>::error("Value " + std::to_string(value) + " outside of range [" +
                                          std::to_string(minInclusive) + ":" +
                                          std::to_string(maxInclusive) + "]",
                                      value);
  };
  return codecs::Int.flatXmap<int32_t>(checker, checker);
}

inline Codec<float> floatRange(float minInclusive, float maxInclusive) {
  const auto checker = [minInclusive, maxInclusive](const float& value) -> DataResult<float> {
    if (value >= minInclusive && value <= maxInclusive) {
      return DataResult<float>::success(value);
    }
    return DataResult<float>::error("Value " + Number::ofDouble(value).toString() +
                                        " outside of range [" +
                                        Number::ofDouble(minInclusive).toString() + ":" +
                                        Number::ofDouble(maxInclusive).toString() + "]",
                                    value);
  };
  return codecs::Float.flatXmap<float>(checker, checker);
}

inline Codec<double> doubleRange(double minInclusive, double maxInclusive) {
  const auto checker = [minInclusive, maxInclusive](const double& value) -> DataResult<double> {
    if (value >= minInclusive && value <= maxInclusive) {
      return DataResult<double>::success(value);
    }
    return DataResult<double>::error("Value " + Number::ofDouble(value).toString() +
                                        " outside of range [" +
                                        Number::ofDouble(minInclusive).toString() + ":" +
                                        Number::ofDouble(maxInclusive).toString() + "]",
                                    value);
  };
  return codecs::Double.flatXmap<double>(checker, checker);
}

// ---------------------------------------------------------------------------
// 递归 / 惰性 codec
//
// DFU 把 codec 交给 datafixer 来表达递归 codec；在 C++ 中
// 最干净的等价物是一个首次使用时才求解的 supplier，它同时
// 打破了静态初始化循环。
// ---------------------------------------------------------------------------
template <class A>
Codec<A> recursive(std::function<Codec<A>()> supplier) {
  struct State {
    std::function<Codec<A>()> supplier;
    std::optional<Codec<A>> cached;
  };
  const auto state = std::make_shared<State>();
  state->supplier = std::move(supplier);

  const auto resolve = [state]() -> const Codec<A>& {
    if (!state->cached.has_value()) {
      state->cached = state->supplier();
    }
    return *state->cached;
  };

  Encoder<A> encoder([resolve](const A& input, const DynamicOps& ops, const JsonValue& prefix) {
    return resolve().encode(input, ops, prefix);
  });
  Decoder<A> decoder([resolve](const DynamicOps& ops, const JsonValue& input) {
    return resolve().decode(ops, input);
  });
  return Codec<A>::of(std::move(encoder), std::move(decoder), "RecursiveCodec");
}

// ---------------------------------------------------------------------------
// KeyDispatchCodec
// ---------------------------------------------------------------------------
template <class K, class V>
MapCodec<V> keyDispatchMapCodec(const std::string& typeKey, const Codec<K>& keyCodec,
                                std::function<DataResult<K>(const V&)> type,
                                std::function<DataResult<Codec<V>>(const K&)> codecSelector,
                                bool assumeMap, SourceLocation where = SourceLocation::current()) {
  const auto keys = [typeKey](const DynamicOps& ops) {
    return std::vector<JsonValue>{ops.createString(typeKey), ops.createString("value")};
  };

  const auto selectCodec = [type, codecSelector](const V& input) -> DataResult<Codec<V>> {
    return type(input).flatMap([&](const K& key) -> DataResult<Codec<V>> {
      return codecSelector(key);
    });
  };

  MapEncoder<V> encoder(
      [type, selectCodec, typeKey, keyCodec, assumeMap](const V& input, const DynamicOps& ops,
                                                        RecordBuilder& prefix) -> RecordBuilder& {
        const DataResult<Codec<V>> elementCodec = selectCodec(input);
        RecordBuilder& builder = prefix.withErrorsFrom(elementCodec);
        if (!elementCodec.result().has_value()) {
          return builder;
        }
        const Codec<V> codec = *elementCodec.result();
        const DataResult<JsonValue> typeResult = type(input).flatMap([&](const K& key) {
          return keyCodec.encodeStart(ops, key);
        });
        if (ops.compressMaps()) {
          prefix.add(typeKey, typeResult);
          prefix.add("value", codec.encodeStart(ops, input));
          return prefix;
        }
        if (codec.mapCodec() != nullptr) {
          codec.mapCodec()->encode(input, ops, prefix);
          prefix.add(typeKey, typeResult);
          return prefix;
        }
        const JsonValue typeString = ops.createString(typeKey);
        const DataResult<JsonValue> result = codec.encodeStart(ops, input);
        if (assumeMap) {
          const DataResult<MapLikePtr> element =
              result.flatMap([&](const JsonValue& value) { return ops.getMap(value); });
          if (!element.result().has_value()) {
            return prefix.withErrorsFrom(element);
          }
          prefix.add(typeString, typeResult);
          const std::vector<std::pair<JsonValue, JsonValue>> entries =
              (*element.result())->entries();
          for (const auto& entry : entries) {
            if (!(entry.first == typeString)) {
              prefix.add(entry.first, entry.second);
            }
          }
          return prefix;
        }
        prefix.add(typeString, typeResult);
        prefix.add("value", result);
        return prefix;
      },
      keys);

  MapDecoder<V> decoder(
      [typeKey, keyCodec, codecSelector, assumeMap, where](const DynamicOps& ops,
                                                           const MapLike& input) -> DataResult<V> {
        const std::optional<JsonValue> elementName = input.get(typeKey);
        if (!elementName.has_value()) {
          return DataResult<V>::error("Input does not contain a key [" + typeKey + "]: " +
                                      input.toString());
        }
        DataResult<V> decodedResult = keyCodec.decode(ops, *elementName)
            .flatMap([&](const std::pair<K, JsonValue>& decoded) -> DataResult<V> {
              return codecSelector(decoded.first)
                  .flatMap([&](const Codec<V>& codec) -> DataResult<V> {
                    if (ops.compressMaps()) {
                      const std::optional<JsonValue> value = input.get(ops.createString("value"));
                      if (!value.has_value()) {
                        return DataResult<V>::error("Input does not have a \"value\" entry: " +
                                                    input.toString())
                            .addPath("value");
                      }
                      // 压缩后的 dispatch 的载荷位于 "value" 之下。
                      return codec.parse(ops, *value).addPath("value");
                    }
                    if (codec.mapCodec() != nullptr) {
                      return codec.mapCodec()->decode(ops, input);
                    }
                    if (assumeMap) {
                      return codec.decode(ops, ops.createMap(input.entries()))
                          .map([](const std::pair<V, JsonValue>& pair) { return pair.first; });
                    }
                    const std::optional<JsonValue> value = input.get("value");
                    if (!value.has_value()) {
                      return DataResult<V>::error("Input does not have a \"value\" entry: " +
                                                  input.toString())
                          .addPath("value");
                    }
                    return codec.parse(ops, *value).addPath("value");
                  });
            });
        if (decodedResult.isError()) {
          decodedResult = decodedResult.addFrame("dispatch[" + typeKey + "]", where);
        }
        return decodedResult;
      },
      keys);

  return MapCodec<V>::of(std::move(encoder), std::move(decoder),
                         "KeyDispatchCodec[" + keyCodec.name() + "]");
}

template <class A>
template <class E, class TypeFn, class CodecFn>
Codec<E> Codec<A>::partialDispatch(const std::string& typeKey, TypeFn type, CodecFn codec,
                                   SourceLocation where) const {
  return keyDispatchMapCodec<A, E>(
             typeKey, *this,
             [type](const E& value) -> DataResult<A> { return type(value); },
             [codec](const A& key) -> DataResult<Codec<E>> { return codec(key); }, false, where)
      .codec();
}

template <class A>
template <class E, class TypeFn, class CodecFn>
Codec<E> Codec<A>::dispatch(const std::string& typeKey, TypeFn type, CodecFn codec,
                            SourceLocation where) const {
  return partialDispatch<E>(
      typeKey, [type](const E& value) { return DataResult<A>::success(type(value)); },
      [codec](const A& key) { return DataResult<Codec<E>>::success(codec(key)); }, where);
}

template <class A>
template <class E, class TypeFn, class CodecFn>
MapCodec<E> Codec<A>::dispatchMap(const std::string& typeKey, TypeFn type, CodecFn codec,
                                  SourceLocation where) const {
  return keyDispatchMapCodec<A, E>(
      typeKey, *this, [type](const E& value) { return DataResult<A>::success(type(value)); },
      [codec](const A& key) { return DataResult<Codec<E>>::success(codec(key)); }, false, where);
}

}  // namespace codec


// ===========================================================================
// 8/8  record_codec.hpp -- RecordCodecBuilder：record<>、fieldOf、optionalFieldOf、forGetter
// ===========================================================================

// Mojang DataFixerUpper com.mojang.serialization Codec API 的 C++17 移植。
//
// record_codec.hpp -- RecordCodecBuilder 的移植：ieldOf、
// optionalFieldOf、orGetter、MapCodec::forGetter 以及变参的
// ecord<O>(...) 构建器。
//
// DFU：
//     RecordCodecBuilder.create(instance -> instance.group(
//             Codec.STRING.fieldOf("id").forGetter(RiskDef::id),
//             ...)
//         .apply(instance, RiskDef::new));
//
// C++17：
//     Codec<RiskDef> codec = record<RiskDef>(
//             fieldOf("id", &RiskDef::id, codecs::String),
//             ...);
//
// 字段有两种形态，与 pply 所能接受的内容相对应：
//   * RecordField<O, F>  -- 同时有 getter *和* setter（由成员指针或显式
//     setter 构建）；ecord<O>(fields...) 通过默认构造 O 再逐字段赋值来构造它。
//   * GetterField<O, F>  -- 只有 getter；需要构造器形式
//     ecord<O>(ctor, fields...)。
//     `record<O>(ctor, fields...)`.



namespace codec {

// ---------------------------------------------------------------------------
// 字段类型
// ---------------------------------------------------------------------------
template <class O, class F>
class GetterField {
 public:
  using object_type = O;
  using value_type = F;

  GetterField() = default;
  GetterField(std::string name, std::function<F(const O&)> getter, MapCodec<F> codec,
              SourceLocation where = {})
      : name_(std::move(name)),
        getter_(std::move(getter)),
        codec_(std::move(codec)),
        where_(where) {}

  const std::string& name() const { return name_; }
  F get(const O& object) const { return getter_(object); }
  const MapCodec<F>& codec() const { return codec_; }
  // 该字段的构造位置；record 帧用它点回 record<O>(...) 那一行
  // （可变参数调用无法捕获自身的调用点，所以用第一个字段的位置）。
  SourceLocation where() const { return where_; }

 private:
  std::string name_;
  std::function<F(const O&)> getter_;
  MapCodec<F> codec_;
  SourceLocation where_;
};

template <class O, class F>
class RecordField {
 public:
  using object_type = O;
  using value_type = F;

  RecordField() = default;
  RecordField(std::string name, std::function<F(const O&)> getter,
              std::function<void(O&, F)> setter, MapCodec<F> codec,
              SourceLocation where = {})
      : name_(std::move(name)),
        getter_(std::move(getter)),
        setter_(std::move(setter)),
        codec_(std::move(codec)),
        where_(where) {}

  const std::string& name() const { return name_; }
  F get(const O& object) const { return getter_(object); }
  void set(O& object, F value) const { setter_(object, std::move(value)); }
  const MapCodec<F>& codec() const { return codec_; }
  const std::function<void(O&, F)>& setter() const { return setter_; }
  // 见 GetterField::where()。
  SourceLocation where() const { return where_; }

 private:
  std::string name_;
  std::function<F(const O&)> getter_;
  std::function<void(O&, F)> setter_;
  MapCodec<F> codec_;
  SourceLocation where_;
};

template <class T>
struct is_field : std::false_type {};
template <class O, class F>
struct is_field<RecordField<O, F>> : std::true_type {};
template <class O, class F>
struct is_field<GetterField<O, F>> : std::true_type {};

template <class T>
struct is_record_field : std::false_type {};
template <class O, class F>
struct is_record_field<RecordField<O, F>> : std::true_type {};

// ---------------------------------------------------------------------------
// fieldOf / optionalFieldOf
// ---------------------------------------------------------------------------

// `codecs::String.fieldOf("id").forGetter(&RiskDef::id)`
template <class F>
class FieldBuilder {
 public:
  FieldBuilder(std::string name, Codec<F> codec, SourceLocation where = {})
      : name_(std::move(name)), codec_(std::move(codec)), where_(where) {}

  MapCodec<F> map() const { return codec_.fieldOf(name_); }

  template <class O>
  RecordField<O, F> forGetter(F O::*member) const {
    return RecordField<O, F>(
        name_, [member](const O& object) { return object.*member; },
        [member](O& object, F value) { object.*member = std::move(value); },
        codec_.fieldOf(name_), where_);
  }

  template <class O>
  GetterField<O, F> forGetter(std::function<F(const O&)> getter) const {
    return GetterField<O, F>(name_, std::move(getter), codec_.fieldOf(name_), where_);
  }

 private:
  std::string name_;
  Codec<F> codec_;
  SourceLocation where_;
};

// fieldOf(name, codec) -- 一个 MapCodec<F> 字段构建器。
template <class F>
FieldBuilder<F> fieldOf(const std::string& name, const Codec<F>& codec,
                        SourceLocation where = SourceLocation::current()) {
  return FieldBuilder<F>(name, std::move(codec), where);
}

// fieldOf(name, member, codec) -- 一个可设置字段。
template <class O, class F>
RecordField<O, F> fieldOf(const std::string& name, F O::*member, const Codec<F>& codec,
                          SourceLocation where = SourceLocation::current()) {
  return fieldOf(name, std::move(codec), where).forGetter(member);
}

// optionalFieldOf(name, codec) -- 一个 std::optional<F> 的字段构建器。
template <class F>
FieldBuilder<std::optional<F>> optionalFieldOf(const std::string& name, const Codec<F>& codec,
                                               SourceLocation where = SourceLocation::current()) {
  return FieldBuilder<std::optional<F>>(name, optionalField(name, std::move(codec)), where);
}

// optionalFieldOf(name, member, codec) -- member 是一个 std::optional<F>。
template <class O, class F>
RecordField<O, std::optional<F>> optionalFieldOf(const std::string& name,
                                                 std::optional<F> O::*member,
                                                 const Codec<F>& codec,
                                                 SourceLocation where = SourceLocation::current()) {
  return RecordField<O, std::optional<F>>(
      name, [member](const O& object) { return object.*member; },
      [member](O& object, std::optional<F> value) { object.*member = std::move(value); },
      optionalField(name, std::move(codec)), where);
}

// optionalFieldOf(name, member, codec, defaultValue) -- 缺失的成员会解码为
// defaultValue，而等于该值的成员不会被编码（DFU 的
// Codec.optionalFieldOf(String, A)）。
template <class O, class F>
RecordField<O, F> optionalFieldOf(const std::string& name, F O::*member, const Codec<F>& codec,
                                  F defaultValue, SourceLocation where = SourceLocation::current()) {
  return RecordField<O, F>(
      name, [member](const O& object) { return object.*member; },
      [member](O& object, F value) { object.*member = std::move(value); },
      codec.optionalFieldOf(name, std::move(defaultValue)), where);
}

// optionalFieldOfStrict(name, member, codec) -- 新增：类似 optionalFieldOf，
// 但值存在却非法时会带着其位置失败，而不是被静默视为缺失。
// 校验时优先使用它（见 optionalFieldStrict）。
template <class O, class F>
RecordField<O, std::optional<F>> optionalFieldOfStrict(
    const std::string& name, std::optional<F> O::*member, const Codec<F>& codec,
    SourceLocation where = SourceLocation::current()) {
  return RecordField<O, std::optional<F>>(
      name, [member](const O& object) { return object.*member; },
      [member](O& object, std::optional<F> value) { object.*member = std::move(value); },
      optionalFieldStrict(name, codec, where), where);
}

// optionalFieldOfStrict(name, member, codec, defaultValue) -- 新增：缺失的
// 成员会解码为 defaultValue，但值存在却非法则是一个错误。
template <class O, class F>
RecordField<O, F> optionalFieldOfStrict(const std::string& name, F O::*member,
                                        const Codec<F>& codec, F defaultValue,
                                        SourceLocation where = SourceLocation::current()) {
  return RecordField<O, F>(
      name, [member](const O& object) { return object.*member; },
      [member](O& object, F value) { object.*member = std::move(value); },
      codec.optionalFieldOfStrict(name, std::move(defaultValue)), where);
}

// MapCodec.forGetter -- DFU 的 mapCodec.forGetter(getter)。
template <class A>
template <class O>
inline GetterField<O, A> MapCodec<A>::forGetter(std::function<A(const O&)> getter) const {
  return GetterField<O, A>("<map>", std::move(getter), *this, SourceLocation::current());
}

// ---------------------------------------------------------------------------
// record<O>(...)
// ---------------------------------------------------------------------------
namespace detail {

struct ResultSummary {
  Lifecycle lifecycle;
  bool allSuccess = true;
  bool allValues = true;
  std::vector<ErrorPart> errors;
};

template <class... Rs>
ResultSummary summarizeResults(const Lifecycle& base,
                               const std::tuple<DataResult<Rs>...>& results) {
  ResultSummary summary{base, true, true, {}};
  const auto step = [&summary](const DataResultBase& result) {
    summary.lifecycle = summary.lifecycle.add(result.lifecycle());
    summary.allSuccess = summary.allSuccess && result.isSuccess();
    summary.allValues = summary.allValues && result.hasValue();
    if (result.isError()) {
      summary.errors.insert(summary.errors.end(), result.errors().begin(), result.errors().end());
    }
  };
  std::apply([&](const DataResult<Rs>&... result) {
    (void)std::initializer_list<int>{(step(result), 0)...};
  }, results);
  return summary;
}

template <class O, class FieldsTuple, std::size_t... I>
auto makeSetters(const FieldsTuple& fields, std::index_sequence<I...>) {
  return std::make_tuple(std::get<I>(fields).setter()...);
}

template <class O, class Setters, class Values, std::size_t... I>
void applySettersImpl(O& object, const Setters& setters, const Values& values,
                      std::index_sequence<I...>) {
  (void)std::initializer_list<int>{(std::get<I>(setters)(object, std::get<I>(values)), 0)...};
}

template <class O, class Setters, class... Vs>
void applySetters(O& object, const Setters& setters, const Vs&... values) {
  applySettersImpl(object, setters, std::tuple<const Vs&...>(values...),
                   std::make_index_sequence<sizeof...(Vs)>{});
}

template <class O, class Ctor, class Tuple, std::size_t... I>
O buildFromResults(const Ctor& ctor, const Tuple& results, std::index_sequence<I...>) {
  return ctor(*std::get<I>(results).valueOrPartial()...);
}

inline void appendKeys(std::vector<JsonValue>& out, std::vector<JsonValue> keys) {
  out.insert(out.end(), std::make_move_iterator(keys.begin()), std::make_move_iterator(keys.end()));
}

template <class FieldsTuple, std::size_t... I>
std::vector<JsonValue> fieldEncoderKeys(const FieldsTuple& fields, const DynamicOps& ops,
                                        std::index_sequence<I...>) {
  std::vector<JsonValue> out;
  (void)std::initializer_list<int>{
      (appendKeys(out, std::get<I>(fields).codec().encoder().keys(ops)), 0)...};
  return out;
}

template <class FieldsTuple, std::size_t... I>
std::vector<JsonValue> fieldDecoderKeys(const FieldsTuple& fields, const DynamicOps& ops,
                                        std::index_sequence<I...>) {
  std::vector<JsonValue> out;
  (void)std::initializer_list<int>{
      (appendKeys(out, std::get<I>(fields).codec().decoder().keys(ops)), 0)...};
  return out;
}

template <class FieldsTuple, std::size_t... I>
std::string joinFieldNames(const FieldsTuple& fields, std::index_sequence<I...>) {
  std::string out;
  (void)std::initializer_list<int>{
      (out += (out.empty() ? "" : ", "), out += std::get<I>(fields).name(), 0)...};
  return out;
}

// record 帧的源码位置。可变参数函数无法捕获自己的调用点，因此取第一个
// 字段的构造位置：`record<O>(...)` 的参数都写在同一处，
// 行号相同或只差一行。
template <class... Fields>
SourceLocation recordLocation(const std::tuple<Fields...>& fields) {
  if constexpr (sizeof...(Fields) == 0) {
    return SourceLocation{};
  } else {
    return std::get<0>(fields).where();
  }
}

// 把每个字段按声明顺序编码进同一个 RecordBuilder。
template <class O, class FieldsTuple, std::size_t... I>
RecordBuilder& encodeFields(const FieldsTuple& fields, const O& input, const DynamicOps& ops,
                            RecordBuilder& prefix, std::index_sequence<I...>) {
  (void)std::initializer_list<int>{
      (std::get<I>(fields).codec().encode(std::get<I>(fields).get(input), ops, prefix), 0)...};
  return prefix;
}

}  // namespace detail

// record<O>(fields...) -- 所有字段都必须可设置，O 必须可默认
// 构造（DFU：RecordCodecBuilder.create(instance -> instance.group(...)
// .apply(instance, O::new))）。
template <class O, class... Fields,
          class = std::enable_if_t<(is_record_field<Fields>::value && ...)>,
          class = std::enable_if_t<(std::is_same<typename Fields::object_type, O>::value && ...)>>
MapCodec<O> record(Fields... fields) {
  static_assert(std::is_default_constructible<O>::value,
                "record<O>(fields...) needs a default constructible O; use "
                "record<O>(constructor, fields...) instead");
  const auto fieldsTuple = std::make_tuple(fields...);
  const auto setters = detail::makeSetters<O>(fieldsTuple, std::index_sequence_for<Fields...>{});

  const auto ctor = [setters](const typename Fields::value_type&... values) -> O {
    O object{};
    detail::applySetters(object, setters, values...);
    return object;
  };

  MapEncoder<O> encoder(
      [fieldsTuple](const O& input, const DynamicOps& ops, RecordBuilder& prefix) -> RecordBuilder& {
        return detail::encodeFields(fieldsTuple, input, ops, prefix,
                                    std::index_sequence_for<Fields...>{});
      },
      [fieldsTuple](const DynamicOps& ops) {
        return detail::fieldEncoderKeys(fieldsTuple, ops, std::index_sequence_for<Fields...>{});
      });

  const std::string codecName =
      "RecordCodec[" +
      detail::joinFieldNames(fieldsTuple, std::index_sequence_for<Fields...>{}) + "]";
  const SourceLocation where = detail::recordLocation(fieldsTuple);

  MapDecoder<O> decoder(
      [fieldsTuple, ctor, codecName, where](const DynamicOps& ops,
                                            const MapLike& input) -> DataResult<O> {
        const std::tuple<DataResult<typename Fields::value_type>...> results =
            std::apply(
                [&](const auto&... field) {
                  return std::tuple<DataResult<typename Fields::value_type>...>(
                      field.codec().decode(ops, input)...);
                },
                fieldsTuple);
        const detail::ResultSummary summary =
            detail::summarizeResults(Lifecycle::experimental(), results);
        if (summary.allSuccess || summary.allValues) {
          O built = detail::buildFromResults<O>(ctor, results, std::index_sequence_for<Fields...>{});
          if (summary.allSuccess) {
            return DataResult<O>::success(std::move(built), summary.lifecycle);
          }
          return DataResult<O>::errorParts(summary.errors, std::move(built), summary.lifecycle)
              .addFrame(codecName, where);
        }
        return DataResult<O>::errorParts(summary.errors, std::nullopt, summary.lifecycle)
            .addFrame(codecName, where);
      },
      [fieldsTuple](const DynamicOps& ops) {
        return detail::fieldEncoderKeys(fieldsTuple, ops, std::index_sequence_for<Fields...>{});
      });

  return MapCodec<O>::of(std::move(encoder), std::move(decoder), codecName);
}

// record<O>(constructor, fields...) -- DFU 的 `apply(instance, ctor)` 形式；当类型
// 没有默认构造函数、或字段只有 getter 时使用它。
template <class O, class Ctor, class... Fields,
          class = std::enable_if_t<(is_field<Fields>::value && ...)>,
          class = std::enable_if_t<(std::is_same<typename Fields::object_type, O>::value && ...)>,
          class = std::enable_if_t<
              std::is_invocable<Ctor, typename Fields::value_type...>::value>>
MapCodec<O> record(Ctor ctor, Fields... fields) {
  const auto fieldsTuple = std::make_tuple(fields...);
  const auto constructor = std::move(ctor);

  MapEncoder<O> encoder(
      [fieldsTuple](const O& input, const DynamicOps& ops, RecordBuilder& prefix) -> RecordBuilder& {
        return detail::encodeFields(fieldsTuple, input, ops, prefix,
                                    std::index_sequence_for<Fields...>{});
      },
      [fieldsTuple](const DynamicOps& ops) {
        return detail::fieldEncoderKeys(fieldsTuple, ops, std::index_sequence_for<Fields...>{});
      });

  const std::string codecName =
      "RecordCodec[" +
      detail::joinFieldNames(fieldsTuple, std::index_sequence_for<Fields...>{}) + "]";

  const SourceLocation where = detail::recordLocation(fieldsTuple);

  MapDecoder<O> decoder(
      [fieldsTuple, constructor, codecName, where](const DynamicOps& ops,
                                                   const MapLike& input) -> DataResult<O> {
        const std::tuple<DataResult<typename Fields::value_type>...> results =
            std::apply(
                [&](const auto&... field) {
                  return std::tuple<DataResult<typename Fields::value_type>...>(
                      field.codec().decode(ops, input)...);
                },
                fieldsTuple);
        const detail::ResultSummary summary =
            detail::summarizeResults(Lifecycle::experimental(), results);
        if (summary.allSuccess || summary.allValues) {
          O built = detail::buildFromResults<O>(constructor, results,
                                                std::index_sequence_for<Fields...>{});
          if (summary.allSuccess) {
            return DataResult<O>::success(std::move(built), summary.lifecycle);
          }
          return DataResult<O>::errorParts(summary.errors, std::move(built), summary.lifecycle)
              .addFrame(codecName, where);
        }
        return DataResult<O>::errorParts(summary.errors, std::nullopt, summary.lifecycle)
            .addFrame(codecName, where);
      },
      [fieldsTuple](const DynamicOps& ops) {
        return detail::fieldDecoderKeys(fieldsTuple, ops, std::index_sequence_for<Fields...>{});
      });

  return MapCodec<O>::of(std::move(encoder), std::move(decoder), codecName);
}

// recordCodec<O>(...) -- 相同的构建器，但直接返回一个 Codec
// （DFU 的 RecordCodecBuilder.create）。
template <class O, class... Args>
Codec<O> recordCodec(Args&&... args) {
  return record<O>(std::forward<Args>(args)...).codec();
}

}  // namespace codec

