// Example: decode a risk-definition document with the ported Codec API.
//
//   risk_def_example [path-to-json]
//
// Without an argument the sample document from the task description is used.
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "codec.hpp"
#include "risk_def.hpp"

namespace {

std::string readFile(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open " + path);
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

void printLocalized(const char* label, const risk::LocalizedText& text) {
  std::cout << "  " << label << ".cn: " << text.cn << "\n";
  std::cout << "  " << label << ".en: " << text.en << "\n";
}

void printCondition(const risk::Condition& condition, const std::string& indent) {
  if (!condition.orClauses.empty()) {
    std::cout << indent << "or:\n";
    for (const risk::Condition& clause : condition.orClauses) {
      printCondition(clause, indent + "  ");
    }
    return;
  }
  if (!condition.andClauses.empty()) {
    std::cout << indent << "and:\n";
    for (const risk::Condition& clause : condition.andClauses) {
      printCondition(clause, indent + "  ");
    }
    return;
  }
  if (!condition.notClauses.empty()) {
    std::cout << indent << "not:\n";
    for (const risk::Condition& clause : condition.notClauses) {
      printCondition(clause, indent + "  ");
    }
    return;
  }
  std::cout << indent << "param=" << condition.param.value_or("<none>")
            << " op=" << condition.op.value_or("<none>");
  if (condition.value.has_value()) {
    std::cout << " value=" << condition.value->dump();
  }
  if (condition.listMatch.has_value()) {
    std::cout << " list_match=" << *condition.listMatch;
  }
  std::cout << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  using codec::Codec;
  using codec::DataResult;
  using codec::JsonOps;
  using codec::JsonValue;

  try {
    const std::string text = argc > 1 ? readFile(argv[1]) : std::string(risk::kSampleRiskJson());
    const JsonValue input = JsonValue::parse(text);

    const Codec<risk::RiskDocument>& codec = risk::riskDocumentCodec();
    const DataResult<risk::RiskDocument> decoded = codec.parse(JsonOps::INSTANCE, input);

    if (decoded.isError()) {
      // report() is DFU's message plus the location and the chain of codecs that
      // handled the value, so the diagnostic points at the JSON:
      //
      //   decode failed:
      //   risks[3].condition.or[0].op: Not a string: 1
      //     in String
      //     in optional[op]
      //     in RecordCodec[...]
      //     ...
      std::cerr << "decode failed:\n" << decoded.report() << "\n";
      if (decoded.hasPartial()) {
        std::cerr << "partial decode produced " << decoded.valueOrPartial()->risks.size()
                  << " risk(s)\n";
      }
      return 1;
    }

    const risk::RiskDocument& document = *decoded.result();
    std::cout << "decoded " << document.risks.size() << " risk definition(s) [lifecycle: "
              << decoded.lifecycle().toString() << "]\n";
    for (const risk::RiskDef& risk : document.risks) {
      std::cout << "\n[" << risk.id << "] " << risk.vid << " (" << risk.riskType << ", "
                << risk.severity << ")\n";
      printLocalized("name", risk.name);
      printLocalized("description", risk.description);
      printLocalized("solution", risk.solution);
      std::cout << "  evidence:";
      for (const std::string& item : risk.evidence) {
        std::cout << " " << item;
      }
      std::cout << "\n";
      if (risk.condition.has_value()) {
        std::cout << "  condition:\n";
        printCondition(*risk.condition, "    ");
      }
    }

    const DataResult<JsonValue> encoded = codec.encodeStart(JsonOps::INSTANCE, document);
    if (encoded.isError()) {
      std::cerr << "encode failed:\n" << encoded.report() << "\n";
      return 1;
    }
    std::cout << "\nre-encoded:\n" << encoded.result()->dump(true, 2) << "\n";
    std::cout << "\nround-trip identical to input: "
              << (encoded.result()->dump() == input.dump() ? "yes" : "no") << "\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << "\n";
    return 2;
  }
}
