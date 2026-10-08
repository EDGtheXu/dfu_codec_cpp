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
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "codec/data_result.hpp"
#include "codec/dynamic_ops.hpp"
#include "codec/json.hpp"
#include "codec/lifecycle.hpp"

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
