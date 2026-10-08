// Performance of the TOML layer, in the same spirit as `codec_perf_test.cpp`:
// a *measurement*, not a correctness gate (the only assertions are that the
// round trip produces the same document -- a benchmark that fails on a busy
// machine is worse than no benchmark).
//
// What it is for:
//
//   * visibility for the TOML *encoding* path.  `TomlOps` used to fall back to
//     the generic builders, which call `mergeToList`/`mergeToMap` once per
//     element; those deep-copy the whole accumulated `toml::Array`/`toml::Table`
//     on every call, so encoding was O(N^2) (400 risks: ~250 ms).  `TomlOps` now
//     uses its own mutable-accumulator builders; a regression back to the
//     generic ones shows up here as a ~100x jump in `encode` and in the
//     per-risk column, not as a subtle few-percent drift.
//   * the cost of the second ops overall: TOML parse, decode, encode, dump and
//     (when the JSON entry header is available) both `convertTo` directions.
//
// This layer is **JSON-free**: its fixtures are TOML text generated in C++, so it
// builds and runs with `-DCODEC_BUILD_JSON=OFF` on a machine without any JSON
// library.  Only the rows that need a second ops to compare against are wrapped in
// `CODEC_TEST_WITH_JSON` (defined by CMake when the JSON layer is built).
//
// Run it with `ctest -R toml_perf -V` or directly:
//
//   build/test/codec_toml_perf_tests.exe --gtest_filter=TomlPerfTest.SmallDocument
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "codec_toml.hpp"
#include "risk_def.hpp"
#include <gtest/gtest.h>

#ifdef CODEC_TEST_WITH_JSON
#include "codec_json.hpp"
#endif

