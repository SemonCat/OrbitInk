// Journal (文藝刊物) Home geometry and title wrapping. Every layout invariant is asserted at the
// built-in Latin line heights, at the X3 zh-TW profile's shared 14pt CJK fallback (one taller line
// for every UI role), and with the hero headline in the resident 28pt reader font -- and for 0, 1
// and 2 recent books -- because a layout that only holds together at one scale is a ship-able bug
// a single simulator screenshot does not show.
#include <gtest/gtest.h>

#include <string>
#include <tuple>
#include <vector>

#include "components/themes/journal/JournalHomeLayout.h"
#include "components/themes/journal/JournalText.h"

namespace {

constexpr int kW = 528;
constexpr int kH = 792;

enum class Scale { Latin, Cjk, CjkReaderHeadline };

journal::Metrics metricsFor(const Scale scale) {
  journal::Metrics m;
  m.screenWidth = kW;
  m.screenHeight = kH;
  m.sidePadding = 20;
  m.hintsHeight = 40;
  m.mastheadLine = 79;  // notoserif_28_bold advanceY
  m.mastheadWidth = 308;
  m.statusLine = 23;  // clock digits stay Latin
  if (scale == Scale::Latin) {
    // notosans_8 advanceY 23, ubuntu_10 24, ubuntu_12 29.
    m.taglineLine = 23;
    m.taglineWidth = 92;
    m.stripLine = 23;
    m.labelLine = 24;
    m.titleLine = 29;
    m.bodyLine = 24;
    m.metaLine = 23;
  } else {
    constexpr int cjk = 34;
    m.taglineLine = cjk;
    m.taglineWidth = 2 * cjk;
    m.stripLine = cjk;
    m.labelLine = cjk;
    m.titleLine = scale == Scale::CjkReaderHeadline ? 70 : cjk;
    m.bodyLine = cjk;
    m.metaLine = cjk;
  }
  return m;
}

journal::Request fullRequest(const int recentCount) {
  journal::Request r;
  r.recentCount = recentCount;
  r.heroHasAuthor = true;
  r.heroHasProgress = true;
  r.recentHasAuthor = recentCount > 0;
  r.recentHasMeta = recentCount > 0;
  return r;
}

int bottom(const Rect& r) { return r.y + r.height; }
int right(const Rect& r) { return r.x + r.width; }

class JournalLayoutScales : public ::testing::TestWithParam<std::tuple<Scale, int>> {};

TEST_P(JournalLayoutScales, SectionsAreOrderedAndNeverOverlap) {
  const auto [scale, recent] = GetParam();
  const auto m = metricsFor(scale);
  const auto l = journal::computeLayout(m, fullRequest(recent));
  ASSERT_TRUE(l.valid);

  EXPECT_GE(l.status.y, 0);
  EXPECT_LE(bottom(l.status), l.masthead.y);
  EXPECT_LE(bottom(l.masthead), l.ruleThickY);
  EXPECT_LT(l.ruleThickY + journal::kRuleThick, l.ruleThinY);
  EXPECT_LT(l.ruleThinY, l.strip.y);
  EXPECT_LE(bottom(l.strip), l.stripRuleY);
  EXPECT_LT(l.stripRuleY, l.heroLabel.y);
  // The hero's focus brackets sit kBracketOutset outside the cover: they must clear the label.
  EXPECT_LE(bottom(l.heroLabel), l.hero.y - journal::kBracketOutset + 1);
  EXPECT_LE(bottom(l.hero), l.recentLabel.y);
  EXPECT_LE(bottom(l.recentLabel), l.recentColumns[0].y);
  // The recent brackets must clear the hairline above the utility row.
  EXPECT_LT(bottom(l.recentColumns[0]) + journal::kBracketOutset, l.navRuleY);
  EXPECT_LT(l.navRuleY, l.nav.y);
  EXPECT_LE(bottom(l.nav), l.hintsRuleY);
  EXPECT_LE(l.hintsRuleY, l.hintsTop);
  EXPECT_EQ(l.hintsTop, kH - m.hintsHeight);
}

TEST_P(JournalLayoutScales, EverythingStaysInsideTheScreenAndContentColumn) {
  const auto [scale, recent] = GetParam();
  const auto m = metricsFor(scale);
  const auto l = journal::computeLayout(m, fullRequest(recent));
  ASSERT_TRUE(l.valid);
  const int contentRight = kW - m.sidePadding;
  for (const Rect& r : {l.status, l.masthead, l.strip, l.heroLabel, l.hero, l.heroCover, l.heroText, l.recentLabel,
                        l.recentColumns[0], l.recentColumns[1], l.nav}) {
    EXPECT_GE(r.x, m.sidePadding);
    EXPECT_LE(right(r), contentRight);
    EXPECT_GE(r.y, 0);
    EXPECT_LE(bottom(r), kH);
  }
  // Brackets around the recent columns and hero cover stay on the screen.
  EXPECT_GE(l.heroCover.x - journal::kBracketOutset, 0);
  EXPECT_LE(right(l.recentColumns[1]) + journal::kBracketOutset, kW);
}

TEST_P(JournalLayoutScales, HeroIsABookShapedCoverAtAboutFortyPercentWithTextAtRight) {
  const auto [scale, recent] = GetParam();
  const auto m = metricsFor(scale);
  const auto l = journal::computeLayout(m, fullRequest(recent));
  ASSERT_TRUE(l.valid);
  const int contentW = kW - 2 * m.sidePadding;
  EXPECT_GE(l.heroCover.width * 100, contentW * 30);
  EXPECT_LE(l.heroCover.width * 100, contentW * journal::kMaxHeroCoverPercent);
  // 2:3 within rounding.
  EXPECT_NEAR(l.heroCover.height * 2, l.heroCover.width * 3, 3);
  EXPECT_EQ(l.heroCover.x, l.hero.x);
  EXPECT_GE(l.heroText.x, right(l.heroCover) + journal::kHeroTextGap);
  EXPECT_EQ(right(l.heroText), right(l.hero));
}

TEST_P(JournalLayoutScales, HeroTextRowsFitTheirColumn) {
  const auto [scale, recent] = GetParam();
  const auto m = metricsFor(scale);
  const auto l = journal::computeLayout(m, fullRequest(recent));
  ASSERT_TRUE(l.valid);
  ASSERT_GE(l.heroTitleLines, 1);
  // Title, author, separator: all above the reserved progress row.
  const int separatorY = l.heroText.y + l.heroTitleLines * m.titleLine + m.bodyLine + l.heroSeparatorGap;
  ASSERT_GE(l.heroProgressY, 0);
  EXPECT_LT(separatorY, l.heroProgressY);
  EXPECT_LE(l.heroProgressY + m.bodyLine, bottom(l.heroText));
}

TEST_P(JournalLayoutScales, RecentColumnsHoldEveryRowTheyDraw) {
  const auto [scale, recent] = GetParam();
  if (recent == 0) GTEST_SKIP() << "no columns";
  const auto m = metricsFor(scale);
  const auto l = journal::computeLayout(m, fullRequest(recent));
  ASSERT_TRUE(l.valid);
  ASSERT_TRUE(l.recentHasColumns);
  const Rect& col = l.recentColumns[0];
  // Two title lines, then the author, then the bottom-anchored meta row: no row past the column.
  EXPECT_EQ(l.recentAuthorDy, l.recentTitleLines * m.bodyLine);
  EXPECT_GE(l.recentMetaDy, l.recentAuthorDy + m.metaLine);
  EXPECT_EQ(l.recentMetaDy + m.metaLine, col.height);
  EXPECT_LE(l.recentCoverHeight, col.height);
  // The columns are equal, side by side, and the divider sits between them.
  EXPECT_EQ(l.recentColumns[0].width, l.recentColumns[1].width);
  EXPECT_LT(right(l.recentColumns[0]), l.recentDividerX);
  EXPECT_LT(l.recentDividerX, l.recentColumns[1].x);
  // Brackets of adjacent columns do not touch.
  EXPECT_LT(right(l.recentColumns[0]) + journal::kBracketOutset, l.recentColumns[1].x - journal::kBracketOutset);
}

TEST_P(JournalLayoutScales, NoLargeEmptyBandOpensBetweenSections) {
  const auto [scale, recent] = GetParam();
  const auto m = metricsFor(scale);
  const auto l = journal::computeLayout(m, fullRequest(recent));
  ASSERT_TRUE(l.valid);
  // Spare height is shared between the two gaps around the recent section; neither may become a
  // band taller than two body lines.
  EXPECT_LE(l.recentLabel.y - bottom(l.hero), 2 * m.bodyLine + journal::kSectionGap);
  EXPECT_LE(l.navRuleY - bottom(l.recentColumns[0]), 2 * m.bodyLine + journal::kRecentToNav);
}

TEST_P(JournalLayoutScales, UtilityRowGivesTheLibraryEntryHalfTheWidth) {
  const auto [scale, recent] = GetParam();
  const auto m = metricsFor(scale);
  const auto l = journal::computeLayout(m, fullRequest(recent));
  ASSERT_TRUE(l.valid);
  const int contentW = kW - 2 * m.sidePadding;
  EXPECT_EQ(l.navCells[0].x, m.sidePadding);
  EXPECT_EQ(right(l.navCells[0]), l.navCells[1].x);
  EXPECT_EQ(right(l.navCells[1]), l.navCells[2].x);
  EXPECT_EQ(right(l.navCells[2]), kW - m.sidePadding);
  EXPECT_EQ(l.navCells[0].width, contentW / 2);
  EXPECT_GT(l.navCells[0].width, l.navCells[1].width);
}

INSTANTIATE_TEST_SUITE_P(ScalesAndCardinalities, JournalLayoutScales,
                         ::testing::Combine(::testing::Values(Scale::Latin, Scale::Cjk, Scale::CjkReaderHeadline),
                                            ::testing::Values(0, 1, 2)));

TEST(JournalLayout, ReaderHeadlineKeepsAtLeastTwoLinesWithTwoRecentBooks) {
  // The shipped zh-TW state: 70px reader-font headline, author, progress, two recent columns.
  const auto l = journal::computeLayout(metricsFor(Scale::CjkReaderHeadline), fullRequest(2));
  ASSERT_TRUE(l.valid);
  EXPECT_GE(l.heroTitleLines, 2);
}

TEST(JournalLayout, TaglineIsDroppedRatherThanCrowdingTheMasthead) {
  auto m = metricsFor(Scale::Cjk);
  m.taglineWidth = kW;  // cannot fit beside the masthead
  const auto l = journal::computeLayout(m, fullRequest(2));
  ASSERT_TRUE(l.valid);
  EXPECT_EQ(l.tagline.width, 0);

  const auto ok = journal::computeLayout(metricsFor(Scale::Cjk), fullRequest(2));
  ASSERT_GT(ok.tagline.width, 0);
  EXPECT_GE(ok.tagline.x, right(ok.masthead) + journal::kTaglineGap);
  EXPECT_GE(ok.tagline.y, ok.masthead.y);
  EXPECT_LE(bottom(ok.tagline), bottom(ok.masthead));
}

TEST(JournalLayout, ReportsNotFittingInsteadOfOverlapping) {
  auto m = metricsFor(Scale::Cjk);
  m.screenHeight = 360;
  EXPECT_FALSE(journal::computeLayout(m, fullRequest(2)).valid);
}

// ---- Title wrapping --------------------------------------------------------------------------

// ASCII glyphs 10px, everything else (CJK) 20px, the ellipsis 10px.
int fakeWidth(const std::string& s) {
  int w = 0;
  for (size_t i = 0; i < s.size();) {
    const auto n = journal::utf8SequenceLength(static_cast<unsigned char>(s[i]));
    w += (n == 1 || s.compare(i, n, journal::kEllipsis) == 0) ? 10 : 20;
    i += n;
  }
  return w;
}

bool validUtf8(const std::string& s) {
  for (size_t i = 0; i < s.size();) {
    const auto n = journal::utf8SequenceLength(static_cast<unsigned char>(s[i]));
    if (i + n > s.size()) return false;
    for (size_t k = 1; k < n; k++) {
      if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
    }
    i += n;
  }
  return true;
}

// 37 glyphs, no spaces: more than three 10-glyph lines.
const std::string kCjkTitle = "長夜將盡的星光圖書館關於遠方城市與一位守夜人的漫長故事以及他的十本筆記";

TEST(JournalTitleWrap, UnspacedCjkTitleUsesEveryLineInsteadOfOneTruncatedLine) {
  const auto lines = journal::wrapTitle(kCjkTitle, 200, 3, fakeWidth);  // 10 glyphs per line
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[0], "長夜將盡的星光圖書館");
  EXPECT_EQ(lines[1], "關於遠方城市與一位守");
  // Final line truncated with an ellipsis, and only the final line.
  EXPECT_NE(lines[2].find(journal::kEllipsis), std::string::npos);
  EXPECT_EQ(lines[0].find(journal::kEllipsis), std::string::npos);
  EXPECT_EQ(lines[1].find(journal::kEllipsis), std::string::npos);
  for (const auto& l : lines) {
    EXPECT_TRUE(validUtf8(l)) << l;
    EXPECT_LE(fakeWidth(l), 200) << l;
  }
}

