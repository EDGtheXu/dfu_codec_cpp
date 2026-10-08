// Performance comparison: the ported Codec layer vs nlohmann/json used directly.
//
// This layer is a *measurement*, not a correctness gate: the only assertions are
// that every strategy produces the same result (otherwise the numbers would be
// meaningless).  It prints a table you can read by running the executable
// directly or `ctest -R perf -V`:
//
//   cmake-build-debug/test/perf/codec_perf_tests.exe
//
// What is measured, on the risk-definition document from the task description:
//
//   * nlohmann::json::parse          -- bare parse, unordered objects
//   * nlohmann::ordered_json::parse  -- bare parse, the DOM JsonValue wraps
//   * ordered parse + manual extract -- hand-written nlohmann field extraction
//   * codec decode (pre-parsed)      -- the Codec layer on an existing JsonValue
//   * JsonValue::parse + codec       -- the full text -> struct path
//   * codec encode + dump            -- struct -> text through the Codec layer
//   * manual build + dump            -- hand-written nlohmann building/serialising
//
// No timing assertion is made on purpose: a benchmark that fails on a busy CI
// machine is worse than no benchmark.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "risk_def.hpp"
#include "test_support.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using risk::Condition;
using risk::LocalizedText;
using risk::RiskDef;
using risk::RiskDocument;

// Keeps the optimiser honest: every measured function feeds this sink.
std::uint64_t g_sink = 0;

struct Measurement {
  std::string label;
  double nanosecondsPerOp = 0.0;
  double operationsPerSecond = 0.0;
  int iterations = 0;
};

int rounds() {
#ifdef NDEBUG
  return 5;
#else
  return 3;  // Debug builds are ~30x slower; keep the layer fast
#endif
}

// Time budget per measurement, so that a 600 KiB fixture and a 2 KiB fixture
// both stay fast enough for CI.
constexpr double kCalibrationMilliseconds = 5.0;
constexpr double kRoundMilliseconds = 40.0;

// Runs `body` and reports the best per-operation time of `rounds()` runs.
// The iteration count is calibrated per measurement: the operation is timed
// once, then repeated enough times to fill the per-round budget.
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
  const int count = std::clamp(static_cast<int>(kRoundMilliseconds * 1e6 / std::max(probeNanoseconds, 1.0)),
                               3, 20000);

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
// The hand-written baseline: nlohmann/json alone, no Codec layer.
// Note that it does no validation (missing keys throw, wrong types throw, ranges
// are not checked) -- the Codec layer provides all of that for free.
// ---------------------------------------------------------------------------
Condition extractCondition(const nlohmann::ordered_json& raw) {
  Condition condition;
  if (raw.contains("or")) {
    for (const auto& clause : raw.at("or")) {
      condition.orClauses.push_back(extractCondition(clause));
    }
  }
  if (raw.contains("and")) {
    for (const auto& clause : raw.at("and")) {
      condition.andClauses.push_back(extractCondition(clause));
    }
  }
  if (raw.contains("not")) {
    for (const auto& clause : raw.at("not")) {
      condition.notClauses.push_back(extractCondition(clause));
    }
  }
  if (raw.contains("param")) {
    condition.param = raw.at("param").get<std::string>();
  }
  if (raw.contains("op")) {
    condition.op = raw.at("op").get<std::string>();
  }
  if (raw.contains("value")) {
    condition.value = codec::JsonValue(raw.at("value"));
  }
  if (raw.contains("list_match")) {
    condition.listMatch = raw.at("list_match").get<std::string>();
  }
  return condition;
}

LocalizedText extractLocalized(const nlohmann::ordered_json& raw) {
  return LocalizedText{raw.at("cn").get<std::string>(), raw.at("en").get<std::string>()};
}

RiskDocument extractManually(const nlohmann::ordered_json& document) {
  RiskDocument out;
  for (const auto& rawRisk : document.at("risks")) {
    RiskDef risk;
    risk.id = rawRisk.at("id").get<std::string>();
    risk.vid = rawRisk.at("vid").get<std::string>();
    risk.riskType = rawRisk.at("risk_type").get<std::string>();
    risk.severity = rawRisk.at("severity").get<std::string>();
    risk.name = extractLocalized(rawRisk.at("name"));
    risk.description = extractLocalized(rawRisk.at("description"));
    risk.solution = extractLocalized(rawRisk.at("solution"));
    for (const auto& item : rawRisk.at("evidence")) {
      risk.evidence.push_back(item.get<std::string>());
    }
    if (rawRisk.contains("condition")) {
      risk.condition = extractCondition(rawRisk.at("condition"));
    }
    out.risks.push_back(std::move(risk));
  }
  return out;
}

