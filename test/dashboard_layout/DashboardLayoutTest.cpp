// Geometry for the Dashboard Home card (src/components/themes/dashboard/DashboardLayout.h).
//
// The point of these tests is the X3 zh-TW profile: there SMALL/UI_10/UI_12 all resolve to one
// shared 14pt CJK SD font, so every line is taller and every caption is wider than in the built-in
// Latin build a simulator screenshot shows. Each layout invariant is therefore asserted at BOTH
// metric scales -- a card that only holds together at Latin metrics is exactly the bug this file
// exists to catch, and it is invisible everywhere else in the gate list.
//
// There is nothing here about a progress bar or a percentage, and that is deliberate: the card has
// neither, because every route to a truthful book-level percentage opens the book and Home does
// not open books. See the header's own comment.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "components/themes/dashboard/DashboardLayout.h"

namespace {

// The X3 panel in the firmware's default portrait orientation, and the band HomeActivity hands the
// theme with DashboardMetrics' own homeTopPadding / homeCoverTileHeight.
constexpr int kScreenWidth = 528;
constexpr int kTileY = 44;
constexpr int kTileHeight = 420;

Rect tile() { return Rect{0, kTileY, kScreenWidth, kTileHeight}; }

// Built-in Latin roles: notosans_8 advanceY 23, ubuntu_10 advanceY 24, ubuntu_12 advanceY 29
// (lib/EpdFont/builtinFonts/*). Kept as literals rather than read from the font headers: the point
// is to pin the layout against a realistic scale, not to re-derive the fonts.
dashboard::CardMetrics latinMetrics() {
  dashboard::CardMetrics m;
  m.pillTextWidth = 132;  // "Continue Reading" at UI_10 bold
  m.pillLineHeight = 24;
  m.titleLineHeight = 29;
  m.authorLineHeight = 29;
  m.panelHeadingLineHeight = 24;
  m.statLabelLineHeight = 23;
  m.statValueLineHeight = 24;
  return m;
}

// Every UI role falling back to the single 14pt CJK SD font: one taller line height everywhere,
// and the pill measured as the four full-width glyphs the translated label ("繼續閱讀") actually
// is, rather than the narrower Latin "Continue Reading".
dashboard::CardMetrics cjkMetrics(const int lineHeight = 34) {
  dashboard::CardMetrics m;
  m.pillTextWidth = 4 * lineHeight;
  m.pillLineHeight = lineHeight;
  m.titleLineHeight = lineHeight;
  m.authorLineHeight = lineHeight;
  m.panelHeadingLineHeight = lineHeight;
  m.statLabelLineHeight = lineHeight;
  m.statValueLineHeight = lineHeight;
  return m;
}

dashboard::CardRequest request(const int titleLines, const bool hasAuthor, const bool hasStats) {
  dashboard::CardRequest r;
  r.titleLines = titleLines;
  r.hasAuthor = hasAuthor;
  r.hasStats = hasStats;
  return r;
}

bool contains(const Rect& outer, const Rect& inner) {
  return inner.x >= outer.x && inner.y >= outer.y && inner.x + inner.width <= outer.x + outer.width &&
         inner.y + inner.height <= outer.y + outer.height;
}

}  // namespace

class DashboardLayoutScales : public testing::TestWithParam<dashboard::CardMetrics> {};

INSTANTIATE_TEST_SUITE_P(LatinAndCjk, DashboardLayoutScales,
                         testing::Values(latinMetrics(), cjkMetrics(30), cjkMetrics(34), cjkMetrics(38)));

TEST_P(DashboardLayoutScales, CardNeverOverflowsTheBand) {
  for (int lines = 1; lines <= dashboard::kMaxTitleLines; ++lines) {
    for (const bool author : {false, true}) {
      for (const bool stats : {false, true}) {
        const auto l = dashboard::computeCardLayout(tile(), GetParam(), request(lines, author, stats));
        EXPECT_TRUE(contains(tile(), l.card)) << "lines=" << lines << " author=" << author << " stats=" << stats;
        EXPECT_GT(l.card.width, 0);
        EXPECT_GT(l.card.height, 0);
      }
    }
  }
}

TEST_P(DashboardLayoutScales, PillIconTileAndPanelAllStayInsideTheCard) {
  const auto l = dashboard::computeCardLayout(tile(), GetParam(), request(2, true, true));
  EXPECT_TRUE(contains(l.card, l.pill));
  EXPECT_TRUE(contains(l.card, l.iconTile));
  ASSERT_GT(l.panel.height, 0);
  EXPECT_TRUE(contains(l.card, l.panel));
}

TEST_P(DashboardLayoutScales, PillNeverRunsIntoTheIconTile) {
  const auto l = dashboard::computeCardLayout(tile(), GetParam(), request(1, true, true));
  EXPECT_LT(l.pill.x + l.pill.width, l.iconTile.x);
}

