// Regression coverage for the CJK/zh-TW work in the x3-zh-tw-custom build:
// Unicode decoding and CJK/kinsoku-adjacent classification, grounded in real
// codepoints observed in Edison's actual 148-book Traditional Chinese
// TrueNAS corpus (see /tmp/openclaw/crossfade-zh-tw/scratch/corpus-scan.json
// and implementation-report.md for provenance and exact counts).
//
// Scope note: ParsedText's kinsoku shori functions
// (isNoBreakBeforeCjkPunctuation / isNoBreakAfterCjkPunctuation /
// hasCjkBreakOpportunityBetween) live in an anonymous namespace in
// ParsedText.cpp -- internal linkage, unreachable from an external test
// binary -- and ParsedText's only other public entry point
// (layoutAndExtractLines) requires a working GfxRenderer, which needs
// HalDisplay/Arduino-ESP32 types this host test target cannot link. Per the
// project's own existing test scoping (test/utf8_compose, test/hyphenation_eval
// -- neither touches GfxRenderer either), the correct, honest test surface is
// lib/Utf8/Utf8.h's public classification functions: they are the exact
// primitives the untestable kinsoku logic is built from, so a regression here
// would be the first signal that the underlying classification a kinsoku rule
// depends on has broken, even though the rule itself can't be exercised
// directly from this binary.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Utf8.h"

namespace {

// Decode a UTF-8 string into its codepoints via the same decoder ParsedText
// uses internally (utf8NextCodepoint takes a pointer-to-pointer and advances
// it past each decoded codepoint).
std::vector<uint32_t> decodeAll(const std::string& s) {
  std::vector<uint32_t> out;
  const auto* p = reinterpret_cast<const unsigned char*>(s.c_str());
  while (*p) {
    out.push_back(utf8NextCodepoint(&p));
  }
  return out;
}

}  // namespace

// --- Real Traditional Chinese function words, from the actual corpus scan.
// These are the ten most frequent Han codepoints across all 148 books (見
// corpus-scan.json); "這" (U+9019) is specifically the Traditional form (vs.
// Simplified 这, U+8FD9), confirming genuinely Traditional-register content.
TEST(CjkTraditionalChinese, TopCorpusHanCharactersAreCjkBreakable) {
  const uint32_t topCorpusHan[] = {
      0x7684,  // 的 (count-leading in the corpus)
      0x6211,  // 我
      0x662F,  // 是
      0x4E00,  // 一
      0x4E0D,  // 不
      0x4ED6,  // 他
      0x5728,  // 在
      0x4E86,  // 了
      0x9019,  // 這 -- Traditional form, distinct codepoint from Simplified 这 (U+8FD9)
      0x6709,  // 有
  };
  for (const uint32_t cp : topCorpusHan) {
    EXPECT_TRUE(utf8IsCjkBreakable(cp)) << "U+" << std::hex << cp;
    EXPECT_TRUE(utf8IsCjkCodepoint(cp)) << "U+" << std::hex << cp;
  }
}

// Simplified/Traditional pair: same concept, different codepoints, both must
// classify identically as CJK -- the classifier is script-variant-agnostic by
// design (it works on Unicode blocks, not a Simplified/Traditional distinction).
TEST(CjkTraditionalChinese, SimplifiedAndTraditionalVariantsBothClassifyAsCjk) {
  struct Pair {
    uint32_t traditional;
    uint32_t simplified;
    const char* gloss;
  };
  const Pair pairs[] = {
      {0x9019, 0x8FD9, "this (這/这)"},
      {0x570B, 0x56FD, "country (國/国)"},
      {0x5B78, 0x5B66, "study (學/学)"},
  };
  for (const auto& p : pairs) {
    EXPECT_TRUE(utf8IsCjkBreakable(p.traditional)) << p.gloss << " traditional";
    EXPECT_TRUE(utf8IsCjkBreakable(p.simplified)) << p.gloss << " simplified";
  }
}

// Real CJK Unified Ideographs Extension-A codepoint actually observed in the
// corpus (U+4837, glyph 䠷, seen 16 times across the 148 books) -- confirms
// the "include Extension-A when observed" requirement is grounded in real
// data, not a synthetic example, and that the classifier (which the corpus
// coverage-rule pipeline relies on for is_han()) correctly recognizes it.
TEST(CjkTraditionalChinese, ObservedExtensionACharacterIsCjkBreakable) {
  constexpr uint32_t kObservedExtensionA = 0x4837;  // 䠷, count=16 in the real corpus
  EXPECT_TRUE(utf8IsCjkBreakable(kObservedExtensionA));
  EXPECT_TRUE(utf8IsCjkCodepoint(kObservedExtensionA));
  EXPECT_GE(kObservedExtensionA, 0x3400u);
  EXPECT_LE(kObservedExtensionA, 0x4DBFu);
}

