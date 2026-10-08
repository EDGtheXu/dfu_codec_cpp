// A C++17 port of Mojang's DataFixerUpper `com.mojang.serialization` Codec API.
//
//                              codec.hpp
//                  single-header, header-only library
//
// A faithful port of the Codec / MapCodec / RecordCodecBuilder / DynamicOps /
// DataResult mechanism from DataFixerUpper 6.0.8 (com.mojang:datafixerupper),
// the machinery Minecraft uses to serialise and deserialise everything from
// registry entries to world data.
//
// Usage
// -----
//     #include "codec.hpp"
//
// Everything lives in `namespace codec`; the CMake target is `codec`
// (alias `codec::codec`), an INTERFACE library, so there is nothing to build.
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
// Dependencies
// ------------
//   * C++17
//   * nlohmann/json 3.12.0 -- the JSON parser/serializer/DOM behind JsonValue
//     (only the JsonValue section below uses it; the codec layer never sees it).
//     Fetch it with scripts/fetch_deps.ps1 into third_party/.
//
// Contents
// --------
// Sections appear in dependency order and are named after the per-area headers
// this single file replaces; comments such as "see dynamic_ops.hpp" refer to the
// section of that name below.
//
//   1. json          JsonValue (nlohmann-backed) + Number (java.lang.Number)
//   2. lifecycle     Lifecycle
//   3. data_result   DataResult, PartialResult semantics, Unit
//   4. dynamic_ops   DynamicOps, MapLike, RecordBuilder, ListBuilder, KeyCompressor
//   5. json_ops      JsonOps (INSTANCE / COMPRESSED)
//   6. codec         Encoder, Decoder, MapEncoder, MapDecoder, Codec, MapCodec
//   7. codecs        primitive + composite codecs, range checks, recursive, dispatch
//   8. record_codec  RecordCodecBuilder: record<>, fieldOf, optionalFieldOf, forGetter
//
// Behaviour, deliberate deviations from DFU and the test layers are documented in
// README.md (README_zh.md).  The reference Java sources are not part of the
// library; scripts/decompile_reference.ps1 reproduces them for study.

#pragma once

// --- standard library -------------------------------------------------------
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

// --- third-party ------------------------------------------------------------
#include <nlohmann/json.hpp>

// ===========================================================================
// 1/8  json.hpp -- JsonValue (nlohmann-backed) + Number (java.lang.Number)
// ===========================================================================

// A C++17 port of Mojang's DataFixerUpper `com.mojang.serialization` Codec API.
//
// json.hpp -- the dynamic value type used as the serialized form ("T" in DFU's
// DynamicOps<T>) plus the numeric wrapper mirroring java.lang.Number.
//
// In DataFixerUpper the serialized value type is Gson's JsonElement (see
// JsonOps).  This port stores a nlohmann/json document instead
// (<https://github.com/nlohmann/json>), specifically nlohmann::ordered_json, so
// that objects keep their insertion order exactly like Gson's
// LinkedTreeMap-backed JsonObject and that member assignment replaces in place.
//
// `JsonValue` is a thin reference-semantics facade over nlohmann::ordered_json:
//   * the codec layer talks to DynamicOps/JsonValue and never to nlohmann,
//   * applications can hand a nlohmann document straight in (JsonValue has an
//     implicit constructor from nlohmann::ordered_json) and get it back with
//     raw(), so the parser and serializer are nlohmann's.



namespace codec {

// ---------------------------------------------------------------------------
// Number -- mirrors java.lang.Number as used by DynamicOps.getNumberValue(),
// DynamicOps.createNumeric() and the primitive codecs (`Number::intValue()`).
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
  // Java semantics: narrowing truncation of the (already truncated) long value.
  int32_t intValue() const { return static_cast<int32_t>(longValue()); }
  int16_t shortValue() const { return static_cast<int16_t>(longValue()); }
  int8_t byteValue() const { return static_cast<int8_t>(longValue()); }
  float floatValue() const { return integral_ ? static_cast<float>(i_) : static_cast<float>(d_); }
  double doubleValue() const { return integral_ ? static_cast<double>(i_) : d_; }
  bool booleanValue() const { return byteValue() != 0; }

  std::string toString() const;

  // Gson's JsonPrimitive numeric equality: integral/integral compares long
  // values, otherwise compares doubles.
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

// Shortest decimal representation that round-trips, without relying on
// std::to_chars(double) (which C++17 does not require).
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
  // Keep the value recognisable as a floating point literal (Gson prints 1.0).
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
// JsonValue -- the dynamic serialized value (DFU's "JsonElement")
//
// Backed by nlohmann::ordered_json: parsing, serialisation and the DOM are
// nlohmann/json's; the facade keeps the small API the codec layer needs
// (Gson-like typed accessors, null filtering, ordered member access and
// Gson-style equality).
//
// Like Gson's JsonElement, JsonValue has *reference* semantics: a handle owns the
// document it was created from (shared_ptr) and points at one node inside it, so
// copying a handle, reading a member or walking an array is O(1) and never
// duplicates the DOM.  Values are immutable once built.
// ---------------------------------------------------------------------------
class JsonValue {
 public:
  enum class Type { Null, Boolean, Number, String, Array, Object };
  using Raw = nlohmann::ordered_json;
  using Array = std::vector<JsonValue>;
  using Object = std::vector<std::pair<std::string, JsonValue>>;

  JsonValue();
  JsonValue(const Raw& raw);  // NOLINT(google-explicit-constructor) -- copies into a new node
  JsonValue(Raw&& raw);       // NOLINT(google-explicit-constructor) -- takes ownership

  // Zero-copy view: `owner` keeps the document alive and `node` must point at a
  // node inside it (or at the root).  Used internally to hand out members and
  // elements without copying; also handy to wrap a large nlohmann document once
  // and then navigate it through cheap handles.
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

  // Parses strict JSON with nlohmann's parser; malformed input raises
  // JsonParseError (wrapping nlohmann::json::parse_error).
  static JsonValue parse(std::string_view text);

  // The nlohmann node this handle refers to, for applications that want to work
  // with nlohmann/json directly.
  const Raw& raw() const { return *node_; }

  Type type() const;
  bool isNull() const { return type() == Type::Null; }
  bool isBoolean() const { return type() == Type::Boolean; }
  bool isNumber() const { return type() == Type::Number; }
  bool isString() const { return type() == Type::String; }
  bool isArray() const { return type() == Type::Array; }
  bool isObject() const { return type() == Type::Object; }

