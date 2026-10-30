#include "DashboardTheme.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "CrossPointSettings.h"
#include "RecentBooksStore.h"
#include "activities/reader/ReadingDurationFormat.h"
#include "components/UITheme.h"
#include "components/icons/book.h"
#include "components/icons/pin.h"
#include "components/themes/dashboard/DashboardLayout.h"
#include "fontIds.h"

// See DashboardTheme.h for what this adapts from YACP and what it deliberately leaves behind.

namespace {

// Only the three built-in UI roles are used anywhere on this card. That is not incidental: the X3
// zh-TW profile installs a CJK SD fallback for exactly SMALL_FONT_ID, UI_10_FONT_ID and
// UI_12_FONT_ID (SdCardFontSystem::kUiFontSizes), so any other font id would render CJK as
// missing glyphs on that build. There is no 16pt display role here for the same reason -- YACP's
// card uses one; adding it would mean a new font asset, which this phase does not do.
constexpr int kPillFontId = UI_10_FONT_ID;
constexpr int kTitleFontId = UI_12_FONT_ID;
constexpr int kAuthorFontId = UI_12_FONT_ID;
constexpr int kPanelHeadingFontId = UI_10_FONT_ID;
constexpr int kStatLabelFontId = SMALL_FONT_ID;
constexpr int kStatValueFontId = UI_10_FONT_ID;
constexpr int kMenuFontId = UI_12_FONT_ID;

constexpr int kMenuSideInset = 20;
constexpr int kMenuRowGap = 12;
constexpr int kMenuRowRadius = 24;
constexpr int kMenuRowTextInset = 22;
constexpr int kMenuRowPaddingY = 20;   // 10px above + 10px below the label line
constexpr int kMenuRowMinHeight = 44;  // floor, so short Latin lines still give a tappable row
constexpr int kMenuTopGap = 10;

constexpr int kHeaderSideInset = 20;

int textWidth(const GfxRenderer& renderer, const int fontId, const char* text, const EpdFontFamily::Style style) {
  return text == nullptr ? 0 : renderer.getTextWidth(fontId, text, style);
}

// Measures every line the card will draw, through the text-aware getLineHeight overload so a CJK
// fallback line is measured at the size it will actually render (see DashboardLayout.h).
dashboard::CardMetrics measureCard(const GfxRenderer& renderer, const RecentBook* book) {
  dashboard::CardMetrics m;
  const char* pillLabel = tr(STR_CONTINUE_READING);
  const char* title = (book != nullptr && !book->title.empty()) ? book->title.c_str() : "";
  const char* author = (book != nullptr && !book->author.empty()) ? book->author.c_str() : "";

  m.pillTextWidth = textWidth(renderer, kPillFontId, pillLabel, EpdFontFamily::BOLD);
  m.pillLineHeight = renderer.getLineHeight(kPillFontId, pillLabel, EpdFontFamily::BOLD);
  m.titleLineHeight = renderer.getLineHeight(kTitleFontId, title, EpdFontFamily::BOLD);
  m.authorLineHeight = renderer.getLineHeight(kAuthorFontId, author, EpdFontFamily::REGULAR);
  m.panelHeadingLineHeight = renderer.getLineHeight(kPanelHeadingFontId, tr(STR_READING_STATS), EpdFontFamily::BOLD);
  m.statLabelLineHeight = renderer.getLineHeight(kStatLabelFontId, tr(STR_STATS_TIME_READ), EpdFontFamily::REGULAR);
  m.statValueLineHeight = renderer.getLineHeight(kStatValueFontId, "00h 00m", EpdFontFamily::BOLD);
  return m;
}

// One stat cell: a caption over a value. `value` points into a StatBuffers the caller owns, so
// both must outlive the cell.
struct StatCell {
  const char* label;
  const char* value;
};

// Storage the stat values are formatted into. Held by the caller (drawHomeCard) rather than inside
// the builder, because the cells point at it.
struct StatBuffers {
  char timeRead[16] = "";
  char timeLeft[16] = "";
  char pace[16] = "";
};

// Everything this fork actually persists for a book (BookReadingStats), in display order, with any
// field that has nothing to say left out rather than zero-filled. Returns how many cells were
// written; zero means the panel has nothing in it and is not drawn -- and, because drawHomeCard
// calls this BEFORE laying the card out, the card is not sized for one either.
int buildStatCells(const DashboardCardData& data, StatBuffers& buffers, StatCell* cells) {
  if (!data.hasStats) return 0;
  int count = 0;
  if (data.stats.totalReadingSeconds > 0) {
    ReadingDurationFormat::compact(data.stats.totalReadingSeconds, buffers.timeRead, sizeof(buffers.timeRead));
    cells[count++] = {tr(STR_STATS_TIME_READ), buffers.timeRead};
  }
  if (data.stats.estimatedTimeLeftSeconds > 0) {
    ReadingDurationFormat::compact(data.stats.estimatedTimeLeftSeconds, buffers.timeLeft, sizeof(buffers.timeLeft));
    cells[count++] = {tr(STR_STATS_TIME_LEFT), buffers.timeLeft};
  }
  // Pace straight from the two persisted counters -- no new field, no stored derivative. Guarded
  // on a minute of reading so the very first page turn cannot print an absurd rate.
  if (data.stats.totalReadingSeconds > 60 && data.stats.totalPagesTurned > 0) {
    const float pagesPerMinute =
        static_cast<float>(data.stats.totalPagesTurned) * 60.0f / static_cast<float>(data.stats.totalReadingSeconds);
    snprintf(buffers.pace, sizeof(buffers.pace), "%.1f", pagesPerMinute);
    cells[count++] = {tr(STR_STATS_PAGES_PER_MIN), buffers.pace};
  }
  return count;
}

void drawBrowseDots(const GfxRenderer& renderer, const dashboard::CardLayout& l, const int index, const int count) {
  if (count <= 1) return;
  const int totalW = count * dashboard::kBrowseDotSize + (count - 1) * dashboard::kBrowseDotGap;
  // Right-aligned into the gap between the pill and the icon tile, vertically centred on the pill.
  const int rightLimit = l.iconTile.x - 12;
  int dotX = rightLimit - totalW;
  if (dotX < l.pill.x + l.pill.width + 12) return;  // no room: the indicator is optional
  const int dotY = l.pill.y + (l.pill.height - dashboard::kBrowseDotSize) / 2;
  for (int i = 0; i < count; ++i) {
    if (i == index) {
      renderer.fillRect(dotX, dotY, dashboard::kBrowseDotSize, dashboard::kBrowseDotSize, true);
    } else {
      renderer.drawRect(dotX, dotY, dashboard::kBrowseDotSize, dashboard::kBrowseDotSize, true);
    }
    dotX += dashboard::kBrowseDotSize + dashboard::kBrowseDotGap;
  }
}

void drawCardIdentity(const GfxRenderer& renderer, const dashboard::CardLayout& l, const dashboard::CardMetrics& m,
                      const DashboardCardData& data, const std::vector<std::string>& titleLines) {
  // "Continue Reading" pill.
  const char* pillLabel = tr(STR_CONTINUE_READING);
  renderer.fillRoundedRect(l.pill.x, l.pill.y, l.pill.width, l.pill.height, dashboard::kPillRadius, Color::Black);
  renderer.drawText(kPillFontId, l.pill.x + dashboard::kPillPaddingX, l.pill.y + dashboard::kPillPaddingY, pillLabel,
                    false, EpdFontFamily::BOLD);

  // Round icon tile. Pin glyph when the shown book is the pinned default, book glyph otherwise --
  // the one visual cue that says "this is where Home always starts".
  renderer.fillRoundedRect(l.iconTile.x, l.iconTile.y, l.iconTile.width, l.iconTile.height, l.iconTile.width / 2,
                           Color::White);
  const uint8_t* glyph = data.pinned ? PinIcon : BookIcon;
  renderer.drawIcon(glyph, l.iconTile.x + (l.iconTile.width - dashboard::kIconSize) / 2,
                    l.iconTile.y + (l.iconTile.height - dashboard::kIconSize) / 2, dashboard::kIconSize);

  drawBrowseDots(renderer, l, data.browseIndex, data.browseCount);

  int lineY = l.titleTopY;
  for (const auto& line : titleLines) {
    renderer.drawText(kTitleFontId, l.titleX, lineY, line.c_str(), true, EpdFontFamily::BOLD);
    lineY += m.titleLineHeight;
  }
  const int drawnLines = static_cast<int>(titleLines.size());

  if (!data.book->author.empty()) {
    const std::string author =
        renderer.truncatedText(kAuthorFontId, data.book->author.c_str(), l.titleMaxWidth, EpdFontFamily::REGULAR);
    renderer.drawText(kAuthorFontId, l.titleX, dashboard::authorLineY(l, m, drawnLines), author.c_str(), true,
                      EpdFontFamily::REGULAR);
  }
}

// The stats panel. Everything in it comes from stats.bin alone -- a ~19-byte read off a cache path
// the Epub constructor derives from the book path without any I/O -- so nothing here costs Home a
// book open. There is no progress bar or percentage: see DashboardLayout.h's header comment.
void drawCardStats(const GfxRenderer& renderer, const dashboard::CardLayout& l, const StatCell* cells,
                   const int cellCount) {
  if (l.panel.width <= 0 || l.panel.height <= 0 || cellCount <= 0) return;

  renderer.fillRoundedRect(l.panel.x, l.panel.y, l.panel.width, l.panel.height, dashboard::kPanelRadius, Color::White);
  renderer.drawText(kPanelHeadingFontId, l.panelContentX, l.panelHeadingY, tr(STR_READING_STATS), true,
                    EpdFontFamily::BOLD);

  int cellWidths[3] = {0, 0, 0};
  for (int i = 0; i < cellCount; ++i) {
    const int labelW = textWidth(renderer, kStatLabelFontId, cells[i].label, EpdFontFamily::REGULAR);
    const int valueW = textWidth(renderer, kStatValueFontId, cells[i].value, EpdFontFamily::BOLD);
    cellWidths[i] = std::max(labelW, valueW);
  }
  int cellX[3] = {0, 0, 0};
  const int fitted =
      dashboard::fitStatCells(cellWidths, cellCount, l.panelContentX, l.statAreaWidth, dashboard::kStatCellGap, cellX);
  for (int i = 0; i < fitted; ++i) {
    renderer.drawText(kStatLabelFontId, cellX[i], l.statLabelY, cells[i].label, true, EpdFontFamily::REGULAR);
    renderer.drawText(kStatValueFontId, cellX[i], l.statValueY, cells[i].value, true, EpdFontFamily::BOLD);
  }
}

// Card background plus its selection outline. A heavier outline when the card holds the Home
// selection: the same "this row is focused" language the menu rows below use, without inverting a
// card this large.
void drawCardFrame(const GfxRenderer& renderer, const Rect& card, const bool selected) {
  renderer.fillRoundedRect(card.x, card.y, card.width, card.height, dashboard::kCardRadius, Color::LightGray);
  renderer.drawRoundedRect(card.x, card.y, card.width, card.height, selected ? 4 : 1, dashboard::kCardRadius, true);
}

void drawEmptyCard(const GfxRenderer& renderer, const Rect& card) {
  const int textW = card.width - dashboard::kCardPadding * 2;
  const char* heading = tr(STR_NO_OPEN_BOOK);
  const char* hint = tr(STR_START_READING);
  const char* action = tr(STR_BROWSE_FILES);

  const int headingLineH = renderer.getLineHeight(kTitleFontId, heading, EpdFontFamily::BOLD);
  const auto headingLines = renderer.wrappedText(kTitleFontId, heading, textW, 2, EpdFontFamily::BOLD);
  const int hintLineH = renderer.getLineHeight(kPillFontId, hint, EpdFontFamily::REGULAR);
  const int actionLineH = renderer.getLineHeight(kTitleFontId, action, EpdFontFamily::BOLD);
  const int actionW =
      renderer.getTextWidth(kTitleFontId, action, EpdFontFamily::BOLD) + dashboard::kEmptyActionPaddingX * 2;
  const int actionH = actionLineH + dashboard::kEmptyActionPaddingY * 2;

  // Measured first, then centred as one block: the empty card has no progress panel anchoring its
  // bottom edge, so anything anchored to a fraction of the card height leaves the content stranded
  // near the top -- and the block's own height moves with the CJK fallback's taller lines.
  const int blockH = dashboard::kEmptyIconTileSize + dashboard::kEmptyHeadingTopGap +
                     static_cast<int>(headingLines.size()) * headingLineH + 6 + hintLineH +
                     dashboard::kEmptyActionTopGap + actionH;
  int cursorY = card.y + std::max(dashboard::kCardPadding, (card.height - blockH) / 2);

  const int tileX = card.x + (card.width - dashboard::kEmptyIconTileSize) / 2;
  renderer.fillRoundedRect(tileX, cursorY, dashboard::kEmptyIconTileSize, dashboard::kEmptyIconTileSize,
                           dashboard::kEmptyIconTileSize / 2, Color::White);
  renderer.drawIcon(BookIcon, tileX + (dashboard::kEmptyIconTileSize - dashboard::kEmptyIconSize) / 2,
                    cursorY + (dashboard::kEmptyIconTileSize - dashboard::kEmptyIconSize) / 2,
                    dashboard::kEmptyIconSize);
  cursorY += dashboard::kEmptyIconTileSize + dashboard::kEmptyHeadingTopGap;

  for (const auto& line : headingLines) {
    const int lineW = renderer.getTextWidth(kTitleFontId, line.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(kTitleFontId, card.x + (card.width - lineW) / 2, cursorY, line.c_str(), true,
                      EpdFontFamily::BOLD);
    cursorY += headingLineH;
  }

  cursorY += 6;
  const int hintW = renderer.getTextWidth(kPillFontId, hint, EpdFontFamily::REGULAR);
  renderer.drawText(kPillFontId, card.x + (card.width - hintW) / 2, cursorY, hint, true, EpdFontFamily::REGULAR);
  cursorY += hintLineH + dashboard::kEmptyActionTopGap;

  // Filled action pill naming the row the Home selection actually starts on when there is no book,
  // so the empty state points at something real rather than decorating.
  const int actionX = card.x + (card.width - actionW) / 2;
  const int actionY = std::min(cursorY, card.y + card.height - dashboard::kCardPadding - actionH);
  if (actionY >= cursorY - dashboard::kEmptyActionTopGap) {
    renderer.fillRoundedRect(actionX, actionY, actionW, actionH, actionH / 2, Color::Black);
    renderer.drawText(kTitleFontId, actionX + dashboard::kEmptyActionPaddingX,
                      actionY + dashboard::kEmptyActionPaddingY, action, false, EpdFontFamily::BOLD);
  }
}

}  // namespace

void DashboardTheme::drawHeader(const GfxRenderer& renderer, const Rect rect, const char* title,
                                const char* subtitle) const {
  if (title != nullptr) {
    // Every screen except Home passes a real title; keep RoundedRaff's header there.
    RoundedRaffTheme::drawHeader(renderer, rect, title, subtitle);
    return;
  }

  // Home's status band: battery on the right, clock on the left when the user enabled one. No
  // product name and no logo -- the deliberate divergence from YACP's branded header.
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  const int batteryX = rect.x + rect.width - kHeaderSideInset - DashboardMetrics::values.batteryWidth;
  const int batteryY = rect.y + (rect.height - DashboardMetrics::values.batteryHeight) / 2;
  drawBatteryRight(
      renderer, Rect{batteryX, batteryY, DashboardMetrics::values.batteryWidth, DashboardMetrics::values.batteryHeight},
      showBatteryPercentage);

  const auto statusBar = SETTINGS.statusBarSpec();
  if (statusBar.showsClock() && halClock.isAvailable()) {
    char timeBuf[9];
    if (halClock.formatTime(timeBuf, sizeof(timeBuf), statusBar.clockUtcOffsetQ, statusBar.clock12h)) {
      const int clockY = rect.y + (rect.height - renderer.getLineHeight(SMALL_FONT_ID, timeBuf)) / 2;
      renderer.drawText(SMALL_FONT_ID, rect.x + kHeaderSideInset, clockY, timeBuf, true, EpdFontFamily::REGULAR);
    }
  }
}

void DashboardTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                         int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                         bool& bufferRestored, std::function<bool()> storeCoverBuffer,
                                         float progressPercent) const {
  (void)renderer;
  (void)rect;
  (void)recentBooks;
  (void)selectorIndex;
  (void)bufferRestored;
  (void)storeCoverBuffer;
  (void)progressPercent;
  // HomeActivity draws this theme's card through drawHomeCard() instead (it needs reading stats
  // and the browse position, which this virtual cannot carry). Both flags are cleared so the
  // caller never allocates or restores a cover snapshot buffer: the card is typographic and has no
  // bitmap worth retaining between frames.
  coverRendered = false;
  coverBufferStored = false;
}

