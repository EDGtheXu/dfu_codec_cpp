// A C++17 port of Mojang's DataFixerUpper `com.mojang.serialization` Codec API.
//
// codecs.hpp -- the primitive codecs (Codec.BOOL/INT/String/...) plus the
// composite codecs: ListCodec, EitherCodec, PairCodec, UnboundedMapCodec,
// KeyDispatchCodec, the range checkers and the recursive/lazy codec helper.
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "codec/codec.hpp"
#include "codec/dynamic_ops.hpp"
#include "codec/json.hpp"

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
