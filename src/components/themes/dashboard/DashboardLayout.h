#pragma once

#include "components/themes/BaseTheme.h"

// Geometry for the Dashboard Home card. Pure arithmetic over line heights and text widths the
// caller measured from the live renderer -- no renderer, no fonts, no I/O -- so it is
// host-testable (see test/dashboard_layout/).
//
// Why this is split out rather than inlined into DashboardTheme.cpp: every measurement that feeds
// it changes size on the X3 zh-TW profile, where SMALL/UI_10/UI_12 all fall back to ONE shared
// 14pt CJK SD font (see SdCardFontSystem's kUiFontSizes). A CJK line is materially taller than
// the 8/10/12pt builtin Latin line the same role renders, and CJK stat captions are materially
// wider. Hard-coded offsets tuned against Latin metrics are exactly what produces a card whose
// title runs into its stats panel, or a stat row that overlaps itself, on that build only -- and
// neither is visible in a Latin simulator screenshot. Deriving the layout from the measured
// values, and testing it at both metric scales, is what makes that class of bug catchable here.
//
// There is deliberately no progress bar and no percentage here. A truthful book-level percentage
// needs book.bin's cumulative spine sizes, and every route to those opens the book -- Epub::load(),
// or BookMetadataCache::load() against the same file. Home does not open books. Rather than show a
// chapter-local number dressed up as book progress, or invent a new persisted field to cache one,
// the card shows only what the recent-books entry and stats.bin already hold. See
// HomeActivity::loadDashboardBookStats.
namespace dashboard {

// --- Fixed design constants (device-independent; the card scales via the measured values) ------
constexpr int kCardInset = 20;  // card edge to screen edge
constexpr int kCardTopGap = 8;  // tile top to card top
constexpr int kCardRadius = 26;
constexpr int kCardPadding = 22;  // card edge to pill / title / icon tile
constexpr int kPillRadius = 18;
constexpr int kPillPaddingX = 15;
constexpr int kPillPaddingY = 9;
constexpr int kIconTileSize = 54;  // round tile holding the book/pin glyph
// BookIcon/PinIcon are 32x32; see kEmptyIconSize on why this must match the asset exactly.
constexpr int kIconSize = 32;
constexpr int kIconTileLift = 4;   // icon tile sits slightly above the pill's top edge
constexpr int kTitleTopGap = 26;   // pill bottom to first title line
constexpr int kAuthorTopGap = 10;  // title block bottom to author line
constexpr int kMaxTitleLines = 3;
constexpr int kIdentityPanelGap = 22;  // author (or title) bottom to the stats panel's top edge
constexpr int kPanelInset = 16;        // card edge to stats panel edge, and panel to card bottom
constexpr int kPanelRadius = 20;
constexpr int kPanelPaddingX = 18;
constexpr int kPanelPaddingY = 14;
constexpr int kPanelHeadingGap = 14;  // heading row bottom to the stat caption row
constexpr int kStatValueGap = 2;      // stat caption bottom to stat value
constexpr int kStatCellGap = 16;
constexpr int kBrowseDotSize = 7;
constexpr int kBrowseDotGap = 5;

// --- Empty-state constants ---------------------------------------------------------------------
constexpr int kEmptyIconTileSize = 64;
// Must be BookIcon's real pixel size: GfxRenderer::drawIcon derives the bitmap's row stride from
// this argument, so any other value walks the icon's bytes at the wrong pitch and draws noise.
constexpr int kEmptyIconSize = 32;
constexpr int kEmptyHeadingTopGap = 24;
constexpr int kEmptyActionTopGap = 26;
constexpr int kEmptyActionPaddingX = 22;
constexpr int kEmptyActionPaddingY = 12;

// Everything the layout needs measured from the live renderer. All line heights must come from
// GfxRenderer's text-aware getLineHeight(fontId, text, style) overload, not the fontId-only one:
// the latter reports the builtin Latin font's advance even when the string will actually render
// through the CJK fallback, which is precisely the mismatch this header exists to survive.
struct CardMetrics {
  int pillTextWidth = 0;
  int pillLineHeight = 0;          // UI_10, the "Continue Reading" pill label
  int titleLineHeight = 0;         // UI_12 bold, the book title
  int authorLineHeight = 0;        // UI_12 regular, the author line
  int panelHeadingLineHeight = 0;  // UI_10 bold, the stats panel's heading
  int statLabelLineHeight = 0;     // SMALL, stat captions
  int statValueLineHeight = 0;     // UI_10 bold, stat values
};

// What the caller actually has to draw. The card's height follows this rather than filling the
// tile, so a book with no author and no recorded reading time gets a card that ends where its
// content ends instead of a mostly-empty box -- which is the shape the card takes by default,
// since reading-stats tracking is opt-in.
struct CardRequest {
  int titleLines = 1;  // wrapped title lines the caller measured, before any clamp below
  bool hasAuthor = false;
  bool hasStats = false;  // a stats panel is worth drawing (stats.bin had a qualifying session)
};

struct CardLayout {
  Rect card{};
  Rect pill{};
  Rect iconTile{};
  // Zero width/height when the caller has no stats, or when the measured line heights leave no
  // room for the panel. The theme draws no panel at all in that case.
  Rect panel{};
  int titleX = 0;
  int titleTopY = 0;
  int titleMaxWidth = 0;
  // May be fewer than CardRequest::titleLines when the card would otherwise overflow the tile.
  // The theme re-wraps to this, so a long title is truncated, never overlapped or clipped.
  int maxTitleLines = 0;
  int panelContentX = 0;
  int panelContentRight = 0;
  int panelHeadingY = 0;
  int statLabelY = 0;
  int statValueY = 0;
  int statAreaWidth = 0;
};

// Width available to the title and author, derivable from the tile alone -- so the caller can wrap
// its title before it knows anything else about the layout.
constexpr int cardTextWidth(const Rect& tile) { return tile.width - kCardInset * 2 - kCardPadding * 2; }

// The empty state's card fills the whole band. It has no content to size itself to, and a small
// card floating in a large void reads worse than one large calm one.
inline Rect emptyCardRect(const Rect& tile) {
  return Rect{tile.x + kCardInset, tile.y + kCardTopGap, tile.width - kCardInset * 2, tile.height - kCardTopGap * 2};
}

// Height the stats panel needs: a heading row over one row of caption/value stat cells.
inline int panelHeight(const CardMetrics& m) {
  return kPanelPaddingY + m.panelHeadingLineHeight + kPanelHeadingGap + m.statLabelLineHeight + kStatValueGap +
         m.statValueLineHeight + kPanelPaddingY;
}

// `tile` is the Home cover-tile rect HomeActivity hands the theme. The card is anchored to the
// tile's top edge and sized to its content, never taller than the tile.
inline CardLayout computeCardLayout(const Rect& tile, const CardMetrics& m, const CardRequest& req) {
  CardLayout l;
  const int cardWidth = tile.width - kCardInset * 2;
  const int maxCardHeight = tile.height - kCardTopGap * 2;
  if (cardWidth <= 0 || maxCardHeight <= 0) return l;

  const int pillH = m.pillLineHeight + kPillPaddingY * 2;
  const int headerH = kCardPadding + pillH + kTitleTopGap;
  const int authorH = req.hasAuthor ? kAuthorTopGap + m.authorLineHeight : 0;
  const int wantedPanelH = req.hasStats ? panelHeight(m) : 0;

  // Shrink the title block until the whole card fits the tile, then drop the panel if it still
  // does not. One title line is the floor: a card that cannot name its book is not worth drawing,
  // so the panel goes first.
  int lines = req.titleLines < 1 ? 1 : (req.titleLines > kMaxTitleLines ? kMaxTitleLines : req.titleLines);
  int panelH = wantedPanelH;
  const auto heightFor = [&](const int titleLines, const int panel) {
    const int footer = panel > 0 ? kIdentityPanelGap + panel + kPanelInset : kCardPadding;
    return headerH + titleLines * m.titleLineHeight + authorH + footer;
  };
  while (heightFor(lines, panelH) > maxCardHeight && lines > 1) --lines;
  if (heightFor(lines, panelH) > maxCardHeight) panelH = 0;
  const int cardHeight = heightFor(lines, panelH) > maxCardHeight ? maxCardHeight : heightFor(lines, panelH);

  l.card = Rect{tile.x + kCardInset, tile.y + kCardTopGap, cardWidth, cardHeight};
  l.pill = Rect{l.card.x + kCardPadding, l.card.y + kCardPadding, m.pillTextWidth + kPillPaddingX * 2, pillH};
  l.iconTile = Rect{l.card.x + l.card.width - kCardPadding - kIconTileSize, l.card.y + kCardPadding - kIconTileLift,
                    kIconTileSize, kIconTileSize};
  l.titleX = l.card.x + kCardPadding;
  l.titleTopY = l.pill.y + l.pill.height + kTitleTopGap;
  l.titleMaxWidth = cardTextWidth(tile);
  l.maxTitleLines = lines;

  if (panelH > 0) {
    l.panel = Rect{l.card.x + kPanelInset, l.card.y + l.card.height - kPanelInset - panelH,
                   l.card.width - kPanelInset * 2, panelH};
    l.panelContentX = l.panel.x + kPanelPaddingX;
    l.panelContentRight = l.panel.x + l.panel.width - kPanelPaddingX;
    l.statAreaWidth = l.panel.width - kPanelPaddingX * 2;
    l.panelHeadingY = l.panel.y + kPanelPaddingY;
    l.statLabelY = l.panelHeadingY + m.panelHeadingLineHeight + kPanelHeadingGap;
    l.statValueY = l.statLabelY + m.statLabelLineHeight + kStatValueGap;
  }
  return l;
}

// Top y of the author line, given how many title lines were actually drawn.
inline int authorLineY(const CardLayout& l, const CardMetrics& m, const int drawnTitleLines) {
  return l.titleTopY + drawnTitleLines * m.titleLineHeight + kAuthorTopGap;
}

// Lays out up to `count` stat cells left-to-right across `availableWidth`, each cell as wide as
// its own widest line. Returns how many cells fit and writes their left x into outX[0..returned).
// Trailing cells that would not fit are dropped rather than clipped or overlapped -- the failure
// mode on the zh-TW profile, where a caption like "閱讀時間" is several times the width of
// "Time Read".
inline int fitStatCells(const int* cellWidths, const int count, const int x, const int availableWidth, const int gap,
                        int* outX) {
  if (cellWidths == nullptr || outX == nullptr || count <= 0 || availableWidth <= 0) return 0;
  int used = 0;
  int fitted = 0;
  for (int i = 0; i < count; ++i) {
    const int width = cellWidths[i] > 0 ? cellWidths[i] : 0;
    const int needed = used + (fitted > 0 ? gap : 0) + width;
    if (needed > availableWidth) break;
    outX[fitted] = x + used + (fitted > 0 ? gap : 0);
    used = needed;
    ++fitted;
  }
  return fitted;
}

}  // namespace dashboard