int DashboardTheme::drawHomeCard(GfxRenderer& renderer, const Rect rect, const DashboardCardData& data) const {
  if (data.book == nullptr) {
    const Rect card = dashboard::emptyCardRect(rect);
    if (card.width <= 0 || card.height <= 0) return rect.y;
    drawCardFrame(renderer, card, data.selected);
    drawEmptyCard(renderer, card);
    return card.y + card.height;
  }

  renderer.prewarmTextRuns({{kTitleFontId, data.book->title.c_str(), EpdFontFamily::BOLD},
                            {kAuthorFontId, data.book->author.c_str()},
                            {kPillFontId, tr(STR_CONTINUE_READING), EpdFontFamily::BOLD}});
  const dashboard::CardMetrics metrics = measureCard(renderer, data.book);

  // Stats first: whether there is anything to put in the panel is what decides whether the card is
  // sized for a panel at all. Nothing here reads more than stats.bin.
  StatBuffers statBuffers;
  StatCell statCells[3];
  const int statCellCount = buildStatCells(data, statBuffers, statCells);

  // Then the title, wrapped at the width the card is going to have, so the layout knows how tall
  // the identity block really is before it picks the card's height. Falls back to the file path
  // only when the cached metadata has no title at all, which is what every other Home theme does.
  const char* title = !data.book->title.empty() ? data.book->title.c_str() : data.book->path.c_str();
  auto titleLines = renderer.wrappedText(kTitleFontId, title, dashboard::cardTextWidth(rect), dashboard::kMaxTitleLines,
                                         EpdFontFamily::BOLD);

  dashboard::CardRequest request;
  request.titleLines = static_cast<int>(titleLines.size());
  request.hasAuthor = !data.book->author.empty();
  request.hasStats = statCellCount > 0;
  const dashboard::CardLayout layout = dashboard::computeCardLayout(rect, metrics, request);
  if (layout.card.width <= 0 || layout.card.height <= 0) return rect.y;

  // The layout may have had to give up a line to fit the band; re-wrap so the last visible line
  // ellipsizes rather than the tail simply vanishing.
  if (layout.maxTitleLines < static_cast<int>(titleLines.size())) {
    titleLines =
        renderer.wrappedText(kTitleFontId, title, layout.titleMaxWidth, layout.maxTitleLines, EpdFontFamily::BOLD);
  }

  drawCardFrame(renderer, layout.card, data.selected);
  drawCardIdentity(renderer, layout, metrics, data, titleLines);
  drawCardStats(renderer, layout, statCells, statCellCount);
  return layout.card.y + layout.card.height;
}