  // Typed accessors; each throws std::runtime_error when the value has the
  // wrong type (Gson would throw ClassCastException / IllegalStateException).
  bool asBoolean() const;
  Number asNumber() const;
  const std::string& asString() const;
  Array asArray() const;
  Object asObject() const;

  size_t size() const { return node_->size(); }

  // Object lookup.  `find` yields an explicit JSON null member; `get` treats it
  // as absent, which is what JsonOps' MapLike does.
  std::optional<JsonValue> find(std::string_view key) const;
  std::optional<JsonValue> get(std::string_view key) const;
  bool contains(std::string_view key) const { return find(key).has_value(); }

  std::string dump(bool pretty = false, int indent = 2) const;
  std::string typeName() const { return std::string(node_->type_name()); }

  // Deep equality.  Objects compare order-insensitively and numbers compare like
  // Gson's JsonPrimitive (42 == 42.0).
  bool equals(const JsonValue& other) const;
  // Deep equality that also requires identical member order.
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
    // nlohmann's ordered object replaces an existing member in place, matching
    // Gson's LinkedTreeMap-backed JsonObject#add.
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
      // nlohmann's binary values are not part of the JSON data model the codecs
      // operate on; they are reported as null.
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
  // Handles share this document, so no node is copied.
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
      // Order-insensitive, like Gson's JsonObject (LinkedTreeMap) equality.
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

// A C++17 port of Mojang's DataFixerUpper `com.mojang.serialization` Codec API.
//
// lifecycle.hpp -- port of com.mojang.serialization.Lifecycle.


namespace codec {

// Lifecycle tracks whether a (de)serialization path touched anything that is
// not guaranteed to stay stable forever.  It mirrors DFU's Lifecycle exactly,
// including the "experimental wins" combination rule and the
// "lowest `since` deprecated value wins" rule.
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
// 3/8  data_result.hpp -- DataResult, PartialResult semantics, Unit
// ===========================================================================

// A C++17 port of Mojang's DataFixerUpper `com.mojang.serialization` Codec API.
//
// data_result.hpp -- port of com.mojang.serialization.DataResult.
//
// DFU models a result as Either<R, PartialResult<R>>: a success carrying a
// value, or a failure carrying a message plus (optionally) a partially decoded
// value.  The C++ port keeps the exact same shape:
//
//   value_   engaged  <=> Either.left  (success) or PartialResult.partialResult
//   error_   engaged  <=> Either.right (failure)
//
// so `result()` is Java's `result()` (success value only), `error()` is Java's
// `error()` (message) and `resultOrPartial()` / `getOrThrow(allowPartial, ...)`
// return the partial value of a failed decode.



namespace codec {

// Java's Consumer<String> / UnaryOperator<String>.
using ErrorHandler = std::function<void(const std::string&)>;
using StringUnaryOperator = std::function<std::string(const std::string&)>;

template <class R>
class DataResult;

namespace detail {

// Applicative combination shared by apply2/apply2stable/apply3 and by the
// record codec builder; defined below DataResult.
template <class F, class... Ts>
auto combineAll(const Lifecycle& base, F function, const DataResult<Ts>&... results)
    -> DataResult<std::invoke_result_t<F, const Ts&...>>;

}  // namespace detail

// Type-erased view over a DataResult<?>, used by RecordBuilder/ListBuilder
// `withErrorsFrom` (which in DFU take a DataResult<?>).
class DataResultBase {
 public:
  virtual ~DataResultBase() = default;

  bool isSuccess() const { return !error_.has_value(); }
  bool isError() const { return error_.has_value(); }
  // True when a value is available: either a success value or the partial value
  // of a failure.
  bool hasValue() const { return valuePresent(); }

  const std::string& message() const {
    if (!error_) {
      throw std::logic_error("DataResult::message() called on a successful result");
    }
    return *error_;
  }
  const std::optional<std::string>& errorMessage() const { return error_; }
  const Lifecycle& lifecycle() const { return lifecycle_; }

 protected:
  virtual bool valuePresent() const = 0;

  std::optional<std::string> error_;
  Lifecycle lifecycle_;
};

template <class R>
class DataResult : public DataResultBase {
 public:
  using value_type = R;

  DataResult() = default;

  // --- factories (DataResult.success / DataResult.error) -------------------
  static DataResult success(R value, Lifecycle lifecycle = Lifecycle::experimental()) {
    DataResult result;
    result.value_ = std::move(value);
    result.lifecycle_ = lifecycle;
    return result;
  }

  static DataResult error(std::string message) {
    return errorNoPartial(std::move(message), Lifecycle::experimental());
  }

  static DataResult error(std::string message, R partial,
                          Lifecycle lifecycle = Lifecycle::experimental()) {
    DataResult result;
    result.value_ = std::move(partial);
    result.error_ = std::move(message);
    result.lifecycle_ = lifecycle;
    return result;
  }

  static DataResult errorNoPartial(std::string message,
                                   Lifecycle lifecycle = Lifecycle::experimental()) {
    DataResult result;
    result.error_ = std::move(message);
    result.lifecycle_ = lifecycle;
    return result;
  }

  static DataResult errorPartial(std::string message, std::optional<R> partial,
                                 Lifecycle lifecycle = Lifecycle::experimental()) {
    DataResult result;
    result.value_ = std::move(partial);
    result.error_ = std::move(message);
    result.lifecycle_ = lifecycle;
    return result;
  }

  // --- accessors ----------------------------------------------------------
  // Java's result(): the value of a successful result, empty otherwise.
  const std::optional<R>& result() const { return isError() ? kNoValue : value_; }
  const std::optional<std::string>& error() const { return error_; }
  // The stored value whether the result succeeded or failed with a partial.
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
      onError(message());
      if (allowPartial && value_) {
        return *value_;
      }
      throw std::runtime_error(message());
    }
    return *value_;
  }

  std::optional<R> resultOrPartial(const ErrorHandler& onError) const {
    if (isError()) {
      onError(message());
    }
    return value_;
  }

  // --- transformations ----------------------------------------------------
  template <class F>
  using MapValue = std::invoke_result_t<F, const R&>;

  template <class F>
  DataResult<MapValue<F>> map(F function) const {
    using S = MapValue<F>;
    DataResult<S> out;
    out.lifecycle_ = lifecycle_;
    out.error_ = error_;
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
      // Failure without a partial: there is no value to feed the function with.
      return DataResult<S>::errorNoPartial(*error_, lifecycle_);
    }
    DataResult<S> second = std::invoke(function, *value_);
    DataResult<S> out;
    out.lifecycle_ = lifecycle_.add(second.lifecycle_);
    out.value_ = second.value_;
    out.error_ = second.isSuccess() ? *error_ : (*error_ + "; " + second.message());
    return out;
  }

