#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "TextWrap.h"
#include "Utf8.h"

// The optimization under test replaces a quadratic break search (re-measure the whole candidate
// prefix once per candidate) with a single forward scan. Correctness here is not "looks
// reasonable" -- TXT reading progress is stored as a byte offset into the file, so a break
// position that differs by even one codepoint moves which page a saved offset resumes on. Every
// test below therefore compares against a reference implementation of the ORIGINAL algorithm,
// driven by the same synthetic advance table.
namespace {

// Stand-in for an SD card font's advance table: fixed-width CJK, narrower Latin, a couple of
// zero-advance combining marks. Values are 4.4 fixed-point, as SdCardFont::getAdvance returns.
// Deliberately not round pixel multiples, so pixel snapping actually bites.
int32_t fakeAdvanceFp(void* /*ctx*/, const uint32_t cp) {
  if (cp == 0x0301 || cp == 0x0300) return 0;  // combining acute/grave: zero-advance overlay
  if (cp == ' ') return 0x38;                  // 3.5px
  if (cp < 0x80) return 0x71;                  // 7.0625px -- Latin
  return 0x0100;                               // 16px -- CJK, full width
}

// Mirrors GfxRenderer::getTextAdvanceX()'s SD advance-table branch: sum the per-codepoint
// fixed-point advances, snap to pixels exactly once at the end.
int measurePrefixPx(const std::string& text) {
  int32_t widthFp = 0;
  const auto* cursor = reinterpret_cast<const unsigned char*>(text.c_str());
  while (const uint32_t cp = utf8NextCodepoint(&cursor)) {
    widthFp += fakeAdvanceFp(nullptr, cp);
  }
  return (widthFp + 8) >> 4;  // fp4::toPixel
}

// The original TxtReaderActivity::loadPage() break search, verbatim in structure.
size_t referenceBreakPos(const std::string& line, const int viewportWidth) {
  size_t breakPos = line.length();
  while (breakPos > 0 && measurePrefixPx(line.substr(0, breakPos)) > viewportWidth) {
    const size_t spacePos = line.rfind(' ', breakPos - 1);
    if (spacePos != std::string::npos && spacePos > 0) {
      breakPos = spacePos;
    } else {
      breakPos--;
      while (breakPos > 0 && (line[breakPos] & 0xC0) == 0x80) {
        breakPos--;
      }
    }
  }
  if (breakPos == 0) {
    breakPos = 1;
  }
  return breakPos;
}

size_t fastBreakPos(const std::string& line, const int viewportWidth) {
  size_t fitBytes = 0;
  EXPECT_TRUE(textwrap::longestFittingPrefix(line, viewportWidth, &fakeAdvanceFp, nullptr, fitBytes));
  return textwrap::breakPosFromFit(line, fitBytes);
}

// Only meaningful for lines that do not already fit -- that is the only state in which the
// caller runs a break search at all.
void expectSameBreak(const std::string& line, const int viewportWidth) {
  ASSERT_GT(measurePrefixPx(line), viewportWidth) << "test input already fits; nothing to break";
  EXPECT_EQ(fastBreakPos(line, viewportWidth), referenceBreakPos(line, viewportWidth))
      << "line=[" << line << "] viewport=" << viewportWidth;
}

std::string repeat(const std::string& unit, const int times) {
  std::string out;
  for (int i = 0; i < times; ++i) out += unit;
  return out;
}

}  // namespace

// The target case: a long Chinese line with no spaces at all, where the original search walked
// back one codepoint per iteration re-measuring the whole prefix each time.
TEST(TextWrap, ChineseLineWithNoSpacesMatchesReference) {
  const std::string line = repeat("中文測試", 40);
  for (int viewport = 16; viewport < measurePrefixPx(line); viewport += 7) {
    expectSameBreak(line, viewport);
  }
}

TEST(TextWrap, LatinLineBreaksAtSpaceLikeReference) {
  const std::string line = "the quick brown fox jumps over the lazy dog again and again and again";
  for (int viewport = 20; viewport < measurePrefixPx(line); viewport += 3) {
    expectSameBreak(line, viewport);
  }
}

// Mixed content is where a prefix scan could most plausibly diverge from a per-prefix search.
TEST(TextWrap, MixedLatinAndCjkMatchesReference) {
  const std::string line = "Chapter 3 第三章 the beginning 開始了 and then 然後呢 end";
  for (int viewport = 16; viewport < measurePrefixPx(line); viewport += 5) {
    expectSameBreak(line, viewport);
  }
}