int DashboardTheme::menuRowHeight(const GfxRenderer& renderer) {
  // Text-aware: on the zh-TW profile the menu labels render through the 14pt CJK fallback, whose
  // line is taller than UI_12's builtin Latin advance. Measuring one representative translated
  // label is what keeps the row from clipping its own text there.
  const int lineHeight = renderer.getLineHeight(kMenuFontId, tr(STR_BROWSE_FILES), EpdFontFamily::BOLD);
  return std::max(kMenuRowMinHeight, lineHeight + kMenuRowPaddingY);
}

void DashboardTheme::drawButtonMenu(GfxRenderer& renderer, const Rect rect, const int buttonCount,
                                    const int selectedIndex, const std::function<std::string(int index)>& buttonLabel,
                                    const std::function<UIIcon(int index)>& rowIcon) const {
  (void)rowIcon;  // full-width rows, label only -- the card above carries the only iconography
  if (buttonCount <= 0 || buttonLabel == nullptr) return;

  const int rowHeight = menuRowHeight(renderer);
  const int rowStep = rowHeight + kMenuRowGap;
  const int rowX = rect.x + kMenuSideInset;
  const int rowW = std::max(0, rect.width - kMenuSideInset * 2);
  const int menuTop = rect.y + kMenuTopGap;
  // rect.height comes from HomeActivity's legacy header-based formula and overstates the real
  // space, so the page size is bounded by the screen instead: the button-hints band is the true
  // floor, and metrics.buttonHintsHeight is already 0 when hints are hidden.
  const int availableH =
      std::max(rowHeight, renderer.getScreenHeight() - UITheme::getInstance().getMetrics().buttonHintsHeight - menuTop);
  const int pageItems = std::max(1, (availableH + kMenuRowGap) / rowStep);
  const int safeSelected = std::clamp(selectedIndex, 0, buttonCount - 1);
  const int pageStart = (safeSelected / pageItems) * pageItems;

  std::string labelBatch;
  for (int i = pageStart; i < buttonCount && i < pageStart + pageItems; ++i) {
    labelBatch += buttonLabel(i);
    labelBatch.push_back('\n');
  }
  renderer.prewarmTextRuns({{kMenuFontId, labelBatch.c_str(), EpdFontFamily::BOLD}});

  for (int i = pageStart; i < buttonCount && i < pageStart + pageItems; ++i) {
    const int rowY = menuTop + (i - pageStart) * rowStep;
    // Compared against the RAW index, not the clamped one: HomeActivity passes a negative index
    // when the Home selection is on the card above the menu, and no row should light up then.
    // safeSelected exists only to keep pagination in range. Same split every other theme's
    // drawButtonMenu uses.
    const bool selected = i == selectedIndex;
    renderer.fillRoundedRect(rowX, rowY, rowW, rowHeight, kMenuRowRadius, selected ? Color::Black : Color::White);
    if (!selected) {
      renderer.drawRoundedRect(rowX, rowY, rowW, rowHeight, 1, kMenuRowRadius, true);
    }
    const std::string label = buttonLabel(i);
    const int maxLabelW = std::max(0, rowW - kMenuRowTextInset * 2);
    const std::string shown = renderer.truncatedText(kMenuFontId, label.c_str(), maxLabelW, EpdFontFamily::BOLD);
    const int textY = rowY + (rowHeight - renderer.getLineHeight(kMenuFontId, shown.c_str(), EpdFontFamily::BOLD)) / 2;
    renderer.drawText(kMenuFontId, rowX + kMenuRowTextInset, textY, shown.c_str(), !selected, EpdFontFamily::BOLD);
  }

  if (buttonCount > pageItems) {
    constexpr int scrollW = 4;
    const int trackH = std::max(1, pageItems * rowStep - kMenuRowGap);
    const int thumbH = std::max(12, trackH * pageItems / buttonCount);
    const int maxStart = std::max(1, buttonCount - pageItems);
    const int thumbY = menuTop + (std::min(pageStart, maxStart) * std::max(0, trackH - thumbH)) / maxStart;
    renderer.fillRoundedRect(rect.x + rect.width - kMenuSideInset / 2 - scrollW, thumbY, scrollW, thumbH, scrollW / 2,
                             Color::Black);
  }
}

int DashboardTheme::getMenuBottomEdge(const GfxRenderer& renderer, const int menuTop, const int itemCount) const {
  if (itemCount <= 0) return menuTop;
  const int rowHeight = menuRowHeight(renderer);
  const int rowStep = rowHeight + kMenuRowGap;
  return menuTop + kMenuTopGap + (itemCount - 1) * rowStep + rowHeight;
}