  // Java's DataResult.apply2 / apply2stable / apply3 (the Applicative instance
  // over DataResult).  The lifecycle base is the lifecycle of the pointed
  // function; DFU uses an experimental point for apply2/apply3 and a stable one
  // for apply2stable.
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

  // Only meaningful on a failed result (DFU's setPartial).
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
    if (out.error_) {
      out.error_ = function(*out.error_);
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

  // Turns a partial failure into a success when a partial value exists.
  DataResult promotePartial(const ErrorHandler& onError) const {
    if (!isError()) {
      return *this;
    }
    onError(message());
    if (value_) {
      return DataResult::success(*value_, lifecycle_);
    }
    return *this;
  }

  template <class>
  friend class DataResult;

 private:
  static const std::optional<R> kNoValue;

  bool valuePresent() const override { return value_.has_value(); }

  std::optional<R> value_;
};

template <class R>
const std::optional<R> DataResult<R>::kNoValue{};

namespace detail {

// ---------------------------------------------------------------------------
// combineAll -- DFU's Applicative.super.ap2 chain generalised to N operands:
//   * all operands succeed -> success(f(values...))
//   * otherwise            -> failure whose message is the failed messages
//                             joined by "; " in operand order, carrying a
//                             partial value when every operand has one
//   * lifecycle            -> base.add(op1.lifecycle).add(op2.lifecycle)...
// ---------------------------------------------------------------------------
template <class F, class... Ts>
auto combineAll(const Lifecycle& base, F function, const DataResult<Ts>&... results)
    -> DataResult<std::invoke_result_t<F, const Ts&...>> {
  using R = std::invoke_result_t<F, const Ts&...>;

  Lifecycle lifecycle = base;
  bool allSuccess = true;
  bool allValues = true;
  std::string message;

  const auto step = [&](const DataResultBase& result) {
    lifecycle = lifecycle.add(result.lifecycle());
    allSuccess = allSuccess && result.isSuccess();
    allValues = allValues && result.hasValue();
    if (result.isError()) {
      if (!message.empty()) {
        message += "; ";
      }
      message += result.message();
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
  return DataResult<R>::errorPartial(std::move(message), std::move(partial), lifecycle);
}

// Java's AbstractBuilder.withErrorsFrom: `builder.flatMap(b -> result.map(r -> b))`.
template <class S>
DataResult<S> propagateErrors(const DataResult<S>& builder, const DataResultBase& result) {
  if (result.isSuccess()) {
    return builder;
  }
  if (builder.isSuccess()) {
    return DataResult<S>::error(result.message(), builder.value(),
                                builder.lifecycle().add(result.lifecycle()));
  }
  if (builder.hasValue()) {
    return DataResult<S>::error(builder.message() + "; " + result.message(), builder.value(),
                                builder.lifecycle().add(result.lifecycle()));
  }
  return builder;
}

}  // namespace detail

// `Unit` -- DFU's com.mojang.datafixers.util.Unit, the unit type used by codecs
// that carry no information (Codec::EMPTY, list/map accumulation).
struct Unit {
  bool operator==(const Unit&) const { return true; }
  bool operator!=(const Unit&) const { return false; }
};

}  // namespace codec


// ===========================================================================
// 4/8  dynamic_ops.hpp -- DynamicOps, MapLike, RecordBuilder, ListBuilder, KeyCompressor
// ===========================================================================

// A C++17 port of Mojang's DataFixerUpper `com.mojang.serialization` Codec API.
//
// dynamic_ops.hpp -- ports of DynamicOps, MapLike, RecordBuilder, ListBuilder,
// KeyCompressor and Compressable.
//
// DFU's DynamicOps<T> is generic over the serialized value type (JsonElement,
// NbtTag, ...).  This port uses one universal value type (JsonValue), so the
// type parameter collapses and DynamicOps becomes an abstract class over
// JsonValue.  JsonOps is the only concrete implementation shipped; because the
// value type is universal, DynamicOps::convertTo is the identity here whereas in
// DFU it would rewrite the DOM into the target ops' representation.



namespace codec {

class DynamicOps;
class MapLike;
class KeyCompressor;

using MapLikePtr = std::shared_ptr<const MapLike>;

// Gson's JsonObject.add: an existing member is replaced in place (the object is
// backed by a LinkedTreeMap), which is what DFU's JsonOps relies on.
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
// MapLike<T> -- a read-only view over an object.
//
// Quirk faithfully reproduced from JsonOps: `get` treats an explicit JSON null
// as "absent", while `entries` yields null members as well.
// ---------------------------------------------------------------------------
class MapLike {
 public:
  virtual ~MapLike() = default;

  virtual std::optional<JsonValue> get(const JsonValue& key) const = 0;
  virtual std::optional<JsonValue> get(const std::string& key) const = 0;
  virtual std::vector<std::pair<JsonValue, JsonValue>> entries() const = 0;
  virtual std::string toString() const = 0;
};

// MapLike backed by a JsonValue object (JsonOps.getMap).
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
    // JsonValue::get already treats an explicit JSON null as "absent".
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
// KeyCompressor<T> -- maps keys to dense indices (used when compressMaps()).
//
// DFU's fastutil-backed maps return 0 for unknown keys; this port returns -1
// and callers treat it as "absent" instead of silently reading index 0.
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

// MapLike used by MapDecoder::compressedDecode: a compressed list is addressed
// by key index (DFU's anonymous MapLike in MapDecoder.compressedDecode).
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

// Port of JsonOps.ArrayBuilder (the builder returned by JsonOps.listBuilder()).
//
// The accumulator is held behind a shared_ptr: DFU's builders
// (ImmutableList.Builder, JsonObject, ...) are *mutable objects* that the
// applicative chain carries by reference, so `map`/`apply2stable` copy a pointer
// and appending stays O(1).  Storing the container inline would instead copy the
// whole accumulated list on every element (quadratic encoding).
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

// Port of RecordBuilder.AbstractBuilder<T, R>.
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

// Port of RecordBuilder.AbstractUniversalBuilder / RecordBuilder.MapBuilder.
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
        value);
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

// Port of RecordBuilder.AbstractStringBuilder / JsonOps.JsonRecordBuilder.
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
        value);
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

// Port of MapEncoder.makeCompressedBuilder's CompressedRecordBuilder.
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
        value);
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
  // Java writes into a dense list sized by the compressor; an entry that is
  // never written stays null, which is exactly what the compressed decoder
  // reads back as "absent".
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