// Punctuation the (untestable, file-local) kinsoku shori rules in
// ParsedText.cpp key off of -- see this file's header comment for why this is
// the right test boundary. If any of these ever stopped being CJK-breakable,
// the kinsoku forbidden-line-start/end logic downstream would silently lose
// its precondition for that character.
TEST(CjkTraditionalChinese, KinsokuRelevantPunctuationIsCjkBreakable) {
  const uint32_t kinsokuPunctuation[] = {
      0x3001,  // 、 ideographic comma
      0x3002,  // 。 ideographic full stop
      0x300C,  // 「 left corner bracket
      0x300D,  // 」 right corner bracket
      0x300E,  // 『 left white corner bracket
      0x300F,  // 』 right white corner bracket
      0x3008,  // 〈 left angle bracket
      0x3009,  // 〉 right angle bracket
      0x300A,  // 《 left double angle bracket
      0x300B,  // 》 right double angle bracket
      0x3010,  // 【 left black lenticular bracket
      0x3011,  // 】 right black lenticular bracket
      0xFF0C,  // ， fullwidth comma
      0xFF01,  // ！ fullwidth exclamation mark
      0xFF1F,  // ？ fullwidth question mark
  };
  for (const uint32_t cp : kinsokuPunctuation) {
    EXPECT_TRUE(utf8IsCjkBreakable(cp)) << "U+" << std::hex << cp;
  }
}

// Japanese kana and a Han-block kanji shared with Chinese -- this build does
// not add Japanese support (out of scope per the task), but the shared
// classification infrastructure must not regress for scripts this codebase
// already claims to support in its line-breaking layer.
TEST(CjkTraditionalChinese, JapaneseKanaAndKanjiAreCjkBreakable) {
  EXPECT_TRUE(utf8IsCjkBreakable(0x3042));  // あ hiragana A
  EXPECT_TRUE(utf8IsCjkBreakable(0x30A2));  // ア katakana A
  EXPECT_TRUE(utf8IsCjkBreakable(0x65E5));  // 日 kanji "sun/day", shared Han codepoint
}

// Korean Hangul -- also out of scope for this build's font asset (see
// implementation-report.md), but utf8IsCjkBreakable already claims Hangul
// support in the line-breaking layer (lib/Utf8/Utf8.h's own range comment);
// this guards that claim from silently regressing.
TEST(CjkTraditionalChinese, KoreanHangulIsCjkBreakable) {
  EXPECT_TRUE(utf8IsCjkBreakable(0xD55C));  // 한 Hangul syllable "han"
  EXPECT_TRUE(utf8IsCjkBreakable(0x1100));  // Hangul Jamo (leading consonant)
}

// Negative check: ordinary Latin text must not be misclassified as CJK --
// guards against an over-broadened range accidentally routing every UI
// fallback decision through the CJK path.
TEST(CjkTraditionalChinese, LatinAsciiIsNotCjk) {
  for (char c = 'A'; c <= 'Z'; ++c) {
    EXPECT_FALSE(utf8IsCjkBreakable(static_cast<uint32_t>(c)));
    EXPECT_FALSE(utf8IsCjkCodepoint(static_cast<uint32_t>(c)));
  }
}

// Whole-sentence UTF-8 decode round-trip: a real-shaped Traditional Chinese
// sentence ("這是一本書" -- "This is a book"), five 3-byte-UTF-8 Han
// characters back to back, no spaces (as real CJK prose has none) -- the
// exact shape ParsedText::cjkCharacterBreakByteOffsets must tokenize
// correctly, decoded here through the same utf8NextCodepoint() primitive it
// uses.
TEST(CjkTraditionalChinese, DecodesRealTraditionalChineseSentence) {
  // 這 是 一 本 書
  const std::string sentence = "\xE9\x80\x99\xE6\x98\xAF\xE4\xB8\x80\xE6\x9C\xAC\xE6\x9B\xB8";
  const std::vector<uint32_t> expected = {0x9019, 0x662F, 0x4E00, 0x672C, 0x66F8};
  EXPECT_EQ(decodeAll(sentence), expected);
  for (const uint32_t cp : expected) {
    EXPECT_TRUE(utf8IsCjkBreakable(cp));
  }
}
