// Guards the "header-only" promise of the library.
//
// `odr_probe.cpp` is a second translation unit that includes every header (plus
// models/risk_def.hpp), so this binary links all of those definitions twice.  If
// any definition were missing `inline` the link would fail; and if a singleton
// were per-translation-unit instead of shared, the addresses below would differ.
#include "risk_def.hpp"
#include "test_support.hpp"

namespace codec_odr {

const void* riskDocumentCodecAddress();
const void* conditionCodecAddress();
const void* localizedTextCodecAddress();
const void* jsonOpsInstanceAddress();
const void* jsonOpsCompressedAddress();
const void* intCodecAddress();
std::string decodeRiskDocument(const std::string& text);

}  // namespace codec_odr

namespace {

TEST(OdrTest, LibraryIsHeaderOnlyAndSingletonsAreSharedAcrossTranslationUnits) {
  // Linking is the first assertion: every inline definition above is present in
  // two TUs of this executable.
  EXPECT_EQ(static_cast<const void*>(&risk::riskDocumentCodec()),
            codec_odr::riskDocumentCodecAddress());
  EXPECT_EQ(static_cast<const void*>(&risk::conditionCodec()), codec_odr::conditionCodecAddress());
  EXPECT_EQ(static_cast<const void*>(&risk::localizedTextCodec()),
            codec_odr::localizedTextCodecAddress());
  EXPECT_EQ(static_cast<const void*>(&codec::JsonOps::INSTANCE),
            codec_odr::jsonOpsInstanceAddress());
  EXPECT_EQ(static_cast<const void*>(&codec::JsonOps::COMPRESSED),
            codec_odr::jsonOpsCompressedAddress());
  EXPECT_EQ(static_cast<const void*>(&codec::codecs::Int), codec_odr::intCodecAddress());
}

TEST(OdrTest, CodecsBuiltInOneTranslationUnitWorkFromAnother) {
  EXPECT_EQ(codec_odr::decodeRiskDocument(risk::kSampleRiskJson()), "2");

  const risk::RiskDocument document =
      codec::testing::decode(risk::riskDocumentCodec(), risk::kSampleRiskJson());
  const std::string encoded = codec::testing::encode(risk::riskDocumentCodec(), document);
  EXPECT_EQ(codec_odr::decodeRiskDocument(encoded), "2");
}

TEST(OdrTest, InlineConstantsAndFactoriesAreUsableFromEveryTranslationUnit) {
  // Primitives live in `codecs::` as inline variables; factories are templates.
  const codec::Codec<std::vector<int32_t>> listCodec = codec::listOf(codec::codecs::Int);
  EXPECT_EQ(codec_odr::intCodecAddress() == static_cast<const void*>(&codec::codecs::Int), true);
  EXPECT_EQ(codec::testing::encode(listCodec, std::vector<int32_t>{1, 2}), "[1,2]");
  EXPECT_EQ(codec::testing::decode(listCodec, "[3]"), std::vector<int32_t>({3}));
}

}  // namespace