TEST_P(DashboardLayoutScales, TitleBlockAndAuthorLineClearTheStatsPanel) {
  const auto m = GetParam();
  for (int lines = 1; lines <= dashboard::kMaxTitleLines; ++lines) {
    const auto l = dashboard::computeCardLayout(tile(), m, request(lines, true, true));
    ASSERT_GE(l.maxTitleLines, 1) << "lines=" << lines;
    const int identityBottom = dashboard::authorLineY(l, m, l.maxTitleLines) + m.authorLineHeight;
    if (l.panel.height > 0) {
      EXPECT_LE(identityBottom, l.panel.y) << "lines=" << lines << " kept=" << l.maxTitleLines;
    } else {
      EXPECT_LE(identityBottom, l.card.y + l.card.height) << "lines=" << lines;
    }
  }
}

TEST_P(DashboardLayoutScales, PanelRowsStayInsideThePanel) {
  const auto m = GetParam();
  const auto l = dashboard::computeCardLayout(tile(), m, request(1, true, true));
  ASSERT_GT(l.panel.height, 0);
  const int panelBottom = l.panel.y + l.panel.height;
  EXPECT_GE(l.panelHeadingY, l.panel.y);
  EXPECT_GT(l.statLabelY, l.panelHeadingY);
  EXPECT_LE(l.statValueY + m.statValueLineHeight, panelBottom);
  EXPECT_GT(l.statAreaWidth, 0);
  EXPECT_LE(l.panelContentX + l.statAreaWidth, l.panelContentRight);
}

TEST_P(DashboardLayoutScales, TitleNeverExceedsTheDesignMaximum) {
  const auto l = dashboard::computeCardLayout(tile(), GetParam(), request(dashboard::kMaxTitleLines, true, true));
  EXPECT_LE(l.maxTitleLines, dashboard::kMaxTitleLines);
  EXPECT_GE(l.maxTitleLines, 1);
}

TEST_P(DashboardLayoutScales, EmptyStateCardFillsTheWholeBand) {
  const auto card = dashboard::emptyCardRect(tile());
  EXPECT_TRUE(contains(tile(), card));
  EXPECT_EQ(card.height, kTileHeight - dashboard::kCardTopGap * 2);
}

// --- The card is sized to its content, not to the band -----------------------------------------

TEST(DashboardCardHeight, GrowsWithEachExtraTitleLine) {
  const auto m = latinMetrics();
  const int one = dashboard::computeCardLayout(tile(), m, request(1, true, true)).card.height;
  const int two = dashboard::computeCardLayout(tile(), m, request(2, true, true)).card.height;
  EXPECT_EQ(two - one, m.titleLineHeight);
}

TEST(DashboardCardHeight, ShrinksWhenThereIsNoAuthor) {
  const auto m = latinMetrics();
  const int withAuthor = dashboard::computeCardLayout(tile(), m, request(1, true, true)).card.height;
  const int without = dashboard::computeCardLayout(tile(), m, request(1, false, true)).card.height;
  EXPECT_EQ(withAuthor - without, dashboard::kAuthorTopGap + m.authorLineHeight);
}

TEST(DashboardCardHeight, ShrinksAndDropsThePanelWhenThereAreNoStats) {
  const auto m = latinMetrics();
  const auto withStats = dashboard::computeCardLayout(tile(), m, request(1, true, true));
  const auto without = dashboard::computeCardLayout(tile(), m, request(1, true, false));
  EXPECT_GT(withStats.panel.height, 0);
  EXPECT_EQ(without.panel.height, 0) << "no stats must mean no panel, not an empty one";
  EXPECT_EQ(without.panel.width, 0);
  EXPECT_LT(without.card.height, withStats.card.height);
  // Both start at the band's top edge; only the bottom edge moves.
  EXPECT_EQ(without.card.y, withStats.card.y);
}

TEST(DashboardCardHeight, TallLinesGiveUpTitleLinesBeforeTheBandOverflows) {
  // A three-line title at a 38px CJK line cannot coexist with an author line and a stats panel in
  // a 420px band; the layout must give up lines rather than draw past the band.
  const auto l = dashboard::computeCardLayout(tile(), cjkMetrics(38), request(3, true, true));
  EXPECT_LT(l.maxTitleLines, 3);
  EXPECT_GE(l.maxTitleLines, 1);
  EXPECT_TRUE(contains(tile(), l.card));
}

TEST(DashboardCardHeight, ExtremeLinesDropThePanelRatherThanTheLastTitleLine) {
  // Far past any real font. The card must still name its book, so the panel goes first.
  const auto l = dashboard::computeCardLayout(tile(), cjkMetrics(70), request(3, true, true));
  EXPECT_EQ(l.maxTitleLines, 1);
  EXPECT_EQ(l.panel.height, 0);
  EXPECT_TRUE(contains(tile(), l.card));
}

TEST(DashboardCardHeight, DegenerateBandProducesNoCardRatherThanNegativeGeometry) {
  const auto l = dashboard::computeCardLayout(Rect{0, 0, 10, 10}, latinMetrics(), request(1, true, true));
  EXPECT_LE(l.card.width, 0);
  EXPECT_EQ(l.panel.width, 0);
  EXPECT_EQ(l.maxTitleLines, 0);
}

// --- Stat cell fitting -------------------------------------------------------------------------

