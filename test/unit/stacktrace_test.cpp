// 标准库栈回溯（std::stacktrace）。
//
// 这是「用标准库的方式拿到真正的 C++ 调用栈」：错误产生的那一刻抓栈，
// report() 用标准库自己的排版把它贴在 codec 调用链之后——
//
//   MSVC:      0> D:\...\stacktrace_test.cpp(41): codec_unit_tests!...   （需 /Zi）
//   libstdc++: 0# ... at /path/stacktrace_test.cpp:41
//
// IDE（CLion 的 Analyze Stack Trace、控制台超链接）认这两种格式，因此
// 栈里的每一帧都能点开——包括用户调用 parse() 的那一行，这是 codec 调用链
// 自己给不出的信息。
//
// 代价：每产生一个错误就抓一次栈（包括会被吞掉的「存在但非法的可选字段」），
// 所以默认关闭。打开方式：
//
//   cmake -DCODEC_RECORD_STACKTRACE=ON ...   （会把本工程切到 C++23）
//
// 没有它时，跨平台的位置信息来自 SourceLocation（见 error_path_test.cpp），
// 那条路不需要调试信息、也不依赖 C++23。
#include <string>
#include <vector>

#include "codec_json.hpp"
#include <gtest/gtest.h>

namespace {

using codec::Codec;
using codec::DataResult;
using codec::JsonOps;
using codec::JsonValue;
using codec::Value;

// 开关打开但标准库没有 <stacktrace> 时，这里会明确报错，而不是静默失去栈。
TEST(StacktraceTest, FeatureDetectionMatchesTheBuild) {
#if defined(CODEC_RECORD_STACKTRACE) && !defined(CODEC_HAS_STACKTRACE)
  FAIL() << "CODEC_RECORD_STACKTRACE is defined but this standard library has no "
            "<stacktrace>. Compile as C++23 (MSVC: /std:c++latest, GCC/Clang: -std=c++23).";
#else
  SUCCEED();
#endif
}

#if defined(CODEC_CAPTURES_STACKTRACE)

TEST(StacktraceTest, CapturesTheStackAndRendersItInReport) {
  const Codec<std::vector<int32_t>> codec = codec::listOf(codec::codecs::Int);
  const DataResult<std::vector<int32_t>> result =
      codec.parse(JsonOps::INSTANCE, JsonValue::parse(R"([1,"x"])"));
  ASSERT_TRUE(result.isError());

  // 每个失败部分都带一份抓到的栈。
  ASSERT_EQ(result.errors().size(), 1u);
  EXPECT_GT(result.errors().front().trace.size(), 0u);

  const std::string report = result.report();
  const size_t traceAt = report.find("stacktrace:");
  ASSERT_NE(traceAt, std::string::npos) << report;
  // codec 调用链与栈回溯是两段内容：链在前，栈在后
  // （两种排版风格都会打印 codec 名字，所以这里不假设 msvc/gnu）。
  EXPECT_LT(report.find("list"), traceAt) << report;

#if defined(_DEBUG)
  // Debug 构建带调试信息，因此栈里能解析出本文件与行号——
  // 这正是 IDE 里「可点击」的前提。
  EXPECT_NE(report.find("stacktrace_test.cpp"), std::string::npos) << report;
#endif
}

#else

TEST(StacktraceTest, IsOffUnlessTheBuildOptionIsSet) {
  GTEST_SKIP() << "build with -DCODEC_RECORD_STACKTRACE=ON (C++23) to capture "
                  "std::stacktrace; SourceLocation-based frames work either way";
}

#endif

}  // namespace