  // --- primitives ---------------------------------------------------------
  virtual JsonValue empty() const = 0;

  virtual JsonValue emptyMap() const { return createMap({}); }
  virtual JsonValue emptyList() const { return createList({}); }

  // In DFU this rewrites the value into the target ops' representation; the
  // port's value type is universal, so JsonOps implements it as the identity.
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

  // --- list/map construction ---------------------------------------------
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

  // --- map access ---------------------------------------------------------
  virtual DataResult<std::vector<std::pair<JsonValue, JsonValue>>> getMapValues(
      const JsonValue& input) const = 0;

  virtual DataResult<MapLikePtr> getMap(const JsonValue& input) const {
    if (!input.isObject()) {
      return DataResult<MapLikePtr>::error("Not a JSON object: " + input.dump());
    }
    return DataResult<MapLikePtr>::success(std::make_shared<JsonObjectMapLike>(input));
  }

  virtual JsonValue createMap(const std::vector<std::pair<JsonValue, JsonValue>>& entries) const = 0;

  // --- list access --------------------------------------------------------
  virtual DataResult<std::vector<JsonValue>> getStream(const JsonValue& input) const = 0;

  virtual DataResult<std::vector<JsonValue>> getList(const JsonValue& input) const {
    return getStream(input);
  }

  virtual JsonValue createList(const std::vector<JsonValue>& values) const = 0;

  virtual JsonValue remove(const JsonValue& input, const std::string& key) const = 0;

  // --- generic access -----------------------------------------------------
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

  // --- builders -----------------------------------------------------------
  virtual std::shared_ptr<ListBuilder> listBuilder() const;
  virtual std::shared_ptr<RecordBuilder> mapBuilder() const;

  // --- conversion helpers -------------------------------------------------
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
// Definitions that need DynamicOps to be complete
// ---------------------------------------------------------------------------
inline RecordBuilder& RecordBuilder::add(const std::string& key, const JsonValue& value) {
  return add(ops().createString(key), value);
}

inline RecordBuilder& RecordBuilder::add(const std::string& key,
                                        const DataResult<JsonValue>& value) {
  return add(ops().createString(key), value);
}

// Port of RecordBuilder.AbstractStringBuilder#add(T key, ...).
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
  // Port of JsonOps.JsonRecordBuilder#build.
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

// A C++17 port of Mojang's DataFixerUpper `com.mojang.serialization` Codec API.
//
// json_ops.hpp -- port of com.mojang.serialization.JsonOps (backed by the
// JsonValue DOM instead of Gson's JsonElement).



namespace codec {

class JsonOps : public DynamicOps {
 public:
  static const JsonOps INSTANCE;
  static const JsonOps COMPRESSED;

  explicit JsonOps(bool compressed) : compressed_(compressed) {}

  // Re-expose the DynamicOps overload sets that JsonOps specialises, so that
  // `ops.getNumberValue(value, fallback)` / `ops.mergeToMap(map, mapLike)` keep
  // working on a JsonOps instance.
  using DynamicOps::getNumberValue;
  using DynamicOps::mergeToMap;

  // --- primitives ---------------------------------------------------------
  JsonValue empty() const override { return JsonValue::null(); }

  JsonValue convertTo(const DynamicOps& outOps, const JsonValue& input) const override {
    (void)outOps;
    // The port's serialized value type is universal (it plays the role of
    // JsonElement), so converting between ops is the identity.
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

  // --- list/map construction ---------------------------------------------
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

  // --- map access ---------------------------------------------------------
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

  // --- list access --------------------------------------------------------
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

  // --- behaviour ----------------------------------------------------------
  bool compressMaps() const override { return compressed_; }

  std::shared_ptr<ListBuilder> listBuilder() const override {
    return std::make_shared<ArrayListBuilder>(*this);
  }

  std::shared_ptr<RecordBuilder> mapBuilder() const override {
    return std::make_shared<StringRecordBuilder>(*this);
  }

  std::string toString() const { return "JSON"; }

 private:
  // Java's `key.getAsString()`: strings, or numbers in compressed mode.
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

// A C++17 port of Mojang's DataFixerUpper `com.mojang.serialization` Codec API.
//
// codec.hpp -- ports of Encoder, Decoder, MapEncoder, MapDecoder, Codec and
// MapCodec, including the combinator methods (xmap / flatXmap / comapFlatMap /
// orElse / mapResult / fieldOf / optionalFieldOf / promotePartial / ...).
//
// The Java interfaces are anonymous-implementation based; the C++ port keeps the
// same composition model using std::function wrappers, which is why Codec and
// MapCodec are copyable value types that can be stored in containers and
// captured in lambdas.



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

// DFU's Codec.ResultFunction / MapCodec.ResultFunction.
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
// Encoder<A>  (com.mojang.serialization.Encoder)
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

  // Encoder.empty() -- the MapEncoder that writes nothing.
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
// Decoder<A>  (com.mojang.serialization.Decoder)
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

  // Decoder.unit -- a MapDecoder that ignores its input and yields `value`.
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
// MapEncoder<A>  (com.mojang.serialization.MapEncoder)
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

  // MapEncoder.compressedBuilder: honours DynamicOps.compressMaps().
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

  // MapEncoder.empty() (DFU: Encoder.empty) -- writes nothing, has no keys.
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
// MapDecoder<A>  (com.mojang.serialization.MapDecoder)
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

  // MapDecoder.compressedDecode: reads a compressed key list when the ops ask
  // for map compression, otherwise decodes from the object view.
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

  // Decoder.unit: ignores the input, always yields `value`, has no keys.
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
// MapCodec<A>  (com.mojang.serialization.MapCodec)
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

  // MapCodec.codec() -- wraps this map codec into a full Codec.
  Codec<A> codec() const;

  // MapCodec.forGetter -- builds a getter-only record field (record_codec.hpp).
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

  // MapCodec.unit -- decodes to `value` without reading anything.
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
// Codec<A>  (com.mojang.serialization.Codec)
// ===========================================================================
template <class A>
class Codec {
 public:
  using value_type = A;

  Codec() = default;
  Codec(Encoder<A> encoder, Decoder<A> decoder, std::string name)
      : encoder_(std::move(encoder)), decoder_(std::move(decoder)), name_(std::move(name)) {}

  // Implicit conversion from a MapCodec (DFU's MapCodec.MapCodecCodec).
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

  // Non-null when this codec is backed by a MapCodec (used by dispatch).
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

  // --- structural fields --------------------------------------------------
  MapCodec<A> fieldOf(const std::string& name) const {
    return MapCodec<A>::of(encoder_.fieldOf(name), decoder_.fieldOf(name),
                           "Field[" + name + ": " + name_ + "]");
  }

  MapCodec<std::optional<A>> optionalFieldOf(const std::string& name) const;

  MapCodec<A> optionalFieldOf(const std::string& name, A defaultValue) const;

  Codec<std::vector<A>> listOf() const;

  // --- mapping ------------------------------------------------------------
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

  // --- lifecycle ----------------------------------------------------------
  Codec<A> withLifecycle(const Lifecycle& lifecycle) const {
    return Codec<A>(encoder_.withLifecycle(lifecycle), decoder_.withLifecycle(lifecycle), name_);
  }
  Codec<A> stable() const { return withLifecycle(Lifecycle::stable()); }
  Codec<A> deprecated(int since) const { return withLifecycle(Lifecycle::deprecated(since)); }

  Codec<A> promotePartial(const ErrorHandler& onError) const {
    return Codec<A>(encoder_, decoder_.promotePartial(onError), name_);
  }

  // --- result handling ----------------------------------------------------
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

  // --- dispatch (defined in codecs.hpp) -----------------------------------
  template <class E, class TypeFn, class CodecFn>
  Codec<E> partialDispatch(const std::string& typeKey, TypeFn type, CodecFn codec) const;

  template <class E, class TypeFn, class CodecFn>
  Codec<E> dispatch(const std::string& typeKey, TypeFn type, CodecFn codec) const;

  template <class E, class TypeFn, class CodecFn>
  Codec<E> dispatch(TypeFn type, CodecFn codec) const {
    return dispatch<E>(std::string("type"), std::move(type), std::move(codec));
  }

  template <class E, class TypeFn, class CodecFn>
  MapCodec<E> dispatchMap(const std::string& typeKey, TypeFn type, CodecFn codec) const;

  template <class E, class TypeFn, class CodecFn>
  MapCodec<E> dispatchMap(TypeFn type, CodecFn codec) const {
    return dispatchMap<E>(std::string("type"), std::move(type), std::move(codec));
  }

  // Codec.unit -- a codec that encodes nothing and decodes to `value`.
  static Codec<A> unit(A value) { return MapCodec<A>::unit(std::move(value)).codec(); }

  // Codec.EMPTY -- no-op map codec.
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
// Out-of-line definitions
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
          return DataResult<A>::error("No key " + name + " in " + input.toString());
        }
        return self.parse(ops, *value);
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

// optionalField(name, codec) -- MapCodec<Optional<A>> (OptionalFieldCodec).
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
            // A present-but-invalid optional field is treated as absent.
            return DataResult<std::optional<A>>::success(std::optional<A>{});
          },
          [name](const DynamicOps& ops) { return std::vector<JsonValue>{ops.createString(name)}; }),
      "OptionalFieldCodec[" + name + ": " + elementCodec.name() + "]");
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

}  // namespace codec


