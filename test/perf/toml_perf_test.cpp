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
//     both `convertTo` directions.
//
// Run it with `ctest -R toml_perf -V` or directly:
//
//   build/test/codec_toml_perf_tests.exe --gtest_filter=TomlPerfTest.SmallDocument
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "codec_toml.hpp"
#include "risk_def.hpp"
#include "test_support.hpp"
#include <gtest/gtest.h>

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
// Fixtures: built once, outside every timed region.
// ---------------------------------------------------------------------------
std::string largeRiskJson(int riskCount) {
  // Reuse the sample document's risks, repeated, exactly like the JSON layer.
  const codec::DataResult<risk::RiskDocument> sampleDocument = risk::riskDocumentCodec().parse(
      codec::JsonOps::INSTANCE, codec::JsonValue::parse(risk::kSampleRiskJson()));
  if (sampleDocument.isError()) {
    throw std::runtime_error("sample decode failed: " + sampleDocument.message());
  }
  const std::vector<risk::RiskDef>& risks = sampleDocument.result()->risks;
  risk::RiskDocument big;
  for (int i = 0; i < riskCount; ++i) {
    big.risks.push_back(risks[static_cast<std::size_t>(i) % risks.size()]);
  }
  const codec::DataResult<codec::Value> encoded =
      risk::riskDocumentCodec().encodeStart(codec::JsonOps::INSTANCE, big);
  if (encoded.isError()) {
    throw std::runtime_error("sample encode failed: " + encoded.message());
  }
  return encoded.result()->asJson().dump();
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
  std::string json;
  std::string toml;  // the same document, encoded through TomlOps and dumped
  std::size_t bytes = 0;
  std::size_t risks = 0;
};

Fixture makeFixture(std::string name, std::string json, std::size_t risks) {
  const codec::DataResult<risk::RiskDocument> document =
      risk::riskDocumentCodec().parse(codec::JsonOps::INSTANCE, codec::JsonValue::parse(json));
  if (document.isError()) {
    throw std::runtime_error("fixture decode failed: " + document.message());
  }
  const codec::DataResult<codec::Value> encoded =
      risk::riskDocumentCodec().encodeStart(codec::TomlOps::INSTANCE, *document.result());
  if (encoded.isError()) {
    throw std::runtime_error("fixture TOML encode failed: " + encoded.message());
  }
  const codec::DataResult<std::string> text = codec::dumpToml(*encoded.result());
  if (text.isError()) {
    throw std::runtime_error("fixture dumpToml failed: " + text.message());
  }
  return Fixture{std::move(name), std::move(json), *text.result(), text.result()->size(), risks};
}

const Fixture& smallFixture() {
  static const Fixture fixture = [] {
    const std::string json = risk::kSampleRiskJson();
    return makeFixture("2 risks", json, 2);
  }();
  return fixture;
}

const Fixture& largeFixture() {
  static const Fixture fixture = [] {
    const int count = largeRiskCount();
    return makeFixture(std::to_string(count) + " risks", largeRiskJson(count),
                       static_cast<std::size_t>(count));
  }();
  return fixture;
}

}  // namespace

// Correctness guard: the benchmark only means something if TOML and JSON decode
// to the same document, and if the TOML text is stable across a round trip.
TEST(TomlPerfTest, StrategiesAgreeOnTheFixture) {
  for (const Fixture* fixture : {&smallFixture(), &largeFixture()}) {
    const codec::DataResult<risk::RiskDocument> fromJson = risk::riskDocumentCodec().parse(
        codec::JsonOps::INSTANCE, codec::JsonValue::parse(fixture->json));
    ASSERT_TRUE(fromJson.isSuccess()) << fixture->name << ": " << fromJson.message();
    EXPECT_EQ(fromJson.result()->risks.size(), fixture->risks) << fixture->name;

    const codec::DataResult<codec::TomlDocument> parsed = codec::parseToml(fixture->toml);
    ASSERT_TRUE(parsed.isSuccess()) << fixture->name << ": " << parsed.message();
    const codec::DataResult<risk::RiskDocument> fromToml =
        risk::riskDocumentCodec().parse(codec::TomlOps::INSTANCE, parsed.result()->root());
    ASSERT_TRUE(fromToml.isSuccess()) << fixture->name << ": " << fromToml.message();
    EXPECT_EQ(*fromToml.result(), *fromJson.result()) << fixture->name;

    // Re-encoding the decoded document must reproduce the same TOML text.
    const codec::DataResult<codec::Value> again =
        risk::riskDocumentCodec().encodeStart(codec::TomlOps::INSTANCE, *fromToml.result());
    ASSERT_TRUE(again.isSuccess()) << fixture->name;
    const codec::DataResult<std::string> againText = codec::dumpToml(*again.result());
    ASSERT_TRUE(againText.isSuccess()) << fixture->name;
    EXPECT_EQ(*againText.result(), fixture->toml) << fixture->name;
  }
}

namespace {

void runComparison(const Fixture& fixture) {
  const codec::Codec<risk::RiskDocument>& documentCodec = risk::riskDocumentCodec();

  // Everything below is prepared outside the timed regions.
  const codec::DataResult<risk::RiskDocument> decoded =
      documentCodec.parse(codec::JsonOps::INSTANCE, codec::JsonValue::parse(fixture.json));
  const risk::RiskDocument& expected = *decoded.result();
  const codec::DataResult<codec::TomlDocument> parsedDocument = codec::parseToml(fixture.toml);
  const codec::JsonValue jsonInput = codec::JsonValue::parse(fixture.json);
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
  const Measurement jsonDecodeOnly = measure("JsonOps: codec decode (pre-parsed)", [&] {
    g_sink += documentCodec.parse(codec::JsonOps::INSTANCE, jsonInput).result()->risks.size();
  });

  const double baseline = tomlParse.nanosecondsPerOp;
  printRow(tomlParse, baseline);
  printRow(tomlParseAndDecode, baseline);
  printRow(tomlDecodeOnly, baseline);
  printRow(jsonDecodeOnly, baseline);

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
  const Measurement jsonEncode = measure("JsonOps: codec encode + dump", [&] {
    g_sink += documentCodec.encodeStart(codec::JsonOps::INSTANCE, expected)
                  .result()
                  ->asJson()
                  .dump()
                  .size();
  });
  printRow(codecEncode, baseline);
  printRow(encodeAndDump, baseline);
  printRow(dumpOnly, baseline);
  printRow(jsonEncode, baseline);

  printHeader("converting");
  // The handles are boxed outside the timed regions: only the conversion counts.
  const codec::Value jsonValue = jsonInput;
  const codec::Value tomlValue = parsedDocument.result()->root();
  const Measurement toToml = measure("JsonOps -> TomlOps convertTo", [&] {
    g_sink += static_cast<std::uint64_t>(
        codec::JsonOps::INSTANCE.convertTo(codec::TomlOps::INSTANCE, jsonValue).tag != nullptr);
  });
  const Measurement toJson = measure("TomlOps -> JsonOps convertTo", [&] {
    g_sink += static_cast<std::uint64_t>(
        codec::TomlOps::INSTANCE.convertTo(codec::JsonOps::INSTANCE, tomlValue).tag != nullptr);
  });
  printRow(toToml, baseline);
  printRow(toJson, baseline);

  // Per-risk cost.  With the linear builders this grows only with cache pressure
  // (the large fixture is 400x the TOML text), never with N: quadratic encoding
  // showed up here as 548 us/risk in the 400-risk fixture instead of ~20.
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
