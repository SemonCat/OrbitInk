#include "JournalTheme.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>

#include <algorithm>
#include <string>

#include "CrossPointSettings.h"
#include "fontIds.h"

namespace {
constexpr int kTextPadX = 8;
constexpr int kMaxValueWidth = 200;
constexpr int kRowBracketLength = 10;
constexpr int kRowBracketStroke = 2;

// Draws a full-width horizontal rule `thickness` px tall starting at y.
void hRule(const GfxRenderer& renderer, const int x, const int y, const int w, const int thickness) {
  renderer.fillRect(x, y, w, thickness, true);
}
}  // namespace

void JournalTheme::drawCropBrackets(const GfxRenderer& renderer, const Rect rect, const int outset, const int length,
                                    const int stroke) {
  const int x0 = rect.x - outset;
  const int y0 = rect.y - outset;
  const int x1 = rect.x + rect.width + outset;  // exclusive
  const int y1 = rect.y + rect.height + outset;
  const int len = std::min(length, std::min(x1 - x0, y1 - y0) / 2);
  // top-left
  renderer.fillRect(x0, y0, len, stroke, true);
  renderer.fillRect(x0, y0, stroke, len, true);
  // top-right
  renderer.fillRect(x1 - len, y0, len, stroke, true);
  renderer.fillRect(x1 - stroke, y0, stroke, len, true);
  // bottom-left
  renderer.fillRect(x0, y1 - stroke, len, stroke, true);
  renderer.fillRect(x0, y1 - len, stroke, len, true);
  // bottom-right
  renderer.fillRect(x1 - len, y1 - stroke, len, stroke, true);
  renderer.fillRect(x1 - stroke, y1 - len, stroke, len, true);
}

void JournalTheme::drawHeader(const GfxRenderer& renderer, const Rect rect, const char* title,
                              const char* subtitle) const {
  renderer.prewarmTextRuns({{UI_12_FONT_ID, title, EpdFontFamily::BOLD}, {SMALL_FONT_ID, subtitle}});
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
  const auto& m = JournalMetrics::values;

  const bool showPct = SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  drawBatteryRight(
      renderer, Rect{rect.x + rect.width - 12 - m.batteryWidth, rect.y + 5, m.batteryWidth, m.batteryHeight}, showPct);

  // Editorial double rule closes the header: thick, then thin.
  const int thickY = rect.y + rect.height - 6;
  const int ruleX = rect.x + m.contentSidePadding;
  const int ruleW = rect.width - 2 * m.contentSidePadding;

  const int available = rect.width - m.contentSidePadding * 3;
  int subtitleW = subtitle ? renderer.getTextWidth(SMALL_FONT_ID, subtitle) : 0;
  subtitleW = std::min(subtitleW, available / 2);
  if (title && title[0]) {
    const int titleLine = renderer.getLineHeight(UI_12_FONT_ID, title, EpdFontFamily::BOLD);
    const int titleY = std::max(rect.y + m.batteryBarHeight - 8, thickY - 8 - titleLine);
    const auto t = renderer.truncatedText(UI_12_FONT_ID, title, available - subtitleW, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, ruleX, titleY, t.c_str(), true, EpdFontFamily::BOLD);
    hRule(renderer, ruleX, thickY, ruleW, 2);
    hRule(renderer, ruleX, thickY + 4, ruleW, 1);
  }
  if (subtitle && subtitle[0]) {
    const auto s = renderer.truncatedText(SMALL_FONT_ID, subtitle, subtitleW);
    const int sw = renderer.getTextWidth(SMALL_FONT_ID, s.c_str());
    const int sy = thickY - 8 - renderer.getLineHeight(SMALL_FONT_ID, s.c_str());
    renderer.drawText(SMALL_FONT_ID, ruleX + ruleW - sw, sy, s.c_str(), true);
  }
}

void JournalTheme::drawSubHeader(const GfxRenderer& renderer, const Rect rect, const char* label,
                                 const char* rightLabel) const {
  renderer.prewarmTextRuns({{UI_10_FONT_ID, label, EpdFontFamily::BOLD}, {SMALL_FONT_ID, rightLabel}});
  const auto& m = JournalMetrics::values;
  const int x = rect.x + m.contentSidePadding;
  const int w = rect.width - 2 * m.contentSidePadding;
  int rightW = 0;
  if (rightLabel) {
    const auto r = renderer.truncatedText(SMALL_FONT_ID, rightLabel, kMaxValueWidth);
    rightW = renderer.getTextWidth(SMALL_FONT_ID, r.c_str()) + kTextPadX;
    renderer.drawText(SMALL_FONT_ID, x + w - rightW + kTextPadX, rect.y + 7, r.c_str());
  }
  const auto l = renderer.truncatedText(UI_10_FONT_ID, label, w - rightW, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, x, rect.y + 6, l.c_str(), true, EpdFontFamily::BOLD);
  hRule(renderer, x, rect.y + rect.height - 1, w, 1);
}