// ===========================================================================
// 7/8  codecs.hpp -- primitive + composite codecs, range checks, recursive, dispatch
// ===========================================================================

// A C++17 port of Mojang's DataFixerUpper `com.mojang.serialization` Codec API.
//
// codecs.hpp -- the primitive codecs (Codec.BOOL/INT/String/...) plus the
// composite codecs: ListCodec, EitherCodec, PairCodec, UnboundedMapCodec,
// KeyDispatchCodec, the range checkers and the recursive/lazy codec helper.



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
// Primitive codecs -- PrimitiveCodec<A>
// ---------------------------------------------------------------------------
namespace detail {

template <class A, class ReadFn, class WriteFn>
Codec<A> primitiveCodec(std::string name, ReadFn read, WriteFn write) {
  Encoder<A> encoder([write](const A& input, const DynamicOps& ops, const JsonValue& prefix) {
    return ops.mergeToPrimitive(prefix, write(ops, input));
  });
  Decoder<A> decoder([read](const DynamicOps& ops, const JsonValue& input) {
    return read(ops, input).map([&](const A& value) { return std::make_pair(value, ops.empty()); });
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

// Codec.PASSTHROUGH -- hands the raw dynamic value through unchanged.
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
// stringEnum -- addition without a DFU counterpart
//
// Maps an enum to and from its JSON *name* through a name table, which is what
// Minecraft does with StringRepresentable.fromEnum (that helper lives in
// Minecraft, not in DataFixerUpper, so there is nothing to port here).  It is
// implemented with Codec::flatXmap, so it composes like any other codec: use it
// in fields, lists, dispatch, optional fields, ...
//
//     enum class Severity { Low, Medium, Critical };
//     const Codec<Severity> SeverityCodec = codecs::stringEnum<Severity>(
//         {{"low", Severity::Low},
//          {"medium", Severity::Medium},
//          {"critical", Severity::Critical}},
//         "Severity");
//
// An unknown name fails with `Unknown Severity: "fatal"`, and a value that is not
// in the table fails encoding with `Unmapped Severity value`.  `E` must be
// equality comparable; the table is copied once and shared by both directions.
//
// The other direction -- numbers, or enums serialised as numbers -- needs no
// helper: `Int.xmap<E>(toEnum, toInt)` or `Int.flatXmap<E>(...)` when the mapping
// can fail.  See test/unit/string_and_enum_test.cpp for all four combinations.
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
Codec<std::vector<A>> listOf(const Codec<A>& elementCodec) {
  Encoder<std::vector<A>> encoder([elementCodec](const std::vector<A>& input,
                                                 const DynamicOps& ops,
                                                 const JsonValue& prefix) {
    const std::shared_ptr<ListBuilder> builder = ops.listBuilder();
    for (const A& element : input) {
      builder->add(elementCodec.encodeStart(ops, element));
    }
    return builder->build(prefix);
  });

  Decoder<std::vector<A>> decoder(
      [elementCodec](const DynamicOps& ops,
                     const JsonValue& input)
          -> DataResult<std::pair<std::vector<A>, JsonValue>> {
    return ops.getList(input)
        .setLifecycle(Lifecycle::stable())
        .flatMap([&](const std::vector<JsonValue>& values)
                     -> DataResult<std::pair<std::vector<A>, JsonValue>> {
          std::vector<A> elements;
          std::vector<JsonValue> failed;
          DataResult<Unit> result = DataResult<Unit>::success(Unit{}, Lifecycle::stable());
          for (const JsonValue& value : values) {
            const DataResult<std::pair<A, JsonValue>> element = elementCodec.decode(ops, value);
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
          return result.map([&](const Unit&) { return pair; }).setPartial(pair);
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
Codec<Either<F, S>> either(const Codec<F>& first, const Codec<S>& second) {
  Encoder<Either<F, S>> encoder([first, second](const Either<F, S>& input,
                                               const DynamicOps& ops,
                                               const JsonValue& prefix) {
    return input.isLeft() ? first.encode(input.left(), ops, prefix)
                          : second.encode(input.right(), ops, prefix);
  });

  Decoder<Either<F, S>> decoder([first, second](const DynamicOps& ops, const JsonValue& input) {
    const DataResult<std::pair<Either<F, S>, JsonValue>> firstRead =
        first.decode(ops, input).map([](const std::pair<F, JsonValue>& pair) {
          return std::make_pair(Either<F, S>::left(pair.first), pair.second);
        });
    if (firstRead.result().has_value()) {
      return firstRead;
    }
    return second.decode(ops, input).map([](const std::pair<S, JsonValue>& pair) {
      return std::make_pair(Either<F, S>::right(pair.first), pair.second);
    });
  });

  return Codec<Either<F, S>>::of(Encoder<Either<F, S>>(std::move(encoder)),
                                 Decoder<Either<F, S>>(std::move(decoder)),
                                 "EitherCodec[" + first.name() + ", " + second.name() + "]");
}

// ---------------------------------------------------------------------------
// PairCodec
// ---------------------------------------------------------------------------
template <class F, class S>
Codec<std::pair<F, S>> pair(const Codec<F>& first, const Codec<S>& second) {
  Encoder<std::pair<F, S>> encoder([first, second](const std::pair<F, S>& value,
                                                  const DynamicOps& ops,
                                                  const JsonValue& rest) {
    return second.encode(value.second, ops, rest).flatMap(
        [&](const JsonValue& encoded) { return first.encode(value.first, ops, encoded); });
  });

  Decoder<std::pair<F, S>> decoder([first, second](const DynamicOps& ops, const JsonValue& input) {
    return first.decode(ops, input).flatMap([&](const std::pair<F, JsonValue>& p1) {
      return second.decode(ops, p1.second).map([&](const std::pair<S, JsonValue>& p2) {
        return std::make_pair(std::make_pair(p1.first, p2.first), p2.second);
      });
    });
  });

  return Codec<std::pair<F, S>>::of(Encoder<std::pair<F, S>>(std::move(encoder)),
                                    Decoder<std::pair<F, S>>(std::move(decoder)),
                                    "PairCodec[" + first.name() + ", " + second.name() + "]");
}

// ---------------------------------------------------------------------------
// UnboundedMapCodec (BaseMapCodec)
// ---------------------------------------------------------------------------
template <class K, class V>
Codec<std::vector<std::pair<K, V>>> unboundedMap(const Codec<K>& keyCodec, const Codec<V>& elementCodec) {
  using Entry = std::pair<K, V>;
  using Entries = std::vector<Entry>;

  Encoder<Entries> encoder([keyCodec, elementCodec](const Entries& input, const DynamicOps& ops,
                                                   const JsonValue& prefix) {
    const std::shared_ptr<RecordBuilder> builder = ops.mapBuilder();
    for (const Entry& entry : input) {
      builder->add(keyCodec.encodeStart(ops, entry.first), elementCodec.encodeStart(ops, entry.second));
    }
    return builder->build(prefix);
  });

  Decoder<Entries> decoder([keyCodec, elementCodec](const DynamicOps& ops,
                                                    const JsonValue& input)
      -> DataResult<std::pair<Entries, JsonValue>> {
    return ops.getMap(input).setLifecycle(Lifecycle::stable()).flatMap(
        [&](const MapLikePtr& map) -> DataResult<std::pair<Entries, JsonValue>> {
          Entries elements;
          std::vector<std::pair<JsonValue, JsonValue>> failed;
          DataResult<Unit> result = DataResult<Unit>::success(Unit{}, Lifecycle::stable());
          for (const auto& entry : map->entries()) {
            const DataResult<K> key = keyCodec.parse(ops, entry.first);
            const DataResult<V> value = elementCodec.parse(ops, entry.second);
            const DataResult<Entry> decoded = key.apply2stable(
                [](const K& k, const V& v) { return Entry(k, v); }, value);
            if (decoded.isError()) {
              failed.push_back(entry);
            }
            result = result.apply2stable(
                [&](const Unit& unit, const Entry& decodedEntry) {
                  // DFU uses ImmutableMap (which rejects duplicate keys); the
                  // port keeps insertion order with last-wins semantics.
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
              });
        });
  });

  return Codec<Entries>::of(Encoder<Entries>(std::move(encoder)), Decoder<Entries>(std::move(decoder)),
                            "UnboundedMapCodec[" + keyCodec.name() + " -> " + elementCodec.name() + "]");
}

// ---------------------------------------------------------------------------
// Range checkers (Codec.checkRange / intRange / floatRange / doubleRange)
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
// Recursive / lazy codec
//
// DFU expresses recursive codecs by handing a codec to a datafixer; in C++ the
// cleanest equivalent is a supplier that is resolved on first use, which also
// breaks the static initialisation cycle.
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
                                bool assumeMap) {
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
      [typeKey, keyCodec, codecSelector, assumeMap](const DynamicOps& ops,
                                                    const MapLike& input) -> DataResult<V> {
        const std::optional<JsonValue> elementName = input.get(typeKey);
        if (!elementName.has_value()) {
          return DataResult<V>::error("Input does not contain a key [" + typeKey + "]: " +
                                      input.toString());
        }
        return keyCodec.decode(ops, *elementName)
            .flatMap([&](const std::pair<K, JsonValue>& decoded) -> DataResult<V> {
              return codecSelector(decoded.first)
                  .flatMap([&](const Codec<V>& codec) -> DataResult<V> {
                    if (ops.compressMaps()) {
                      const std::optional<JsonValue> value = input.get(ops.createString("value"));
                      if (!value.has_value()) {
                        return DataResult<V>::error("Input does not have a \"value\" entry: " +
                                                    input.toString());
                      }
                      return codec.parse(ops, *value);
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
                                                  input.toString());
                    }
                    return codec.parse(ops, *value);
                  });
            });
      },
      keys);

  return MapCodec<V>::of(std::move(encoder), std::move(decoder),
                         "KeyDispatchCodec[" + keyCodec.name() + "]");
}

template <class A>
template <class E, class TypeFn, class CodecFn>
Codec<E> Codec<A>::partialDispatch(const std::string& typeKey, TypeFn type, CodecFn codec) const {
  return keyDispatchMapCodec<A, E>(
             typeKey, *this,
             [type](const E& value) -> DataResult<A> { return type(value); },
             [codec](const A& key) -> DataResult<Codec<E>> { return codec(key); }, false)
      .codec();
}

template <class A>
template <class E, class TypeFn, class CodecFn>
Codec<E> Codec<A>::dispatch(const std::string& typeKey, TypeFn type, CodecFn codec) const {
  return partialDispatch<E>(
      typeKey, [type](const E& value) { return DataResult<A>::success(type(value)); },
      [codec](const A& key) { return DataResult<Codec<E>>::success(codec(key)); });
}

template <class A>
template <class E, class TypeFn, class CodecFn>
MapCodec<E> Codec<A>::dispatchMap(const std::string& typeKey, TypeFn type, CodecFn codec) const {
  return keyDispatchMapCodec<A, E>(
      typeKey, *this, [type](const E& value) { return DataResult<A>::success(type(value)); },
      [codec](const A& key) { return DataResult<Codec<E>>::success(codec(key)); }, false);
}

}  // namespace codec


// ===========================================================================
// 8/8  record_codec.hpp -- RecordCodecBuilder: record<>, fieldOf, optionalFieldOf, forGetter
// ===========================================================================

// A C++17 port of Mojang's DataFixerUpper `com.mojang.serialization` Codec API.
//
// record_codec.hpp -- the port of RecordCodecBuilder: `fieldOf`,
// `optionalFieldOf`, `forGetter`, `MapCodec::forGetter` and the variadic
// `record<O>(...)` builder.
//
// DFU:
//     RecordCodecBuilder.create(instance -> instance.group(
//             Codec.STRING.fieldOf("id").forGetter(RiskDef::id),
//             ...)
//         .apply(instance, RiskDef::new));
//
// C++17:
//     Codec<RiskDef> codec = record<RiskDef>(
//             fieldOf("id", &RiskDef::id, codecs::String),
//             ...);
//
// Two flavours of field exist, mirroring what `apply` can consume:
//   * RecordField<O, F>  -- has a getter *and* a setter (built from a member
//     pointer or an explicit setter); `record<O>(fields...)` constructs O by
//     default-constructing and assigning each field.
//   * GetterField<O, F>  -- getter only; requires the constructor form
//     `record<O>(ctor, fields...)`.



namespace codec {

// ---------------------------------------------------------------------------
// Field types
// ---------------------------------------------------------------------------
template <class O, class F>
class GetterField {
 public:
  using object_type = O;
  using value_type = F;

  GetterField() = default;
  GetterField(std::string name, std::function<F(const O&)> getter, MapCodec<F> codec)
      : name_(std::move(name)), getter_(std::move(getter)), codec_(std::move(codec)) {}

  const std::string& name() const { return name_; }
  F get(const O& object) const { return getter_(object); }
  const MapCodec<F>& codec() const { return codec_; }

 private:
  std::string name_;
  std::function<F(const O&)> getter_;
  MapCodec<F> codec_;
};

template <class O, class F>
class RecordField {
 public:
  using object_type = O;
  using value_type = F;

  RecordField() = default;
  RecordField(std::string name, std::function<F(const O&)> getter,
              std::function<void(O&, F)> setter, MapCodec<F> codec)
      : name_(std::move(name)),
        getter_(std::move(getter)),
        setter_(std::move(setter)),
        codec_(std::move(codec)) {}

  const std::string& name() const { return name_; }
  F get(const O& object) const { return getter_(object); }
  void set(O& object, F value) const { setter_(object, std::move(value)); }
  const MapCodec<F>& codec() const { return codec_; }
  const std::function<void(O&, F)>& setter() const { return setter_; }

 private:
  std::string name_;
  std::function<F(const O&)> getter_;
  std::function<void(O&, F)> setter_;
  MapCodec<F> codec_;
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
  FieldBuilder(std::string name, Codec<F> codec)
      : name_(std::move(name)), codec_(std::move(codec)) {}

  MapCodec<F> map() const { return codec_.fieldOf(name_); }

  template <class O>
  RecordField<O, F> forGetter(F O::*member) const {
    return RecordField<O, F>(
        name_, [member](const O& object) { return object.*member; },
        [member](O& object, F value) { object.*member = std::move(value); }, codec_.fieldOf(name_));
  }

  template <class O>
  GetterField<O, F> forGetter(std::function<F(const O&)> getter) const {
    return GetterField<O, F>(name_, std::move(getter), codec_.fieldOf(name_));
  }

 private:
  std::string name_;
  Codec<F> codec_;
};

// fieldOf(name, codec) -- a MapCodec<F> field builder.
template <class F>
FieldBuilder<F> fieldOf(const std::string& name, const Codec<F>& codec) {
  return FieldBuilder<F>(name, std::move(codec));
}

// fieldOf(name, member, codec) -- a settable field.
template <class O, class F>
RecordField<O, F> fieldOf(const std::string& name, F O::*member, const Codec<F>& codec) {
  return fieldOf(name, std::move(codec)).forGetter(member);
}

// optionalFieldOf(name, codec) -- a field builder of std::optional<F>.
template <class F>
FieldBuilder<std::optional<F>> optionalFieldOf(const std::string& name, const Codec<F>& codec) {
  return FieldBuilder<std::optional<F>>(name, optionalField(name, std::move(codec)));
}

// optionalFieldOf(name, member, codec) -- member is a std::optional<F>.
template <class O, class F>
RecordField<O, std::optional<F>> optionalFieldOf(const std::string& name,
                                                std::optional<F> O::*member, const Codec<F>& codec) {
  return RecordField<O, std::optional<F>>(
      name, [member](const O& object) { return object.*member; },
      [member](O& object, std::optional<F> value) { object.*member = std::move(value); },
      optionalField(name, std::move(codec)));
}

// optionalFieldOf(name, member, codec, defaultValue) -- an absent member decodes
// to `defaultValue` and a member equal to it is not encoded (DFU's
// Codec.optionalFieldOf(String, A)).
template <class O, class F>
RecordField<O, F> optionalFieldOf(const std::string& name, F O::*member, const Codec<F>& codec,
                                  F defaultValue) {
  return RecordField<O, F>(
      name, [member](const O& object) { return object.*member; },
      [member](O& object, F value) { object.*member = std::move(value); },
      codec.optionalFieldOf(name, std::move(defaultValue)));
}

// MapCodec.forGetter -- DFU's `mapCodec.forGetter(getter)`.
template <class A>
template <class O>
inline GetterField<O, A> MapCodec<A>::forGetter(std::function<A(const O&)> getter) const {
  return GetterField<O, A>("<map>", std::move(getter), *this);
}

// ---------------------------------------------------------------------------
// record<O>(...)
// ---------------------------------------------------------------------------
namespace detail {

struct ResultSummary {
  Lifecycle lifecycle;
  bool allSuccess = true;
  bool allValues = true;
  std::string message;
};

template <class... Rs>
ResultSummary summarizeResults(const Lifecycle& base,
                               const std::tuple<DataResult<Rs>...>& results) {
  ResultSummary summary{base, true, true, ""};
  const auto step = [&summary](const DataResultBase& result) {
    summary.lifecycle = summary.lifecycle.add(result.lifecycle());
    summary.allSuccess = summary.allSuccess && result.isSuccess();
    summary.allValues = summary.allValues && result.hasValue();
    if (result.isError()) {
      if (!summary.message.empty()) {
        summary.message += "; ";
      }
      summary.message += result.message();
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

// Encodes every field into the same RecordBuilder, in declaration order.
template <class O, class FieldsTuple, std::size_t... I>
RecordBuilder& encodeFields(const FieldsTuple& fields, const O& input, const DynamicOps& ops,
                            RecordBuilder& prefix, std::index_sequence<I...>) {
  (void)std::initializer_list<int>{
      (std::get<I>(fields).codec().encode(std::get<I>(fields).get(input), ops, prefix), 0)...};
  return prefix;
}

}  // namespace detail

// record<O>(fields...) -- all fields must be settable, O must be default
// constructible (DFU: RecordCodecBuilder.create(instance -> instance.group(...)
// .apply(instance, O::new))).
template <class O, class... Fields,
          class = std::enable_if_t<(is_record_field<Fields>::value && ...)>,
          class = std::enable_if_t<(std::is_same<typename Fields::object_type, O>::value && ...)>>
MapCodec<O> record(Fields... fields) {
  static_assert(std::is_default_constructible<O>::value,
                "record<O>(fields...) needs a default constructible O; use "
                "record<O>(constructor, fields...) instead");
  constexpr std::size_t kCount = sizeof...(Fields);
  const auto fieldsTuple = std::make_tuple(fields...);
  const auto setters = detail::makeSetters<O>(fieldsTuple, std::make_index_sequence<kCount>{});

  const auto ctor = [setters](const typename Fields::value_type&... values) -> O {
    O object{};
    detail::applySetters(object, setters, values...);
    return object;
  };

  MapEncoder<O> encoder(
      [fieldsTuple](const O& input, const DynamicOps& ops, RecordBuilder& prefix) -> RecordBuilder& {
        return detail::encodeFields(fieldsTuple, input, ops, prefix,
                                    std::make_index_sequence<kCount>{});
      },
      [fieldsTuple](const DynamicOps& ops) {
        return detail::fieldEncoderKeys(fieldsTuple, ops, std::make_index_sequence<kCount>{});
      });

  MapDecoder<O> decoder(
      [fieldsTuple, ctor](const DynamicOps& ops, const MapLike& input) -> DataResult<O> {
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
          O built = detail::buildFromResults<O>(ctor, results, std::make_index_sequence<kCount>{});
          if (summary.allSuccess) {
            return DataResult<O>::success(std::move(built), summary.lifecycle);
          }
          return DataResult<O>::error(summary.message, std::move(built), summary.lifecycle);
        }
        return DataResult<O>::errorNoPartial(summary.message, summary.lifecycle);
      },
      [fieldsTuple](const DynamicOps& ops) {
        return detail::fieldEncoderKeys(fieldsTuple, ops, std::make_index_sequence<kCount>{});
      });

  return MapCodec<O>::of(std::move(encoder), std::move(decoder),
                         "RecordCodec[" +
                             detail::joinFieldNames(fieldsTuple,
                                                    std::make_index_sequence<kCount>{}) +
                             "]");
}

// record<O>(constructor, fields...) -- the DFU `apply(instance, ctor)` form; use
// it for types without a default constructor or with getter-only fields.
template <class O, class Ctor, class... Fields,
          class = std::enable_if_t<(is_field<Fields>::value && ...)>,
          class = std::enable_if_t<(std::is_same<typename Fields::object_type, O>::value && ...)>,
          class = std::enable_if_t<
              std::is_invocable<Ctor, typename Fields::value_type...>::value>>
MapCodec<O> record(Ctor ctor, Fields... fields) {
  constexpr std::size_t kCount = sizeof...(Fields);
  const auto fieldsTuple = std::make_tuple(fields...);
  const auto constructor = std::move(ctor);

  MapEncoder<O> encoder(
      [fieldsTuple](const O& input, const DynamicOps& ops, RecordBuilder& prefix) -> RecordBuilder& {
        return detail::encodeFields(fieldsTuple, input, ops, prefix,
                                    std::make_index_sequence<kCount>{});
      },
      [fieldsTuple](const DynamicOps& ops) {
        return detail::fieldEncoderKeys(fieldsTuple, ops, std::make_index_sequence<kCount>{});
      });

  MapDecoder<O> decoder(
      [fieldsTuple, constructor](const DynamicOps& ops, const MapLike& input) -> DataResult<O> {
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
                                                std::make_index_sequence<kCount>{});
          if (summary.allSuccess) {
            return DataResult<O>::success(std::move(built), summary.lifecycle);
          }
          return DataResult<O>::error(summary.message, std::move(built), summary.lifecycle);
        }
        return DataResult<O>::errorNoPartial(summary.message, summary.lifecycle);
      },
      [fieldsTuple](const DynamicOps& ops) {
        return detail::fieldDecoderKeys(fieldsTuple, ops, std::make_index_sequence<kCount>{});
      });

  return MapCodec<O>::of(std::move(encoder), std::move(decoder),
                         "RecordCodec[" +
                             detail::joinFieldNames(fieldsTuple,
                                                    std::make_index_sequence<kCount>{}) +
                             "]");
}

// recordCodec<O>(...) -- the same builders, but returning a Codec directly
// (DFU's RecordCodecBuilder.create).
template <class O, class... Args>
Codec<O> recordCodec(Args&&... args) {
  return record<O>(std::forward<Args>(args)...).codec();
}

}  // namespace codec

