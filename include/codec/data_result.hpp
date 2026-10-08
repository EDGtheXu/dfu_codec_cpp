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
#pragma once

#include <functional>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include "codec/lifecycle.hpp"

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
