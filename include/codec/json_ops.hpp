// A C++17 port of Mojang's DataFixerUpper `com.mojang.serialization` Codec API.
//
// json_ops.hpp -- port of com.mojang.serialization.JsonOps (backed by the
// JsonValue DOM instead of Gson's JsonElement).
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "codec/dynamic_ops.hpp"

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
