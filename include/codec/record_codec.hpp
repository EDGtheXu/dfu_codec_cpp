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
#pragma once

#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "codec/codec.hpp"
#include "codec/codecs.hpp"

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
