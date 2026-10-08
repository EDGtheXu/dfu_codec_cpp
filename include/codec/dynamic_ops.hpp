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
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "codec/data_result.hpp"
#include "codec/json.hpp"
#include "codec/lifecycle.hpp"

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