// Hand-written encoding, for the serialisation comparison.
nlohmann::ordered_json buildManually(const RiskDocument& document) {
  nlohmann::ordered_json out;
  out["risks"] = nlohmann::ordered_json::array();
  for (const RiskDef& risk : document.risks) {
    nlohmann::ordered_json rawRisk;
    rawRisk["id"] = risk.id;
    rawRisk["vid"] = risk.vid;
    rawRisk["risk_type"] = risk.riskType;
    rawRisk["severity"] = risk.severity;
    rawRisk["name"] = {{"cn", risk.name.cn}, {"en", risk.name.en}};
    rawRisk["description"] = {{"cn", risk.description.cn}, {"en", risk.description.en}};
    rawRisk["solution"] = {{"cn", risk.solution.cn}, {"en", risk.solution.en}};
    if (risk.condition.has_value()) {
      const Condition& condition = *risk.condition;
      nlohmann::ordered_json rawCondition;
      if (!condition.orClauses.empty()) {
        rawCondition["or"] = nlohmann::ordered_json::array();
        for (const Condition& clause : condition.orClauses) {
          nlohmann::ordered_json rawClause;
          if (clause.param.has_value()) {
            rawClause["param"] = *clause.param;
          }
          if (clause.op.has_value()) {
            rawClause["op"] = *clause.op;
          }
          if (clause.value.has_value()) {
            rawClause["value"] = clause.value->raw();
          }
          rawCondition["or"].push_back(rawClause);
        }
      }
      if (condition.param.has_value()) {
        rawCondition["param"] = *condition.param;
      }
      if (condition.op.has_value()) {
        rawCondition["op"] = *condition.op;
      }
      if (condition.value.has_value()) {
        rawCondition["value"] = condition.value->raw();
      }
      if (condition.listMatch.has_value()) {
        rawCondition["list_match"] = *condition.listMatch;
      }
      rawRisk["condition"] = rawCondition;
    }
    rawRisk["evidence"] = risk.evidence;
    out["risks"].push_back(std::move(rawRisk));
  }
  return out;
}

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------
std::string largeDocumentText(int riskCount) {
  const nlohmann::ordered_json sample = nlohmann::ordered_json::parse(risk::kSampleRiskJson());
  nlohmann::ordered_json document;
  document["risks"] = nlohmann::ordered_json::array();
  for (int i = 0; i < riskCount; ++i) {
    document["risks"].push_back(sample.at("risks").at(i % sample.at("risks").size()));
  }
  return document.dump();
}

struct Fixture {
  std::string name;
  std::string text;
  std::size_t bytes = 0;
  std::size_t risks = 0;
};

std::vector<Fixture> fixtures() {
  const std::string small = risk::kSampleRiskJson();
  const std::string large = largeDocumentText(400);
  return {Fixture{"2 risks", small, small.size(), 2},
          Fixture{"400 risks", large, large.size(), 400}};
}

Fixture smallFixture() { return fixtures()[0]; }
Fixture largeFixture() { return fixtures()[1]; }

// ---------------------------------------------------------------------------
// Correctness guard -- the benchmark only means something if all three
// strategies agree on the fixture.
// ---------------------------------------------------------------------------
RiskDocument codecDecode(const std::string& text) {
  return codec::testing::decode(risk::riskDocumentCodec(), text);
}

TEST(PerfTest, StrategiesAgreeOnTheFixture) {
  for (const Fixture& fixture : fixtures()) {
    const RiskDocument viaCodec = codecDecode(fixture.text);
    const RiskDocument viaManual = extractManually(nlohmann::ordered_json::parse(fixture.text));
    ASSERT_EQ(viaCodec.risks.size(), fixture.risks) << fixture.name;
    EXPECT_EQ(viaCodec, viaManual) << fixture.name;
    EXPECT_EQ(risk::riskDocumentCodec()
                  .encodeStart(codec::JsonOps::INSTANCE, viaCodec)
                  .result()
                  ->dump(),
              buildManually(viaCodec).dump())
        << fixture.name;
  }
}

