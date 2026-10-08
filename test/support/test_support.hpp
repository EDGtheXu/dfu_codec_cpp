// Shared helpers for the Codec port unit tests.
#pragma once

#include <algorithm>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "codec/all.hpp"

namespace codec {
namespace testing {

inline JsonValue json(const std::string& text) { return JsonValue::parse(text); }

// Parses `text` and decodes it, failing the test with the codec error message.
template <class A>
A decode(const Codec<A>& codec, const std::string& text,
         const DynamicOps& ops = JsonOps::INSTANCE) {
  const DataResult<A> result = codec.parse(ops, json(text));
  if (!result.result().has_value()) {
    throw std::runtime_error("decode failed: " + result.message());
  }
  return *result.result();
}

// Encodes a value and returns the JSON text.
template <class A>
std::string encode(const Codec<A>& codec, const typename Codec<A>::value_type& value,
                   const DynamicOps& ops = JsonOps::INSTANCE) {
  const DataResult<JsonValue> result = codec.encodeStart(ops, value);
  if (!result.result().has_value()) {
    throw std::runtime_error("encode failed: " + result.message());
  }
  return result.result()->dump();
}

// Decodes expecting a failure and returns the error message.
template <class A>
std::string decodeError(const Codec<A>& codec, const std::string& text,
                        const DynamicOps& ops = JsonOps::INSTANCE) {
  const DataResult<A> result = codec.parse(ops, json(text));
  if (!result.isError()) {
    throw std::runtime_error("expected a decode failure for: " + text);
  }
  return result.message();
}

}  // namespace testing
}  // namespace codec