TEST(JournalTitleWrap, ShortTitleIsOneUntouchedLine) {
  const auto lines = journal::wrapTitle("無名氏的札記", 200, 3, fakeWidth);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0], "無名氏的札記");
}

TEST(JournalTitleWrap, TitleThatFitsInTwoLinesHasNoEllipsis) {
  const std::string title = kCjkTitle.substr(0, 3 * 15);  // 15 glyphs
  const auto lines = journal::wrapTitle(title, 200, 3, fakeWidth);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0] + lines[1], title);
}

TEST(JournalTitleWrap, LatinBreaksBetweenWordsAndNeverStartsALineWithASpace) {
  const std::string title = "The Lighthouse Keeper's Almanac of Distant Harbours";
  const auto lines = journal::wrapTitle(title, 200, 4, fakeWidth);  // 20 ASCII per line
  ASSERT_GE(lines.size(), 2u);
  EXPECT_EQ(lines[0], "The Lighthouse");
  for (const auto& l : lines) {
    EXPECT_FALSE(l.empty());
    EXPECT_NE(l.front(), ' ');
    EXPECT_NE(l.back(), ' ');
    EXPECT_LE(fakeWidth(l), 200);
  }
}

TEST(JournalTitleWrap, OverlongLatinWordIsSplitAtACodepointNotDropped) {
  const auto lines = journal::wrapTitle(std::string(45, 'x'), 200, 3, fakeWidth);
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[0], std::string(20, 'x'));
  EXPECT_EQ(lines[1], std::string(20, 'x'));
  EXPECT_EQ(lines[2], std::string(5, 'x'));
}

