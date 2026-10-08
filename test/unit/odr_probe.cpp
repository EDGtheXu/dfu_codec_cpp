// A *second* translation unit that includes the single library header plus the
// models header.  The unit test binary therefore contains every inline definition
// twice, which is exactly the situation a header-only library has to survive:
//
//   * a definition that is missing `inline` makes the linker report a duplicate
//     symbol (LNK2005 / "multiple definition") before any test runs;
//   * a function-local `static` inside a non-inline function would silently give
//     each translation unit its own instance, so `odr_test.cpp` compares the
//     addresses with the ones it sees itself.
#include <string>

#include "codec_json.hpp"
#include "risk_def.hpp"

namespace codec_odr {

const void* riskDocumentCodecAddress() { return &risk::riskDocumentCodec(); }
const void* conditionCodecAddress() { return &risk::conditionCodec(); }
const void* localizedTextCodecAddress() { return &risk::localizedTextCodec(); }
const void* jsonOpsInstanceAddress() { return &codec::JsonOps::INSTANCE; }
const void* jsonOpsCompressedAddress() { return &codec::JsonOps::COMPRESSED; }
const void* intCodecAddress() { return &codec::codecs::Int; }

// Decodes from this translation unit, so the test also proves that the shared
// codec instance still works when called through another TU.
std::string decodeRiskDocument(const std::string& text) {
  const codec::DataResult<risk::RiskDocument> result =
      risk::riskDocumentCodec().parse(codec::JsonOps::INSTANCE, codec::JsonValue::parse(text));
  if (result.isError()) {
    return "error: " + result.message();
  }
  return std::to_string(result.result()->risks.size());
}

}  // namespace codec_odr