namespace {

using Clock = std::chrono::steady_clock;

// Keeps the optimiser honest: every measured body feeds this sink.
std::uint64_t g_sink = 0;

struct Measurement {
  std::string label;
  double nanosecondsPerOp = 0.0;
  double operationsPerSecond = 0.0;
  int iterations = 0;
};

int rounds() {
#ifdef NDEBUG
  return 7;  // Release
#else
  return 3;  // Debug builds are ~30x slower; keep the layer fast
#endif
}

constexpr double kCalibrationMilliseconds = 5.0;
constexpr double kRoundMilliseconds = 40.0;

// Same harness as the JSON perf layer: calibrate the iteration count per
// measurement, then report the best per-operation time of `rounds()` runs.
template <class F>
Measurement measure(const std::string& label, F body) {
  int probes = 0;
  const auto probeStart = Clock::now();
  double probeMilliseconds = 0.0;
  do {
    body();
    ++probes;
    probeMilliseconds =
        std::chrono::duration<double, std::milli>(Clock::now() - probeStart).count();
  } while (probeMilliseconds < kCalibrationMilliseconds && probes < 10000);

  const double probeNanoseconds = probeMilliseconds * 1e6 / probes;
  const int count =
      std::clamp(static_cast<int>(kRoundMilliseconds * 1e6 / std::max(probeNanoseconds, 1.0)), 3,
                 20000);

  double best = std::numeric_limits<double>::max();
  for (int round = 0; round < rounds(); ++round) {
    const auto start = Clock::now();
    for (int i = 0; i < count; ++i) {
      body();
    }
    const auto finish = Clock::now();
    const double perOp =
        std::chrono::duration<double, std::nano>(finish - start).count() / count;
    best = std::min(best, perOp);
  }
  return Measurement{label, best, 1e9 / best, count};
}

void printRow(const Measurement& measurement, double baselineNanoseconds) {
  std::printf("  %-40s %7d %10.0f ns/op %11.0f op/s %7.2fx\n", measurement.label.c_str(),
              measurement.iterations, measurement.nanosecondsPerOp,
              measurement.operationsPerSecond,
              measurement.nanosecondsPerOp / baselineNanoseconds);
}

void printHeader(const std::string& title) {
  std::printf("\n%s\n", title.c_str());
  std::printf("  %-40s %7s %16s %15s %8s\n", "operation", "iters", "time", "throughput",
              "relative");
}

// ---------------------------------------------------------------------------
// Fixtures: the document is built in C++ and round-tripped through TomlOps, so
// this layer needs no JSON at all and the fixture text is exactly what the TOML
// writer produces (canonical form, so a re-encode can be compared byte for byte).
// The shape matches the risk-definition document of the other layers.
// ---------------------------------------------------------------------------
risk::RiskDocument makeSampleDocument(int riskCount) {
  risk::RiskDocument document;
  for (int i = 0; i < riskCount; ++i) {
    risk::RiskDef risk;
    risk.id = "r" + std::to_string(i);
    risk.vid = "v";
    risk.riskType = "t";
    risk.severity = "s";
    risk.name = risk::LocalizedText{"c", "e"};
    risk.description = risk::LocalizedText{"c", "e"};
    risk.solution = risk::LocalizedText{"c", "e"};
    risk.evidence = {"e1", "e2"};
    risk::Condition condition;
    condition.param = "p";
    condition.op = "is_true";
    risk.condition = condition;
    document.risks.push_back(risk);
  }
  return document;
}

int largeRiskCount() {
#ifdef NDEBUG
  return 400;
#else
  return 40;  // Debug is ~30x slower; the ratios are what matter
#endif
}

struct Fixture {
  std::string name;
  std::string toml;
  std::size_t bytes = 0;
  std::size_t risks = 0;
  risk::RiskDocument document;
};

Fixture makeFixture(std::string name, int riskCount) {
  const risk::RiskDocument document = makeSampleDocument(riskCount);
  const codec::DataResult<codec::Value> encoded =
      risk::riskDocumentCodec().encodeStart(codec::TomlOps::INSTANCE, document);
  if (encoded.isError()) {
    throw std::runtime_error("fixture encode failed: " + encoded.message());
  }
  const codec::DataResult<std::string> text = codec::dumpToml(*encoded.result());
  if (text.isError()) {
    throw std::runtime_error("fixture dump failed: " + text.message());
  }
  // Fail loudly here rather than measuring a document that cannot be read back.
  const codec::DataResult<codec::TomlDocument> parsed = codec::parseToml(*text.result());
  if (parsed.isError()) {
    throw std::runtime_error("fixture parse failed: " + parsed.message());
  }
  const codec::DataResult<risk::RiskDocument> decoded =
      risk::riskDocumentCodec().parse(codec::TomlOps::INSTANCE, parsed.result()->root());
  if (decoded.isError()) {
    throw std::runtime_error("fixture decode failed: " + decoded.message());
  }
  const std::size_t bytes = text.result()->size();
  return Fixture{std::move(name), *text.result(), bytes, static_cast<std::size_t>(riskCount),
                 document};
}

const Fixture& smallFixture() {
  static const Fixture fixture = [] { return makeFixture("2 risks", 2); }();
  return fixture;
}

const Fixture& largeFixture() {
  static const Fixture fixture = [] {
    const int count = largeRiskCount();
    return makeFixture(std::to_string(count) + " risks", count);
  }();
  return fixture;
}

}  // namespace

