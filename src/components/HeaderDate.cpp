// Adapted from CrossInk (uxjulia/CrossInk 9914146, src/components/HeaderDate.cpp, MIT).
// CrossInk formats through HalClock::formatDate() and its dateFormat/dateSeparator settings,
// neither of which exists in this firmware's HAL or settings. This keeps CrossInk's public API
// and layout (HeaderDate.h is taken verbatim) and derives the date from the HAL's existing UTC
// epoch-minute clock plus the user's existing clock UTC offset, in a fixed YYYY/MM/DD form.
#include "HeaderDate.h"

#include <CalendarTime.h>
#include <GfxRenderer.h>
#include <HalClock.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>

#include "CrossPointSettings.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"

namespace {
constexpr int kHeaderDateRightInset = 12;
constexpr int kHeaderDateBottomGap = 10;
// RTC values before this are an unset clock, not a real date (2024-01-01T00:00Z in minutes).
constexpr uint32_t kMinPlausibleEpochMinutes = CalendarTime::epochMinutes(2024, 1, 1, 0, 0);

bool formatHeaderDateImpl(char* buf, const size_t len) {
  if (!halClock.isAvailable()) return false;
  if (!SETTINGS.clockHasBeenSynced) return false;
  uint32_t epochMinutes = 0;
  if (!halClock.getEpochMinutes(epochMinutes) || epochMinutes < kMinPlausibleEpochMinutes) return false;
  // clockUtcOffsetQ is biased quarter hours: 48 = UTC+0.
  const int64_t offsetMinutes = (static_cast<int64_t>(SETTINGS.clockUtcOffsetQ) - 48) * 15;
  const int64_t localMinutes = static_cast<int64_t>(epochMinutes) + offsetMinutes;
  int year = 0;
  unsigned month = 0;
  unsigned day = 0;
  CalendarTime::civilFromDays(localMinutes / (24 * 60), year, month, day);
  const int written = snprintf(buf, len, "%04d/%02u/%02u", year, month, day);
  return written > 0 && static_cast<size_t>(written) < len;
}
}  // namespace

bool formatHeaderDateText(char* buffer, const size_t length) { return formatHeaderDateImpl(buffer, length); }

int headerDateReservedWidth(const GfxRenderer& renderer) {
  char dateBuf[32];
  if (!formatHeaderDateImpl(dateBuf, sizeof(dateBuf))) return 0;

  return renderer.getTextWidth(UI_10_FONT_ID, dateBuf) + kHeaderDateRightInset;
}

int headerDateLineBottomY(const GfxRenderer&, const ThemeMetrics& metrics, const int headerHeight) {
  const int effectiveHeaderHeight = headerHeight >= 0 ? headerHeight : metrics.headerHeight;
  return metrics.topPadding + effectiveHeaderHeight - kHeaderDateBottomGap;
}

void drawHeaderDate(const GfxRenderer& renderer, const int pageWidth, const ThemeMetrics& metrics,
                    const int headerHeight) {
  drawHeaderDateAtLineBottom(renderer, pageWidth, headerDateLineBottomY(renderer, metrics, headerHeight));
}

void drawHeaderDateAtLineBottom(const GfxRenderer& renderer, const int pageWidth, const int lineBottomY) {
  constexpr int dateFontId = UI_10_FONT_ID;
  drawHeaderDateAtBaseline(renderer, pageWidth,
                           lineBottomY - renderer.getLineHeight(dateFontId) + renderer.getFontAscenderSize(dateFontId));
}

void drawHeaderDateAtBaseline(const GfxRenderer& renderer, const int pageWidth, const int baselineY) {
  char dateBuf[32];
  if (!formatHeaderDateImpl(dateBuf, sizeof(dateBuf))) return;

  constexpr int dateFontId = UI_10_FONT_ID;
  const int textWidth = renderer.getTextWidth(dateFontId, dateBuf);
  const int dateX = pageWidth - kHeaderDateRightInset - textWidth;
  const int dateY = baselineY - renderer.getFontAscenderSize(dateFontId);
  renderer.drawText(dateFontId, std::max(0, dateX), std::max(0, dateY), dateBuf);
}
