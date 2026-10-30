#pragma once

#include <algorithm>

#include "components/themes/BaseTheme.h"

// Geometry for the Journal (文藝刊物) Home. Pure arithmetic over the screen size and line heights
// the caller measured from the live renderer -- no renderer, no fonts, no I/O -- so it is
// host-testable (test/journal_layout/). As with DashboardLayout.h, every line height must come
// from the text-aware GfxRenderer::getLineHeight(fontId, text, style) overload: on the X3 zh-TW
// profile SMALL/UI_10/UI_12 all render through one taller 14pt CJK fallback, and offsets tuned to
// Latin metrics are what make sections collide on that build only.
//
// Top to bottom: status strip (time | battery), masthead with a stacked tagline at its right,
// a thick+thin double rule, a narrow date/category strip, the "now reading" hero (cover at left,
// ~40% of the content width, text at right), the "recently read" section (two columns divided by
// a vertical rule), the utility row (書庫 / 搜尋 / 設定) and the button hints. The top is anchored
// to the masthead and the bottom to the hints; the hero cover takes what is left, capped at a 2:3
// book shape, so any spare height falls between the hero and the recent section instead of
// opening a band anywhere else.
namespace journal {

constexpr int kStatusTopGap = 6;
constexpr int kStatusMastheadGap = 2;
constexpr int kRuleThick = 2;
constexpr int kRuleGap = 3;  // between the thick and the thin rule
constexpr int kRuleToStrip = 6;
constexpr int kStripToRule = 4;   // strip text bottom to its hairline
constexpr int kSectionGap = 14;   // hairline / section bottom to the next section label
constexpr int kLabelGap = 8;      // section label bottom to its content
constexpr int kTaglineGap = 16;   // masthead right edge to tagline left edge, minimum
constexpr int kHeroTextGap = 18;  // hero cover right edge to its text column
constexpr int kColumnGap = 24;    // between the two recent columns; the divider sits in its middle
constexpr int kRecentCoverWidth = 68;
constexpr int kRecentTextGap = 12;
constexpr int kNavPaddingY = 9;
constexpr int kNavToRule = 6;  // utility row bottom to the rule above the hints
constexpr int kRuleToHints = 6;
constexpr int kRecentToNav = 16;
constexpr int kMaxHeroTitleLines = 3;
constexpr int kMaxHeroCoverPercent = 45;  // of the content width; the nominal size is 40%
constexpr int kRecentTitleLines = 2;
// Room the crop-corner focus brackets need outside an item; layouts keep this clear of the edges.
constexpr int kBracketOutset = 6;
constexpr int kBracketLength = 14;
constexpr int kBracketStroke = 3;

struct Metrics {
  int screenWidth = 0;
  int screenHeight = 0;
  int sidePadding = 0;
  int hintsHeight = 0;  // 0 when hints are hidden
  int statusLine = 0;   // SMALL: clock and battery text
  int mastheadLine = 0;
  int mastheadWidth = 0;
  int taglineLine = 0;   // SMALL, per line; the tagline is two stacked lines
  int taglineWidth = 0;  // wider of the two lines
  int stripLine = 0;     // SMALL: date and category
  int labelLine = 0;     // UI_10 bold: section labels
  int titleLine = 0;     // hero title font (the resident reader font when usable, else UI_12 bold)
  int bodyLine = 0;      // UI_10: author, progress, utility labels
  int metaLine = 0;      // SMALL: recent column author/progress
};

// Which optional rows the caller will draw. Every drawn row is reserved, so nothing is pushed past
// its section; an absent row reserves nothing.
struct Request {
  int recentCount = 0;  // recent books besides the hero (0..2)
  bool heroHasAuthor = false;
  bool heroHasProgress = false;  // whole-book progress bar + percentage
  bool recentHasAuthor = false;  // any recent column draws an author line
  bool recentHasMeta = false;    // any recent column draws a progress/downloaded line
};

struct Layout {
  bool valid = false;
  Rect status;
  Rect masthead;
  Rect tagline;  // zero width when it would crowd the masthead; the caller then omits it
  int ruleThickY = 0;
  int ruleThinY = 0;
  Rect strip;
  int stripRuleY = 0;
  Rect heroLabel;
  Rect hero;  // the whole focusable hero item: cover plus text column
  Rect heroCover;
  Rect heroText;
  int heroTitleLines = 1;  // title lines that fit above the reserved rows; the caller wraps to this
  // Hero text rows, top to bottom; a row the request omits has y < 0. The author row follows the
  // title, so its y depends on the wrapped line count: authorY(n) = heroText.y + n * titleLine.
  int heroSeparatorGap = 0;  // author (or title) bottom to the separator hairline
  int heroProgressY = -1;
  // Recent column rows, relative to the column top; the meta row is bottom-anchored.
  int recentTitleLines = kRecentTitleLines;
  int recentAuthorDy = -1;
  int recentMetaDy = -1;
  Rect recentLabel;
  Rect recentColumns[2];
  int recentDividerX = 0;
  int recentCoverWidth = 0;
  int recentCoverHeight = 0;
  bool recentHasColumns = false;  // false: one muted "none" line instead of columns
  int navRuleY = 0;               // hairline above the utility row
  Rect nav;
  Rect navCells[3];
  int hintsRuleY = 0;
  int hintsTop = 0;
};

inline Layout computeLayout(const Metrics& m, const Request& req) {
  const int recentCount = req.recentCount;
  Layout l;
  const int contentW = m.screenWidth - 2 * m.sidePadding;
  if (contentW <= 0 || m.screenHeight <= 0) return l;
  const int left = m.sidePadding;
  const int right = left + contentW;

  int y = kStatusTopGap;
  l.status = Rect{left, y, contentW, m.statusLine};
  y += m.statusLine + kStatusMastheadGap;

  l.masthead = Rect{left, y, std::min(m.mastheadWidth, contentW), m.mastheadLine};
  const int taglineH = 2 * m.taglineLine;
  const int taglineX = right - m.taglineWidth;
  if (m.taglineWidth > 0 && taglineX >= l.masthead.x + l.masthead.width + kTaglineGap && taglineH <= m.mastheadLine) {
    l.tagline = Rect{taglineX, y + (m.mastheadLine - taglineH) / 2, m.taglineWidth, taglineH};
  }
  y += m.mastheadLine;

  l.ruleThickY = y;
  l.ruleThinY = l.ruleThickY + kRuleThick + kRuleGap;
  y = l.ruleThinY + 1 + kRuleToStrip;

  l.strip = Rect{left, y, contentW, m.stripLine};
  y += m.stripLine + kStripToRule;
  l.stripRuleY = y;
  y += 1 + kSectionGap;

  l.heroLabel = Rect{left, y, contentW, m.labelLine};
  y += m.labelLine + kLabelGap;
  const int heroTop = y;

  // Bottom-anchored sections.
  l.hintsTop = m.screenHeight - m.hintsHeight;
  l.hintsRuleY = l.hintsTop - kRuleToHints;
  const int navH = m.bodyLine + 2 * kNavPaddingY;
  l.nav = Rect{left, l.hintsRuleY - kNavToRule - navH, contentW, navH};
  l.navRuleY = l.nav.y - 1;
  // The library entry ("BookOrbit 書庫") gets half the row; 搜尋 and 設定 share the other half.
  const int firstW = contentW / 2;
  const int otherW = (contentW - firstW) / 2;
  l.navCells[0] = Rect{left, l.nav.y, firstW, navH};
  l.navCells[1] = Rect{left + firstW, l.nav.y, otherW, navH};
  l.navCells[2] = Rect{left + firstW + otherW, l.nav.y, right - (left + firstW + otherW), navH};

  l.recentHasColumns = recentCount > 0;
  l.recentCoverWidth = kRecentCoverWidth;
  l.recentCoverHeight = kRecentCoverWidth * 3 / 2;
  // Recent column text: up to two title lines (UI_10 bold, bodyLine), author, then the meta row.
  const int recentTextH =
      kRecentTitleLines * m.bodyLine + (req.recentHasAuthor ? m.metaLine : 0) + (req.recentHasMeta ? m.metaLine : 0);
  const int columnsH = l.recentHasColumns ? std::max(l.recentCoverHeight, recentTextH) : m.bodyLine;
  if (l.recentHasColumns) {
    l.recentAuthorDy = req.recentHasAuthor ? kRecentTitleLines * m.bodyLine : -1;
    l.recentMetaDy = req.recentHasMeta ? columnsH - m.metaLine : -1;
  }
  const int columnsTop = l.navRuleY - kRecentToNav - columnsH;
  l.recentLabel = Rect{left, columnsTop - kLabelGap - m.labelLine, contentW, m.labelLine};
  const int columnW = (contentW - kColumnGap) / 2;
  for (int i = 0; i < 2; i++) {
    l.recentColumns[i] = Rect{left + i * (columnW + kColumnGap), columnsTop, columnW, columnsH};
  }
  l.recentDividerX = left + columnW + kColumnGap / 2;

  // The hero takes what is left, as a 2:3 cover at 40% of the content width, growing to at most
  // kMaxHeroCoverPercent when there is room. Any height still left is split evenly between the
  // gap above the recent section and the gap above the utility row, so no single empty band forms.
  const int heroAvail = l.recentLabel.y - kSectionGap - heroTop;
  int coverW = std::min(contentW * kMaxHeroCoverPercent / 100, heroAvail * 2 / 3);
  const int coverH = std::min(coverW * 3 / 2, heroAvail);
  coverW = std::min(coverW, coverH * 2 / 3);
  if (coverW <= 0 || coverH <= 0) return l;
  const int spare = heroAvail - coverH;
  if (spare > 1) {
    const int shift = spare / 2;  // the recent section moves up by half the spare height
    l.recentLabel.y -= shift;
    for (auto& col : l.recentColumns) col.y -= shift;
  }
  l.hero = Rect{left, heroTop, contentW, coverH};
  l.heroCover = Rect{left, heroTop, coverW, coverH};
  const int textX = left + coverW + kHeroTextGap;
  l.heroText = Rect{textX, heroTop, right - textX, coverH};

  // Hero text column, bottom-up: progress row, separator, author; the title takes the rest. (The
  // 已下載 marker sits at the right end of the hero label row, so it costs no headline line.)
  int bottom = l.heroText.y + l.heroText.height;
  if (req.heroHasProgress) {
    l.heroProgressY = bottom - m.bodyLine;
    bottom = l.heroProgressY - kLabelGap;
  }
  l.heroSeparatorGap = kLabelGap;
  const int reservedBelowTitle = (req.heroHasAuthor ? m.bodyLine : 0) + kLabelGap + 1;
  const int titleRoom = bottom - l.heroText.y - reservedBelowTitle;
  l.heroTitleLines = std::clamp(titleRoom / std::max(1, m.titleLine), 0, kMaxHeroTitleLines);
  if (l.heroTitleLines < 1) return l;  // cannot name the book: report the layout as not fitting
  l.valid = true;
  return l;
}

}  // namespace journal
