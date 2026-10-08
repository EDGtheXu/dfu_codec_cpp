// Shared helpers for the Codec port unit tests.
#pragma once

#include <algorithm>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "codec.hpp"

namespace codec {
namespace testing {

inline JsonValue json(const std::string& text) { return JsonValue::parse(text); }

// 阶段 1：ops 层收发类型擦除的句柄 codec::Value（JsonValue 仍是 JSON 的 DOM）。
// 测试里要读 JSON 文本时用这两个小工具，避免到处写 .asJson()。
inline std::string dumpJson(const Value& value) { return value.asJson().dump(); }
inline std::string dumpJson(const JsonValue& value) { return value.dump(); }

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

// 阶段 2：Passthrough 的值类型是 codec::Dynamic（值 + 懂它的 ops）。
inline Dynamic dynamicJson(const std::string& text) {
  return Dynamic(JsonOps::INSTANCE, Value(JsonValue::parse(text)));
}

// Encodes a value and returns the JSON text.
template <class A>
std::string encode(const Codec<A>& codec, const typename Codec<A>::value_type& value,
                   const DynamicOps& ops = JsonOps::INSTANCE) {
  const DataResult<Value> result = codec.encodeStart(ops, value);
  if (!result.result().has_value()) {
    throw std::runtime_error("encode failed: " + result.message());
  }
  return dumpJson(*result.result());
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