// ---------------------------------------------------------------------------
// Measurements -- one test per fixture, so they can be run individually
// (`--gtest_filter=PerfTest.SmallDocument`) and appear separately in CTest.
// ---------------------------------------------------------------------------
void runComparison(const Fixture& fixture) {
  // Codecs are built once, like an application would: copying a Codec handle
  // deep-copies its captured field table, which must not be charged to a
  // single decode.
  const codec::Codec<RiskDocument>& documentCodec = risk::riskDocumentCodec();

  const nlohmann::ordered_json parsedOrdered = nlohmann::ordered_json::parse(fixture.text);
  const codec::JsonValue parsedValue = codec::JsonValue::parse(fixture.text);
  const RiskDocument expected = codecDecode(fixture.text);

  std::printf("\n-------------------------------------------------------------------------\n");
  std::printf(" fixture: %s (%zu bytes) -- %s build, best of %d rounds\n", fixture.name.c_str(),
              fixture.bytes,
#ifdef NDEBUG
              "Release",
#else
              "Debug",
#endif
              rounds());
  std::printf("-------------------------------------------------------------------------\n");

    printHeader("decoding");
    const Measurement plainParse = measure("nlohmann::json::parse", [&] {
      const nlohmann::json parsed = nlohmann::json::parse(fixture.text);
      g_sink += parsed.size();
    });
    const Measurement orderedParse = measure("nlohmann::ordered_json::parse", [&] {
      const nlohmann::ordered_json parsed = nlohmann::ordered_json::parse(fixture.text);
      g_sink += parsed.size();
    });
    const Measurement parseAndManual =
        measure("ordered_json::parse + manual extraction", [&] {
          g_sink += extractManually(nlohmann::ordered_json::parse(fixture.text)).risks.size();
        });
    const Measurement parseAndCodec = measure("JsonValue::parse + codec decode", [&] {
      g_sink += documentCodec.parse(codec::JsonOps::INSTANCE, codec::JsonValue::parse(fixture.text))
                    .result()
                    ->risks.size();
    });
    const Measurement manualOnly = measure("manual extraction (pre-parsed)", [&] {
      g_sink += extractManually(parsedOrdered).risks.size();
    });
    const Measurement codecOnly = measure("codec decode (pre-parsed)", [&] {
      g_sink += documentCodec.parse(codec::JsonOps::INSTANCE, parsedValue).result()->risks.size();
    });

    const double baseline = orderedParse.nanosecondsPerOp;
    printRow(plainParse, baseline);
    printRow(orderedParse, baseline);
    printRow(parseAndManual, baseline);
    printRow(parseAndCodec, baseline);
    printRow(manualOnly, baseline);
    printRow(codecOnly, baseline);

    printHeader("encoding");
    const Measurement codecEncode = measure("codec encode + dump", [&] {
      g_sink += documentCodec.encodeStart(codec::JsonOps::INSTANCE, expected)
                    .result()
                    ->dump()
                    .size();
    });
    const Measurement manualEncode = measure("manual build + dump", [&] {
      g_sink += buildManually(expected).dump().size();
    });
    const Measurement parseDump = measure("ordered_json::parse + dump", [&] {
      g_sink += nlohmann::ordered_json::parse(fixture.text).dump().size();
    });
    printRow(codecEncode, baseline);
    printRow(manualEncode, baseline);
    printRow(parseDump, baseline);

    std::printf("\n  decode overhead of the Codec layer over a bare ordered_json parse: %.2fx\n",
                parseAndCodec.nanosecondsPerOp / baseline);
    std::printf("  decode overhead of the Codec layer over manual extraction:       %.2fx\n",
                codecOnly.nanosecondsPerOp / manualOnly.nanosecondsPerOp);
    std::printf("  encode: codec %.0f ns/op vs manual %.0f ns/op (%.2fx)\n",
                codecEncode.nanosecondsPerOp, manualEncode.nanosecondsPerOp,
                codecEncode.nanosecondsPerOp / manualEncode.nanosecondsPerOp);
    std::printf("  per-field decode cost: %.2f us/field (%zu fields)\n",
                codecOnly.nanosecondsPerOp / 1000.0 / static_cast<double>(fixture.risks * 11),
                static_cast<size_t>(fixture.risks * 11));
}

TEST(PerfTest, SmallDocument) {
  runComparison(smallFixture());
  std::printf("\n(checksum %llu -- keeps the measured work observable)\n",
              static_cast<unsigned long long>(g_sink));
}

TEST(PerfTest, LargeDocument) {
  runComparison(largeFixture());
  std::printf("\n(checksum %llu -- keeps the measured work observable)\n",
              static_cast<unsigned long long>(g_sink));
}

}  // namespace