// Correctness guard: the benchmark only means something if the TOML document
// decodes back to the document it was generated from, re-encodes to the same
// text, and (with the JSON layer available) survives a trip through JSON.
TEST(TomlPerfTest, StrategiesAgreeOnTheFixture) {
  for (const Fixture* fixture : {&smallFixture(), &largeFixture()}) {
    const codec::DataResult<codec::TomlDocument> parsed = codec::parseToml(fixture->toml);
    ASSERT_TRUE(parsed.isSuccess()) << fixture->name << ": " << parsed.message();
    const codec::DataResult<risk::RiskDocument> fromToml =
        risk::riskDocumentCodec().parse(codec::TomlOps::INSTANCE, parsed.result()->root());
    ASSERT_TRUE(fromToml.isSuccess()) << fixture->name << ": " << fromToml.message();
    EXPECT_EQ(fromToml.result()->risks.size(), fixture->risks) << fixture->name;
    EXPECT_EQ(*fromToml.result(), fixture->document) << fixture->name;

    // Re-encoding the decoded document must reproduce the same TOML text.
    const codec::DataResult<codec::Value> again =
        risk::riskDocumentCodec().encodeStart(codec::TomlOps::INSTANCE, *fromToml.result());
    ASSERT_TRUE(again.isSuccess()) << fixture->name;
    const codec::DataResult<std::string> againText = codec::dumpToml(*again.result());
    ASSERT_TRUE(againText.isSuccess()) << fixture->name;
    EXPECT_EQ(*againText.result(), fixture->toml) << fixture->name;

#ifdef CODEC_TEST_WITH_JSON
    // The same struct must survive a trip through JSON and back.
    const codec::DataResult<codec::Value> encodedJson =
        risk::riskDocumentCodec().encodeStart(codec::JsonOps::INSTANCE, *fromToml.result());
    ASSERT_TRUE(encodedJson.isSuccess()) << fixture->name;
    const codec::DataResult<risk::RiskDocument> fromJson =
        risk::riskDocumentCodec().parse(codec::JsonOps::INSTANCE, *encodedJson.result());
    ASSERT_TRUE(fromJson.isSuccess()) << fixture->name << ": " << fromJson.message();
    EXPECT_EQ(fromJson.result()->risks.size(), fixture->risks) << fixture->name;
    EXPECT_EQ(*fromToml.result(), *fromJson.result()) << fixture->name;
    EXPECT_EQ(*fromJson.result(), fixture->document) << fixture->name;
#endif
  }
}

