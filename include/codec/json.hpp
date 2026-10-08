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
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

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