TEST(DashboardStatCells, ThreeLatinCellsFitTheRealPanelWidth) {
  const auto l = dashboard::computeCardLayout(tile(), latinMetrics(), request(1, true, true));
  // "Time Read" / "Time Left" / "Pages/min" at SMALL, over values at UI_10 bold.
  const int widths[3] = {68, 66, 74};
  int x[3] = {};
  ASSERT_EQ(dashboard::fitStatCells(widths, 3, l.panelContentX, l.statAreaWidth, dashboard::kStatCellGap, x), 3);
  EXPECT_EQ(x[0], l.panelContentX);
  EXPECT_EQ(x[1], x[0] + widths[0] + dashboard::kStatCellGap);
  EXPECT_EQ(x[2], x[1] + widths[1] + dashboard::kStatCellGap);
  EXPECT_LE(x[2] + widths[2], l.panelContentX + l.statAreaWidth);
}

TEST(DashboardStatCells, WideCjkCaptionsStillFitTheRealPanelWidth) {
  const auto l = dashboard::computeCardLayout(tile(), cjkMetrics(34), request(1, true, true));
  // "閱讀時間" / "剩餘時間" / "頁/分" at a 34px full-width advance.
  const int widths[3] = {4 * 34, 4 * 34, 2 * 34 + 17};
  int x[3] = {};
  const int fitted = dashboard::fitStatCells(widths, 3, l.panelContentX, l.statAreaWidth, dashboard::kStatCellGap, x);
  EXPECT_EQ(fitted, 3);
  EXPECT_LE(x[fitted - 1] + widths[fitted - 1], l.panelContentX + l.statAreaWidth);
}

TEST(DashboardStatCells, TrailingCellsAreDroppedRatherThanOverlapped) {
  const int widths[3] = {200, 200, 200};
  int x[3] = {-1, -1, -1};
  EXPECT_EQ(dashboard::fitStatCells(widths, 3, 0, 420, dashboard::kStatCellGap, x), 2);
  EXPECT_EQ(x[0], 0);
  EXPECT_EQ(x[1], 200 + dashboard::kStatCellGap);
  EXPECT_EQ(x[2], -1) << "third cell must not be written when it does not fit";
}

TEST(DashboardStatCells, NoCellFitsWhenEvenTheFirstIsTooWide) {
  const int widths[1] = {500};
  int x[1] = {-1};
  EXPECT_EQ(dashboard::fitStatCells(widths, 1, 0, 420, dashboard::kStatCellGap, x), 0);
  EXPECT_EQ(x[0], -1);
}

TEST(DashboardStatCells, GuardsAgainstEmptyAndNullInput) {
  int x[1] = {};
  const int widths[1] = {10};
  EXPECT_EQ(dashboard::fitStatCells(nullptr, 1, 0, 100, 8, x), 0);
  EXPECT_EQ(dashboard::fitStatCells(widths, 1, 0, 100, 8, nullptr), 0);
  EXPECT_EQ(dashboard::fitStatCells(widths, 0, 0, 100, 8, x), 0);
  EXPECT_EQ(dashboard::fitStatCells(widths, 1, 0, 0, 8, x), 0);
}

// --- Long titles and authors --------------------------------------------------------------------

TEST(DashboardTitleBudget, LongTitlesGetAtLeastAsManyLinesAtLatinMetricsAsAtCjkMetrics) {
  const auto latin = dashboard::computeCardLayout(tile(), latinMetrics(), request(3, true, true));
  const auto cjk = dashboard::computeCardLayout(tile(), cjkMetrics(38), request(3, true, true));
  // A title longer than the budget is truncated by the renderer's own wrappedText(maxLines), never
  // drawn over the panel or past the band.
  EXPECT_GE(latin.maxTitleLines, cjk.maxTitleLines);
  EXPECT_GE(cjk.maxTitleLines, 1);
}

TEST(DashboardTitleBudget, TitleWidthLeavesRoomForTheCardPaddingOnBothSides) {
  const auto l = dashboard::computeCardLayout(tile(), latinMetrics(), request(1, true, true));
  EXPECT_EQ(l.titleX, l.card.x + dashboard::kCardPadding);
  EXPECT_EQ(l.titleX + l.titleMaxWidth, l.card.x + l.card.width - dashboard::kCardPadding);
  // Derivable from the band alone, so the theme can wrap before it has a layout.
  EXPECT_EQ(dashboard::cardTextWidth(tile()), l.titleMaxWidth);
}

TEST(DashboardTitleBudget, AuthorRowFollowsHoweverManyTitleLinesWereActuallyDrawn) {
  const auto m = latinMetrics();
  const auto l = dashboard::computeCardLayout(tile(), m, request(3, true, true));
  EXPECT_EQ(dashboard::authorLineY(l, m, 1), l.titleTopY + m.titleLineHeight + dashboard::kAuthorTopGap);
  EXPECT_EQ(dashboard::authorLineY(l, m, 2), l.titleTopY + 2 * m.titleLineHeight + dashboard::kAuthorTopGap);
  // A one-line title must not leave the author floating where a three-line title would put it.
  EXPECT_LT(dashboard::authorLineY(l, m, 1), dashboard::authorLineY(l, m, 3));
}