namespace {

void runComparison(const Fixture& fixture) {
  const codec::Codec<risk::RiskDocument>& documentCodec = risk::riskDocumentCodec();

  // Everything below is prepared outside the timed regions.
  const codec::DataResult<codec::TomlDocument> parsedDocument = codec::parseToml(fixture.toml);
  const codec::DataResult<risk::RiskDocument> decoded =
      documentCodec.parse(codec::TomlOps::INSTANCE, parsedDocument.result()->root());
  const risk::RiskDocument& expected = *decoded.result();
  const codec::DataResult<codec::Value> encodedValue =
      documentCodec.encodeStart(codec::TomlOps::INSTANCE, expected);

  std::printf("\n-------------------------------------------------------------------------\n");
  std::printf(" fixture: %s (%zu TOML bytes) -- %s build, best of %d rounds\n", fixture.name.c_str(),
              fixture.bytes,
#ifdef NDEBUG
              "Release",
#else
              "Debug",
#endif
              rounds());
  std::printf("-------------------------------------------------------------------------\n");

  printHeader("decoding");
  const Measurement tomlParse = measure("TomlOps: parseToml", [&] {
    g_sink += codec::parseToml(fixture.toml).result()->raw().size();
  });
  const Measurement tomlParseAndDecode = measure("TomlOps: parseToml + codec decode", [&] {
    const codec::DataResult<codec::TomlDocument> document = codec::parseToml(fixture.toml);
    g_sink += documentCodec.parse(codec::TomlOps::INSTANCE, document.result()->root())
                  .result()
                  ->risks.size();
  });
  const Measurement tomlDecodeOnly = measure("TomlOps: codec decode (pre-parsed)", [&] {
    g_sink += documentCodec.parse(codec::TomlOps::INSTANCE, parsedDocument.result()->root())
                  .result()
                  ->risks.size();
  });

  const double baseline = tomlParse.nanosecondsPerOp;
  printRow(tomlParse, baseline);
  printRow(tomlParseAndDecode, baseline);
  printRow(tomlDecodeOnly, baseline);

  printHeader("encoding");
  const Measurement codecEncode = measure("TomlOps: codec encode", [&] {
    g_sink += static_cast<std::uint64_t>(
        documentCodec.encodeStart(codec::TomlOps::INSTANCE, expected).isSuccess());
  });
  const Measurement encodeAndDump = measure("TomlOps: codec encode + dumpToml", [&] {
    const codec::DataResult<codec::Value> value =
        documentCodec.encodeStart(codec::TomlOps::INSTANCE, expected);
    g_sink += codec::dumpToml(*value.result()).result()->size();
  });
  const Measurement dumpOnly = measure("TomlOps: dumpToml (pre-encoded)", [&] {
    g_sink += codec::dumpToml(*encodedValue.result()).result()->size();
  });
  printRow(codecEncode, baseline);
  printRow(encodeAndDump, baseline);
  printRow(dumpOnly, baseline);

#ifdef CODEC_TEST_WITH_JSON
  printHeader("against the JSON layer");
  const codec::JsonValue jsonSample = codec::JsonValue::parse(risk::kSampleRiskJson());
  const Measurement jsonDecodeOnly = measure("JsonOps: codec decode (pre-parsed)", [&] {
    g_sink += documentCodec.parse(codec::JsonOps::INSTANCE, jsonSample).result()->risks.size();
  });
  const Measurement jsonEncode = measure("JsonOps: codec encode + dump", [&] {
    g_sink += jsonView(*documentCodec.encodeStart(codec::JsonOps::INSTANCE, expected).result())
                  .dump()
                  .size();
  });
  // The handles are boxed outside the timed regions: only the conversion counts.
  const codec::Value tomlValue = parsedDocument.result()->root();
  const codec::Value jsonFixture = codec::JsonValue::parse(risk::kSampleRiskJson());
  const Measurement toToml = measure("JsonOps -> TomlOps convertTo", [&] {
    g_sink += static_cast<std::uint64_t>(
        codec::JsonOps::INSTANCE.convertTo(codec::TomlOps::INSTANCE, jsonFixture).tag != nullptr);
  });
  const Measurement toJson = measure("TomlOps -> JsonOps convertTo", [&] {
    g_sink += static_cast<std::uint64_t>(
        codec::TomlOps::INSTANCE.convertTo(codec::JsonOps::INSTANCE, tomlValue).tag != nullptr);
  });
  printRow(jsonDecodeOnly, baseline);
  printRow(jsonEncode, baseline);
  printRow(toToml, baseline);
  printRow(toJson, baseline);
#endif

  // Per-risk cost.  With the linear builders this grows only with cache pressure
  // (the large fixture is 400x the TOML text), never with N: quadratic encoding
  // showed up here as ~550 us/risk in the 400-risk fixture instead of ~20.
  const double risks = static_cast<double>(fixture.risks);
  const double encodePerRisk = codecEncode.nanosecondsPerOp / 1000.0 / risks;
  const double decodePerRisk = tomlDecodeOnly.nanosecondsPerOp / 1000.0 / risks;
  std::printf("\n  per-risk encode:   %8.2f us/risk\n", encodePerRisk);
  std::printf("  per-risk decode:   %8.2f us/risk\n", decodePerRisk);
  std::printf("  encode vs decode:  %8.2fx\n",
              codecEncode.nanosecondsPerOp / tomlDecodeOnly.nanosecondsPerOp);
  std::printf("  TOML text:         %8zu bytes (%zu risks)\n", fixture.bytes, fixture.risks);
}

}  // namespace

TEST(TomlPerfTest, SmallDocument) {
  runComparison(smallFixture());
  std::printf("\n(checksum %llu -- keeps the measured work observable)\n",
              static_cast<unsigned long long>(g_sink));
}

TEST(TomlPerfTest, LargeDocument) {
  runComparison(largeFixture());
  std::printf("\n(checksum %llu -- keeps the measured work observable)\n",
              static_cast<unsigned long long>(g_sink));
}