// A space exists but sits beyond the fitting prefix: the original walked down over it and then
// fell through to a codepoint-at-a-time descent.
TEST(TextWrap, SpaceBeyondFitFallsBackToCodepointBreak) {
  const std::string line = repeat("測", 30) + " tail";
  for (int viewport = 16; viewport < measurePrefixPx(line); viewport += 4) {
    expectSameBreak(line, viewport);
  }
}

// A leading space is at index 0, which the original explicitly refused to break at
// (`spacePos > 0`).
TEST(TextWrap, LeadingSpaceIsNeverTheBreakPoint) {
  const std::string line = " " + repeat("字", 20);
  for (int viewport = 16; viewport < measurePrefixPx(line); viewport += 4) {
    const size_t breakPos = fastBreakPos(line, viewport);
    EXPECT_NE(breakPos, 0u);
    expectSameBreak(line, viewport);
  }
}

// Zero-advance combining marks must not consume width, and must never end up split from the
// base character by being counted as a fitting boundary of their own... which they are, at the
// same width -- so the check that matters is simply that both algorithms agree.
TEST(TextWrap, CombiningMarksMatchReference) {
  const std::string line = repeat("e\xCC\x81", 25) + " " + repeat("a\xCC\x80", 25);
  for (int viewport = 16; viewport < measurePrefixPx(line); viewport += 3) {
    expectSameBreak(line, viewport);
  }
}

// Degenerate: a viewport too narrow for even one glyph. The original clamped to 1 byte, which
// can split a multi-byte codepoint; that behavior is preserved deliberately.
TEST(TextWrap, ViewportNarrowerThanOneGlyphClampsToOneByte) {
  const std::string line = repeat("漢", 5);
  EXPECT_EQ(fastBreakPos(line, 1), 1u);
  EXPECT_EQ(referenceBreakPos(line, 1), 1u);
  EXPECT_EQ(fastBreakPos(line, 0), 1u);
  EXPECT_EQ(referenceBreakPos(line, 0), 1u);
}

TEST(TextWrap, EmptyLineHasNoBreak) {
  size_t fitBytes = 123;
  EXPECT_TRUE(textwrap::longestFittingPrefix("", 100, &fakeAdvanceFp, nullptr, fitBytes));
  EXPECT_EQ(fitBytes, 0u);
  EXPECT_EQ(textwrap::breakPosFromFit("", fitBytes), 0u);
}

// A whole line that fits has every byte in the fitting prefix, so the caller's earlier
// "does it fit?" check and this scan agree on the boundary case.
TEST(TextWrap, FittingLineReportsItsFullLength) {
  const std::string line = "short";
  size_t fitBytes = 0;
  ASSERT_TRUE(textwrap::longestFittingPrefix(line, measurePrefixPx(line), &fakeAdvanceFp, nullptr, fitBytes));
  EXPECT_EQ(fitBytes, line.size());
}

// A negative advance would break the monotonicity the single scan relies on, so the helper must
// refuse rather than return a wrong answer. (The real callback cannot produce one -- both of its
// sources are unsigned -- but the contract is what keeps that true if a source ever changes.)
TEST(TextWrap, NegativeAdvanceAbandonsTheFastPath) {
  const auto negative = [](void*, uint32_t) -> int32_t { return -1; };
  size_t fitBytes = 999;
  EXPECT_FALSE(textwrap::longestFittingPrefix("abc", 100, negative, nullptr, fitBytes));
}

// Broad randomized agreement over the shapes real TXT files mix: CJK runs, Latin words, spaces,
// and combining marks, at many viewport widths.
TEST(TextWrap, RandomizedAgreementWithReference) {
  std::mt19937 rng(20260905);
  const std::vector<std::string> alphabet = {"漢", "字", "測", "試", "a", "b", "z", " ", " ", "e\xCC\x81", "1"};
  std::uniform_int_distribution<size_t> pick(0, alphabet.size() - 1);
  std::uniform_int_distribution<int> lengthDist(1, 60);
  std::uniform_int_distribution<int> viewportDist(1, 260);

  for (int iteration = 0; iteration < 3000; ++iteration) {
    std::string line;
    const int length = lengthDist(rng);
    for (int i = 0; i < length; ++i) line += alphabet[pick(rng)];
    const int viewport = viewportDist(rng);
    if (measurePrefixPx(line) <= viewport) continue;  // no break search would run
    ASSERT_EQ(fastBreakPos(line, viewport), referenceBreakPos(line, viewport))
        << "line=[" << line << "] viewport=" << viewport;
  }
}