TEST(JournalTitleWrap, MixedScriptStaysOnCodepointBoundaries) {
  const std::string title = "BookOrbit 書庫的長夜將盡星光圖書館 Vol.2 關於遠方";
  for (int w = 30; w <= 260; w += 7) {
    for (const auto& l : journal::wrapTitle(title, w, 3, fakeWidth)) {
      EXPECT_TRUE(validUtf8(l)) << "width " << w << ": " << l;
      EXPECT_LE(fakeWidth(l), std::max(w, 20)) << "width " << w;
    }
  }
}

TEST(JournalTitleWrap, DegenerateInputsReturnNothing) {
  EXPECT_TRUE(journal::wrapTitle("", 200, 3, fakeWidth).empty());
  EXPECT_TRUE(journal::wrapTitle(kCjkTitle, 0, 3, fakeWidth).empty());
  EXPECT_TRUE(journal::wrapTitle(kCjkTitle, 200, 0, fakeWidth).empty());
}

TEST(JournalTitleWrap, LongTitlesUseLogarithmicMeasurements) {
  size_t calls = 0;
  size_t measuredBytes = 0;
  const auto measure = [&](const std::string& text) {
    ++calls;
    measuredBytes += text.size();
    return fakeWidth(text);
  };
  const auto lines = journal::wrapTitle(std::string(4096, 'x'), 200, 3, measure);
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_LE(calls, 45u);
  EXPECT_LT(measuredBytes, 100000u);
  for (const auto& line : lines) EXPECT_LE(fakeWidth(line), 200);
}

}  // namespace