void JournalTheme::drawTabBar(const GfxRenderer& renderer, const Rect rect, const std::vector<TabInfo>& tabs,
                              const bool selected) const {
  std::vector<GfxRenderer::TextPrewarmRun> runs;
  runs.reserve(tabs.size());
  for (const auto& tab : tabs) runs.push_back({UI_10_FONT_ID, tab.label});
  renderer.prewarmTextRuns(runs);
  const auto& m = JournalMetrics::values;
  // Same horizontal geometry as Lyra, so LyraTheme::tabIndexFromPoint keeps hit-testing correctly.
  int x = rect.x + m.contentSidePadding;
  for (const auto& tab : tabs) {
    const int textW = renderer.getTextWidth(UI_10_FONT_ID, tab.label);
    const int tabW = textW + 2 * kTextPadX;
    const auto style = tab.selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    renderer.drawText(UI_10_FONT_ID, x + kTextPadX, rect.y + 6, tab.label, true, style);
    if (tab.selected) {
      hRule(renderer, x + kTextPadX, rect.y + rect.height - 5, textW, 2);
      if (selected) {
        drawCropBrackets(renderer, Rect{x + 2, rect.y + 2, tabW - 4, rect.height - 6}, 0, kRowBracketLength,
                         kRowBracketStroke);
      }
    }
    x += tabW + m.tabSpacing;
  }
  hRule(renderer, rect.x + m.contentSidePadding, rect.y + rect.height - 1, rect.width - 2 * m.contentSidePadding, 1);
}

int JournalTheme::getListRowStep(const bool hasSubtitle) const {
  return hasSubtitle ? JournalMetrics::values.listWithSubtitleRowHeight : JournalMetrics::values.listRowHeight;
}

void JournalTheme::drawList(const GfxRenderer& renderer, const Rect rect, const int itemCount, const int selectedIndex,
                            const std::function<std::string(int index)>& rowTitle,
                            const std::function<std::string(int index)>& rowSubtitle,
                            const std::function<UIIcon(int index)>& rowIcon,
                            const std::function<std::string(int index)>& rowValue, const bool highlightValue,
                            const std::function<bool(int index)>& rowDimmed) const {
  (void)rowIcon;  // editorial lists are typographic
  const auto& m = JournalMetrics::values;
  const int rowHeight = getListRowStep(rowSubtitle != nullptr);
  const int pageItems = rowHeight > 0 ? std::max(1, rect.height / rowHeight) : 1;
  const int pageStart = std::max(0, selectedIndex / pageItems) * pageItems;
  const int pageEnd = std::min(itemCount, pageStart + pageItems);

  std::string titleBatch, subtitleBatch, valueBatch;
  for (int i = pageStart; i < pageEnd; i++) {
    titleBatch += rowTitle(i);
    titleBatch.push_back('\n');
    if (rowSubtitle) {
      subtitleBatch += rowSubtitle(i);
      subtitleBatch.push_back('\n');
    }
    if (rowValue) {
      valueBatch += rowValue(i);
      valueBatch.push_back('\n');
    }
  }
  renderer.prewarmTextRuns({{UI_10_FONT_ID, titleBatch.c_str()},
                            {UI_10_FONT_ID, titleBatch.c_str(), EpdFontFamily::BOLD},
                            {SMALL_FONT_ID, subtitleBatch.c_str()},
                            {UI_10_FONT_ID, valueBatch.c_str()}});

  const int totalPages = (itemCount + pageItems - 1) / pageItems;
  if (totalPages > 1) {
    const int barH = std::max(8, (rect.height * pageItems) / std::max(1, itemCount));
    const int page = selectedIndex / pageItems;
    const int barY = rect.y + ((rect.height - barH) * page) / (totalPages - 1);
    const int barX = rect.x + rect.width - m.scrollBarRightOffset;
    renderer.drawLine(barX, rect.y, barX, rect.y + rect.height, true);
    renderer.fillRect(barX - m.scrollBarWidth, barY, m.scrollBarWidth, barH, true);
  }

  const int left = rect.x + m.contentSidePadding;
  const int width = rect.width - 2 * m.contentSidePadding;
  const int textX = left + kTextPadX;
  const int textW = width - 2 * kTextPadX;

  for (int i = pageStart; i < pageEnd; i++) {
    const int rowY = rect.y + (i - pageStart) * rowHeight;
    const bool isSelected = i == selectedIndex;
    const auto style = isSelected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;

    std::string value = rowValue ? rowValue(i) : std::string();
    int valueW = 0;
    if (!value.empty()) {
      value = renderer.truncatedText(UI_10_FONT_ID, value.c_str(), kMaxValueWidth);
      valueW = renderer.getTextWidth(UI_10_FONT_ID, value.c_str()) + kTextPadX;
    }
    const std::string titleText = rowTitle(i);
    const auto title = renderer.truncatedText(UI_10_FONT_ID, titleText.c_str(), textW - valueW, style);
    const int titleLine = renderer.getLineHeight(UI_10_FONT_ID, title.c_str(), style);

    std::string subtitle;
    int subLine = 0;
    if (rowSubtitle) {
      const std::string subText = rowSubtitle(i);
      if (!subText.empty()) {
        subtitle = renderer.truncatedText(SMALL_FONT_ID, subText.c_str(), textW - valueW);
        subLine = renderer.getLineHeight(SMALL_FONT_ID, subtitle.c_str());
      }
    }
    // Center the text block in the row from the measured (possibly CJK) line heights.
    const int blockH = titleLine + subLine;
    const int titleY = rowY + std::max(2, (rowHeight - 1 - blockH) / 2);
    renderer.drawText(UI_10_FONT_ID, textX, titleY, title.c_str(), true, style);
    if (rowDimmed && rowDimmed(i) && !isSelected) {
      const int tw = renderer.getTextWidth(UI_10_FONT_ID, title.c_str(), style);
      for (int py = titleY; py < titleY + titleLine; py++)
        for (int px = textX; px < textX + tw; px++)
          if ((px + py) % 2 == 0) renderer.drawPixel(px, py, false);
    }
    if (!subtitle.empty()) renderer.drawText(SMALL_FONT_ID, textX, titleY + titleLine, subtitle.c_str(), true);
    if (!value.empty()) {
      const int valueY = rowY + (rowHeight - 1 - renderer.getLineHeight(UI_10_FONT_ID, value.c_str())) / 2;
      const auto valueStyle = isSelected && highlightValue ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
      renderer.drawText(UI_10_FONT_ID, left + width - valueW, valueY, value.c_str(), true, valueStyle);
    }

    // Hairline between rows; the selected row gets crop-corner brackets instead of a fill.
    if (i + 1 < pageEnd) renderer.drawLine(left, rowY + rowHeight - 1, left + width - 1, rowY + rowHeight - 1, true);
    if (isSelected) {
      drawCropBrackets(renderer, Rect{left, rowY + 3, width, rowHeight - 7}, 0, kRowBracketLength, kRowBracketStroke);
    }
  }
}

void JournalTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                   const char* btn4) const {
  if (gpio.hasTouch() || SETTINGS.hideButtonHints) return;
  const GfxRenderer::Orientation orig = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& m = JournalMetrics::values;
  const int top = pageHeight - m.buttonHintsHeight;
  // Same physical slots as Lyra: the labels must sit over the buttons they name.
  constexpr int kButtonWidth = 80;
  constexpr int x4Positions[] = {58, 146, 254, 342};
  constexpr int x3Positions[] = {65, 157, 291, 383};
  const int* positions = gpio.deviceIsX3() ? x3Positions : x4Positions;
  const char* labels[] = {btn1, btn2, btn3, btn4};
  renderer.prewarmTextRuns(
      {{SMALL_FONT_ID, btn1}, {SMALL_FONT_ID, btn2}, {SMALL_FONT_ID, btn3}, {SMALL_FONT_ID, btn4}});

  renderer.fillRect(0, top, pageWidth, m.buttonHintsHeight, false);
  hRule(renderer, m.contentSidePadding, top, pageWidth - 2 * m.contentSidePadding, 1);
  for (int i = 0; i < 4; i++) {
    const int x = positions[i];
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      const int tw = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
      const int th = renderer.getLineHeight(SMALL_FONT_ID, labels[i]);
      const int ty = top + std::max(2, (m.buttonHintsHeight - th) / 2);
      renderer.drawText(SMALL_FONT_ID, x + (kButtonWidth - tw) / 2, ty, labels[i]);
    } else {
      // Unlabeled button: a short tick keeps the slot's position readable.
      renderer.fillRect(x + kButtonWidth / 2 - 6, pageHeight - 6, 12, 2, true);
    }
  }
  renderer.setOrientation(orig);
}
