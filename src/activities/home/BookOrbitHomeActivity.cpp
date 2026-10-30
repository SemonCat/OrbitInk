#include "BookOrbitHomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>

#include "BookOrbitCredentialStore.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "RecentBookProgress.h"
#include "activities/settings/BookOrbitSettingsActivity.h"
#include "activities/settings/BookOrbitSetupGuideActivity.h"
#include "activities/settings/BookOrbitSetupPolicy.h"
#include "components/CompactHeader.h"
#include "components/HeaderDate.h"
#include "components/UITheme.h"
#include "components/themes/journal/JournalHomeLayout.h"
#include "components/themes/journal/JournalText.h"
#include "components/themes/journal/JournalTheme.h"
#include "components/themes/lyra/LyraTheme.h"
#include "fontIds.h"
#include "util/BookOrbitLibraryStore.h"
#include "util/BookOrbitStatsSync.h"
#include "util/InputReleaseGuard.h"

namespace {
constexpr int HERO_RADIUS = 10;
constexpr int TILE_RADIUS = 8;
constexpr int FOCUS_STROKE = 3;
constexpr int HERO_PADDING = 14;
}  // namespace

void BookOrbitHomeActivity::onEnter() {
  Activity::onEnter();
  // First-run guide while BookOrbit is incomplete and the first run is not handled ("Later", or complete
  // once). This Home exists only for the BookOrbit-first themes, and reader resume never passes through
  // it. It returns here via onEnter().
  BookOrbitSetupGuideActivity::recordHandledIfConfigured();
  const uint8_t missing = BookOrbitSetupPolicy::missingFields(!BOOKORBIT_STORE.getServerUrl().empty(),
                                                              !BOOKORBIT_STORE.getUsername().empty(),
                                                              !BOOKORBIT_STORE.getPassword().empty());
  if (BookOrbitSetupPolicy::shouldShowGuide(CrossPointSettings::isBookOrbitHomeTheme(SETTINGS.uiTheme), missing,
                                            SETTINGS.bookOrbitSetupGuideDismissed != 0)) {
    startActivityForResult(std::make_unique<BookOrbitSetupGuideActivity>(renderer, mappedInput),
                           [this](const ActivityResult&) { onEnter(); });
    return;
  }
  bookOrbitConfigured = !BookOrbitStats::currentOwner().isZero();
  journal = SETTINGS.uiTheme == CrossPointSettings::UI_THEME::JOURNAL;
  for (bool& tried : journalCoverTried) tried = false;

  shown.clear();
  shown.reserve(MAX_RECENT);
  for (const RecentBook& book : RECENT_BOOKS.getBooks()) {
    if (static_cast<int>(shown.size()) >= MAX_RECENT) break;
    if (RecentBooksStore::isMissing(book)) continue;
    Shown s;
    s.book = book;
    s.percent = RecentBookProgress::loadPercent(book.path);  // progress.bin + book.bin only
    shown.push_back(std::move(s));
  }
  // 已下載 markers: one index scan for all shown books, by local path.
  if (bookOrbitConfigured && !shown.empty()) {
    auto library =
        makeUniqueNoThrow<BookOrbitLibrary::Store>(BookOrbitStats::currentOwner(), bookOrbitFirmwareSha256());
    if (library) {
      for (auto& s : shown) {
        BookOrbitLibrary::Entry e;
        s.downloaded = library->findByPath(s.book.path, e);
      }
    }
  }

  switch (initialMenuItem) {
    case HomeMenuItem::BOOKORBIT_LIBRARY:
      focus = tileFocus(Tile::Library);
      break;
    case HomeMenuItem::SETTINGS_MENU:
      focus = tileFocus(Tile::Settings);
      break;
    default:
      focus = 0;
      break;
  }
  backReleasePending = true;
  confirmReleasePending = true;
  firstRenderDone = false;
  coverPassDone = false;
  requestUpdate();
}

void BookOrbitHomeActivity::loop() {
  if (InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Back, backReleasePending)) {
    return;
  }
  if (InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Confirm,
                                               confirmReleasePending)) {
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // As on the existing Home: Back resumes the current book.
    if (!shown.empty()) activityManager.goToReader(shown[0].book.path, false, /*checkRemoteProgress=*/true);
    return;
  }
  const int count = focusCount();
  buttonNavigator.onNextRelease([this, count] {
    focus = ButtonNavigator::nextIndex(focus, count);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this, count] {
    focus = ButtonNavigator::previousIndex(focus, count);
    requestUpdate();
  });
}

void BookOrbitHomeActivity::activate() {
  if (focus < tileFocus(Tile::Library)) {
    if (shown.empty()) {
      openTile(Tile::Library);  // empty hero: "pick a book from your library"
    } else {
      activityManager.goToReader(shown[focus].book.path, false, /*checkRemoteProgress=*/true);
    }
    return;
  }
  openTile(static_cast<Tile>(focus - tileFocus(Tile::Library)));
}

void BookOrbitHomeActivity::openTile(const Tile t) {
  if ((t == Tile::Library || t == Tile::Search) && !bookOrbitConfigured) {
    startActivityForResult(std::make_unique<BookOrbitSettingsActivity>(renderer, mappedInput),
                           [this](const ActivityResult&) { onEnter(); });
    return;
  }
  switch (t) {
    case Tile::Library:
      activityManager.goToBookOrbitLibrary(false);
      break;
    case Tile::Search:
      activityManager.goToBookOrbitLibrary(true);
      break;
    case Tile::Settings:
      activityManager.goToSettings();
      break;
  }
}

void BookOrbitHomeActivity::drawHero(const int x, const int y, const int w, const int h, const bool focused) {
  renderer.fillRoundedRect(x, y, w, h, HERO_RADIUS, Color::Black);
  if (focused) {
    renderer.drawRoundedRect(x + 4, y + 4, w - 8, h - 8, FOCUS_STROKE, HERO_RADIUS - 2, false);
  }
  const int line12 = renderer.getLineHeight(UI_12_FONT_ID);
  const int line10 = renderer.getLineHeight(UI_10_FONT_ID);

  if (shown.empty()) {
    UITheme::drawCenteredWrappedText(renderer, Rect{x + HERO_PADDING, y, w - 2 * HERO_PADDING, h}, UI_12_FONT_ID,
                                     tr(STR_BO_PICK_A_BOOK), 2, false, EpdFontFamily::BOLD);
    return;
  }
  const Shown& s = shown[0];
  int textX = x + HERO_PADDING;
  const int innerH = h - 2 * HERO_PADDING;

  // Cover only if a thumbnail already exists on SD (see loadHeroCover()). Fitted with ONE scale
  // so the image keeps its aspect ratio, capped to a third of the card width and the card's
  // inner height, and vertically centered in the card.
  if (!s.book.coverBmpPath.empty()) {
    const std::string thumb = UITheme::getCoverThumbPath(s.book.coverBmpPath, LyraMetrics::values.homeCoverHeight);
    HalFile file;
    if (Storage.exists(thumb.c_str()) && Storage.openFileForRead("BOHOME", thumb, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0) {
        const int maxW = w / 3;
        const int maxH = innerH;
        const float scale =
            std::min(static_cast<float>(maxW) / bitmap.getWidth(), static_cast<float>(maxH) / bitmap.getHeight());
        const int coverW = std::max(1, static_cast<int>(bitmap.getWidth() * scale));
        const int coverH = std::max(1, static_cast<int>(bitmap.getHeight() * scale));
        const int coverX = x + HERO_PADDING;
        const int coverY = y + (h - coverH) / 2;
        renderer.fillRect(coverX - 2, coverY - 2, coverW + 4, coverH + 4, false);  // white frame on black
        renderer.drawBitmap(bitmap, coverX, coverY, coverW, coverH);
        textX += coverW + HERO_PADDING;
      }
    }
  }
  const int textW = x + w - HERO_PADDING - textX;
  int ty = y + HERO_PADDING;
  renderer.drawText(UI_10_FONT_ID, textX, ty, tr(STR_CONTINUE_READING), false);
  ty += line10 + 4;
  const auto titleLines = renderer.wrappedText(UI_12_FONT_ID, s.book.title.c_str(), textW, 2, EpdFontFamily::BOLD);
  for (const auto& l : titleLines) {
    renderer.drawText(UI_12_FONT_ID, textX, ty, l.c_str(), false, EpdFontFamily::BOLD);
    ty += line12;
  }
  if (!s.book.author.empty()) {
    const auto author = renderer.truncatedText(UI_10_FONT_ID, s.book.author.c_str(), textW);
    renderer.drawText(UI_10_FONT_ID, textX, ty, author.c_str(), false);
    ty += line10;
  }

  // Bottom of the hero: progress bar + percentage, then the resume pill.
  constexpr int BAR_H = 12;
  const int pillH = line10 + 12;
  const int pillY = y + h - HERO_PADDING - pillH;
  const int pctY = pillY - 10 - line10;
  if (RecentBookProgress::hasPercent(s.percent)) {
    char pct[8];
    RecentBookProgress::formatPercent(s.percent, pct, sizeof(pct));
    const int pctW = renderer.getTextWidth(UI_10_FONT_ID, pct, EpdFontFamily::BOLD);
    const int barW = std::max(20, textW - pctW - 12);
    const int barY = pctY + (line10 - BAR_H) / 2;
    renderer.drawRect(textX, barY, barW, BAR_H, 2, false);
    renderer.fillRect(textX, barY, static_cast<int>(barW * s.percent / 100.0f), BAR_H, false);
    renderer.drawText(UI_10_FONT_ID, textX + barW + 12, pctY, pct, false, EpdFontFamily::BOLD);
  }
  const char* resume = tr(STR_CONTINUE_READING);
  const int pillW = renderer.getTextWidth(UI_10_FONT_ID, resume, EpdFontFamily::BOLD) + 28;
  renderer.fillRoundedRect(textX, pillY, pillW, pillH, pillH / 2, Color::White);
  renderer.drawText(UI_10_FONT_ID, textX + 14, pillY + 6, resume, true, EpdFontFamily::BOLD);
}

void BookOrbitHomeActivity::drawRow(const Shown& s, const int x, const int y, const int w, const int h,
                                    const bool focused) {
  renderer.drawRoundedRect(x, y, w, h, focused ? FOCUS_STROKE : 1, TILE_RADIUS, true);
  const int line10 = renderer.getLineHeight(UI_10_FONT_ID);
  char right[32];
  char pct[8];
  RecentBookProgress::formatPercent(s.percent, pct, sizeof(pct));  // "" when unknown
  if (s.downloaded && pct[0]) {
    snprintf(right, sizeof(right), "%s · %s", tr(STR_BO_DOWNLOADED), pct);
  } else {
    snprintf(right, sizeof(right), "%s", s.downloaded ? tr(STR_BO_DOWNLOADED) : pct);
  }
  const int rightW = right[0] ? renderer.getTextWidth(UI_10_FONT_ID, right) + 12 : 0;
  const int textY = y + (h - line10) / 2;
  const auto title = renderer.truncatedText(UI_10_FONT_ID, s.book.title.c_str(), w - 24 - rightW);
  renderer.drawText(UI_10_FONT_ID, x + 12, textY, title.c_str(), true,
                    focused ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  if (right[0]) renderer.drawText(UI_10_FONT_ID, x + w - rightW, textY, right, true);
}

void BookOrbitHomeActivity::drawTile(const char* label, const int x, const int y, const int w, const int h,
                                     const bool focused) {
  if (focused) {
    renderer.fillRoundedRect(x, y, w, h, TILE_RADIUS, Color::Black);
  } else {
    renderer.drawRoundedRect(x, y, w, h, 2, TILE_RADIUS, true);
  }
  UITheme::drawCenteredWrappedText(renderer, Rect{x + 6, y, w - 12, h}, UI_12_FONT_ID, label, 2, !focused,
                                   EpdFontFamily::BOLD);
}

void BookOrbitHomeActivity::render(RenderLock&&) {
  if (journal) {
    renderJournal();
    return;
  }
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  CompactHeader::drawTitle(renderer, tr(STR_BO_MY_LIBRARY), /*showDate=*/true);
  const int side = metrics.contentSidePadding;
  const int width = pageWidth - 2 * side;
  const int top = CompactHeader::contentTop(metrics);
  const int bottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int available = bottom - top;
  const int gap = metrics.verticalSpacing;

  const int line10 = renderer.getLineHeight(UI_10_FONT_ID);
  const int tileH = std::max(renderer.getLineHeight(UI_12_FONT_ID) * 2 + 20, available / 7);
  const int rowH = line10 + 22;
  const int rows = std::max(0, bookSlots() - 1);
  const int labelH = rows > 0 ? line10 + 6 : 0;
  // The hero holds a fixed amount of content (label, 2-line title, author, progress, pill), so it
  // is capped rather than stretched into an empty black slab when there are no other rows.
  const int heroMax = renderer.getLineHeight(UI_12_FONT_ID) * 2 + line10 * 5 + 2 * HERO_PADDING + 40;
  const int heroH = std::min(heroMax, available - tileH - gap - (rows > 0 ? labelH + rows * (rowH + 8) + gap : 0));

  int y = top;
  drawHero(side, y, width, heroH, focus == 0);
  y += heroH + gap;

  if (rows > 0) {
    renderer.drawText(UI_10_FONT_ID, side, y, tr(STR_BO_ALSO_READING), true, EpdFontFamily::BOLD);
    y += labelH;
    for (int i = 1; i <= rows; i++) {
      drawRow(shown[i], side, y, width, rowH, focus == i);
      y += rowH + 8;
    }
    y += gap - 8;
  }

  const int tileGap = 12;
  const int tileW = (width - 2 * tileGap) / TILE_COUNT;
  const char* labels[TILE_COUNT] = {bookOrbitConfigured ? tr(STR_BO_LIBRARY) : tr(STR_BO_SET_UP), tr(STR_SEARCH),
                                    tr(STR_SETTINGS_TITLE)};
  for (int t = 0; t < TILE_COUNT; t++) {
    drawTile(labels[t], side + t * (tileW + tileGap), bottom - tileH, tileW, tileH,
             focus == tileFocus(static_cast<Tile>(t)));
  }

  const auto hints =
      mappedInput.mapLabels(shown.empty() ? "" : tr(STR_RESUME), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);

  // Whole-surface repaint (large black hero): flash once on entry, then the default waveform for
  // focus moves -- a conservative reading of the display API, not a measured result.
  if (!firstRenderDone) {
    firstRenderDone = true;
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    LOG_DBG("BOHOME", "First paint done");
  } else {
    renderer.displayBuffer();
  }
  if (!coverPassDone) {
    coverPassDone = true;
    if (loadHeroCover()) requestUpdate();
  }
}

bool BookOrbitHomeActivity::loadHeroCover() {
  // Same rule as the legacy Home's loadRecentCovers(): only AFTER the first paint, only for a book
  // whose metadata cache (book.bin) already exists -- never building it, never on the paint path --
  // and only once, when the thumbnail is missing. A book without a cover simply keeps the
  // typographic hero.
  if (shown.empty() || shown[0].book.coverBmpPath.empty()) return false;
  const RecentBook& book = shown[0].book;
  const std::string thumb = UITheme::getCoverThumbPath(book.coverBmpPath, LyraMetrics::values.homeCoverHeight);
  if (Storage.exists(thumb.c_str()) || !FsHelpers::hasEpubExtension(book.path)) return false;
  Epub epub(book.path, "/.crosspoint");
  if (!epub.load(/*buildIfMissing=*/false, /*skipLoadingCss=*/true)) return false;
  return epub.generateThumbBmp(LyraMetrics::values.homeCoverHeight);
}

// ---- Journal (文藝刊物) layout ----------------------------------------------------------------

namespace {
// Thumbnail path for an exact box, from the recent entry's "[HEIGHT]" template.
std::string journalThumbPath(const RecentBook& book, const int w, const int h) {
  return book.coverBmpPath.empty() ? std::string() : UITheme::getCoverThumbPath(book.coverBmpPath, w, h);
}

void hairline(const GfxRenderer& renderer, const int x, const int y, const int w) { renderer.fillRect(x, y, w, 1); }

// Thin progress bar: an outline with a solid fill, `pct` in 0..100.
void journalProgressBar(const GfxRenderer& renderer, const int x, const int y, const int w, const int h,
                        const float pct) {
  renderer.drawRect(x, y, w, h, 1, true);
  const int fill = std::clamp(static_cast<int>((w - 2) * pct / 100.0f), 0, w - 2);
  if (fill > 0) renderer.fillRect(x + 1, y + 1, fill, h - 2);
}
}  // namespace

void BookOrbitHomeActivity::drawJournalCover(const int slot, const int x, const int y, const int w, const int h) {
  bool drawn = false;
  if (slot >= 0 && slot < bookSlots()) {
    const std::string thumb = journalThumbPath(shown[slot].book, w, h);
    HalFile file;
    if (!thumb.empty() && Storage.exists(thumb.c_str()) && Storage.openFileForRead("BOHOME", thumb, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0) {
        // Letterbox-contained thumbnail: never larger than the box, so center it and frame it.
        const int bw = std::min(w, bitmap.getWidth());
        const int bh = std::min(h, bitmap.getHeight());
        const int bx = x + (w - bw) / 2;
        const int by = y + (h - bh) / 2;
        renderer.drawBitmap(bitmap, bx, by, bw, bh);
        renderer.drawRect(bx - 1, by - 1, bw + 2, bh + 2, 1, true);
        drawn = true;
      }
    }
  }
  if (!drawn) {
    // No cover: a ruled frame with an inner hairline, like an empty plate in a printed page.
    renderer.drawRect(x, y, w, h, 1, true);
    renderer.drawRect(x + 6, y + 6, w - 12, h - 12, 1, true);
  }
}

void BookOrbitHomeActivity::renderJournal() {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const char* masthead = tr(STR_THEME_BOOKORBIT);
  const char* tagline1 = tr(STR_JOURNAL_TAGLINE_1);
  const char* tagline2 = tr(STR_JOURNAL_TAGLINE_2);
  const char* nowLabel = tr(STR_JOURNAL_NOW_READING);
  const char* recentLabel = tr(STR_JOURNAL_RECENT);
  const char* navLabels[TILE_COUNT] = {bookOrbitConfigured ? tr(STR_JOURNAL_NAV_LIBRARY) : tr(STR_BO_SET_UP),
                                       tr(STR_SEARCH), tr(STR_SETTINGS_TITLE)};
  const int rows = std::max(0, bookSlots() - 1);

  // Measure every role from the strings it will actually draw (CJK fallback lines are taller).
  journal::Metrics jm;
  jm.screenWidth = renderer.getScreenWidth();
  jm.screenHeight = renderer.getScreenHeight();
  jm.sidePadding = metrics.contentSidePadding;
  jm.hintsHeight = metrics.buttonHintsHeight;
  jm.statusLine = renderer.getLineHeight(SMALL_FONT_ID, "00:00");
  jm.mastheadLine = renderer.getLineHeight(MASTHEAD_FONT_ID, masthead);
  jm.mastheadWidth = renderer.getTextWidth(MASTHEAD_FONT_ID, masthead);
  jm.taglineLine =
      std::max(renderer.getLineHeight(SMALL_FONT_ID, tagline1), renderer.getLineHeight(SMALL_FONT_ID, tagline2));
  jm.taglineWidth =
      std::max(renderer.getTextWidth(SMALL_FONT_ID, tagline1), renderer.getTextWidth(SMALL_FONT_ID, tagline2));
  jm.stripLine = std::max(renderer.getLineHeight(SMALL_FONT_ID, "0000/00/00"),
                          renderer.getLineHeight(SMALL_FONT_ID, tr(STR_BO_MY_LIBRARY)));
  jm.labelLine = std::max(renderer.getLineHeight(UI_10_FONT_ID, nowLabel, EpdFontFamily::BOLD),
                          renderer.getLineHeight(UI_10_FONT_ID, recentLabel, EpdFontFamily::BOLD));
  // Hero headline: the reader font is already resident (no extra SD font memory) and much larger
  // than the shared 14pt UI fallback, so it gives the headline real hierarchy. It is used only
  // when it is clearly larger than UI_12 and draws every glyph of the title; otherwise UI_12 bold.
  int heroFont = UI_12_FONT_ID;
  EpdFontFamily::Style heroStyle = EpdFontFamily::BOLD;
  const char* heroText = shown.empty() ? tr(STR_BO_PICK_A_BOOK) : shown[0].book.title.c_str();
  if (!shown.empty()) {
    const int readerFont = SETTINGS.getReaderFontId();
    const auto style = renderer.isSdCardFont(readerFont) ? EpdFontFamily::REGULAR : EpdFontFamily::BOLD;
    renderer.ensureSdCardFontReady(readerFont, heroText);
    const auto it = renderer.getFontMap().find(readerFont);
    bool covers = it != renderer.getFontMap().end();
    for (const char* c = heroText; covers && *c;) {
      const uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&c));
      if (cp > ' ' && !it->second.hasCodepoint(cp, style)) covers = false;
    }
    if (covers && renderer.getLineHeight(readerFont) >= renderer.getLineHeight(UI_12_FONT_ID, heroText) + 8) {
      heroFont = readerFont;
      heroStyle = style;
    }
  }
  jm.titleLine = renderer.getLineHeight(heroFont, heroText, heroStyle);
  jm.bodyLine = renderer.getLineHeight(UI_10_FONT_ID, navLabels[0]);
  jm.metaLine = renderer.getLineHeight(SMALL_FONT_ID, tr(STR_BO_DOWNLOADED));
  for (const auto& s : shown) {
    jm.bodyLine = std::max(jm.bodyLine, renderer.getLineHeight(UI_10_FONT_ID, s.book.title.c_str()));
    jm.bodyLine = std::max(jm.bodyLine, renderer.getLineHeight(UI_10_FONT_ID, s.book.author.c_str()));
    jm.metaLine = std::max(jm.metaLine, renderer.getLineHeight(SMALL_FONT_ID, s.book.author.c_str()));
  }
  journal::Request req;
  req.recentCount = rows;
  if (!shown.empty()) {
    req.heroHasAuthor = !shown[0].book.author.empty();
    req.heroHasProgress = RecentBookProgress::hasPercent(shown[0].percent);
  }
  for (int i = 1; i < bookSlots(); i++) {
    req.recentHasAuthor = req.recentHasAuthor || !shown[i].book.author.empty();
    req.recentHasMeta = req.recentHasMeta || shown[i].downloaded || RecentBookProgress::hasPercent(shown[i].percent);
  }
  const journal::Layout L = journal::computeLayout(jm, req);
  if (!L.valid) {
    LOG_ERR("BOHOME", "Journal layout does not fit %dx%d", jm.screenWidth, jm.screenHeight);
    renderer.displayBuffer();
    return;
  }

  // Status strip: clock (only when the clock is real) at left, battery at right.
  const auto statusBar = SETTINGS.statusBarSpec();
  if (halClock.isAvailable() && SETTINGS.clockHasBeenSynced) {
    char timeBuf[12];
    if (halClock.formatTime(timeBuf, sizeof(timeBuf), statusBar.clockUtcOffsetQ, statusBar.clock12h)) {
      renderer.drawText(SMALL_FONT_ID, L.status.x, L.status.y, timeBuf);
    }
  }
  const bool showPct = SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  GUI.drawBatteryRight(
      renderer,
      Rect{L.status.x + L.status.width - metrics.batteryWidth,
           L.status.y + (L.status.height - metrics.batteryHeight) / 2, metrics.batteryWidth, metrics.batteryHeight},
      showPct);

  // Masthead, stacked tagline, double rule.
  renderer.drawText(MASTHEAD_FONT_ID, L.masthead.x, L.masthead.y, masthead);
  if (L.tagline.width > 0) {
    const int t1 = renderer.getTextWidth(SMALL_FONT_ID, tagline1);
    const int t2 = renderer.getTextWidth(SMALL_FONT_ID, tagline2);
    const int right = L.tagline.x + L.tagline.width;
    renderer.drawText(SMALL_FONT_ID, right - t1, L.tagline.y, tagline1);
    renderer.drawText(SMALL_FONT_ID, right - t2, L.tagline.y + jm.taglineLine, tagline2);
  }
  const int contentX = L.status.x;
  const int contentW = L.status.width;
  renderer.fillRect(contentX, L.ruleThickY, contentW, journal::kRuleThick);
  hairline(renderer, contentX, L.ruleThinY, contentW);

  // Date (only a real one) at left, the library category at right, closed by a hairline.
  char dateBuf[16];
  if (formatHeaderDateText(dateBuf, sizeof(dateBuf))) renderer.drawText(SMALL_FONT_ID, L.strip.x, L.strip.y, dateBuf);
  const char* category = tr(STR_BO_MY_LIBRARY);
  renderer.drawText(SMALL_FONT_ID, L.strip.x + L.strip.width - renderer.getTextWidth(SMALL_FONT_ID, category),
                    L.strip.y, category);
  hairline(renderer, contentX, L.stripRuleY, contentW);

  // Hero: 正在閱讀 / now reading.
  renderer.drawText(UI_10_FONT_ID, L.heroLabel.x, L.heroLabel.y, nowLabel, true, EpdFontFamily::BOLD);
  if (!shown.empty() && shown[0].downloaded) {
    const char* marker = tr(STR_BO_DOWNLOADED);
    renderer.drawText(SMALL_FONT_ID, L.heroLabel.x + L.heroLabel.width - renderer.getTextWidth(SMALL_FONT_ID, marker),
                      L.heroLabel.y, marker);
  }
  journalCoverBox[0] = {L.heroCover.width, L.heroCover.height};
  drawJournalCover(shown.empty() ? -1 : 0, L.heroCover.x, L.heroCover.y, L.heroCover.width, L.heroCover.height);
  if (focus == 0) {
    JournalTheme::drawCropBrackets(renderer, L.heroCover, journal::kBracketOutset, journal::kBracketLength,
                                   journal::kBracketStroke);
  }
  {
    const Rect& t = L.heroText;
    int ty = t.y;
    if (shown.empty()) {
      const auto lines = renderer.wrappedText(UI_12_FONT_ID, tr(STR_BO_PICK_A_BOOK), t.width, 3, EpdFontFamily::BOLD);
      for (const auto& line : lines) {
        renderer.drawText(UI_12_FONT_ID, t.x, ty, line.c_str(), true, EpdFontFamily::BOLD);
        ty += jm.titleLine;
      }
    } else {
      const Shown& s = shown[0];
      const auto measure = [&](const std::string& str) {
        return renderer.getTextWidth(heroFont, str.c_str(), heroStyle);
      };
      const auto lines = journal::wrapTitle(s.book.title, t.width, L.heroTitleLines, measure);
      for (const auto& line : lines) {
        renderer.drawText(heroFont, t.x, ty, line.c_str(), true, heroStyle);
        ty += jm.titleLine;
      }
      if (!s.book.author.empty()) {
        const auto author = renderer.truncatedText(UI_10_FONT_ID, s.book.author.c_str(), t.width);
        renderer.drawText(UI_10_FONT_ID, t.x, ty, author.c_str());
        ty += jm.bodyLine;
      }
      hairline(renderer, t.x, ty + L.heroSeparatorGap, t.width * 2 / 3);

      // Reserved row at the bottom of the hero text: whole-book progress.
      if (L.heroProgressY >= 0) {
        char pct[8];
        RecentBookProgress::formatPercent(s.percent, pct, sizeof(pct));
        const int pctW = renderer.getTextWidth(UI_10_FONT_ID, pct, EpdFontFamily::BOLD);
        const int barW = std::max(24, t.width - pctW - 12);
        journalProgressBar(renderer, t.x, L.heroProgressY + (jm.bodyLine - 8) / 2, barW, 8, s.percent);
        renderer.drawText(UI_10_FONT_ID, t.x + barW + 12, L.heroProgressY, pct, true, EpdFontFamily::BOLD);
      }
    }
  }

  // Recent section: label with a rule running to the right edge, then up to two columns.
  renderer.drawText(UI_10_FONT_ID, L.recentLabel.x, L.recentLabel.y, recentLabel, true, EpdFontFamily::BOLD);
  {
    const int labelW = renderer.getTextWidth(UI_10_FONT_ID, recentLabel, EpdFontFamily::BOLD);
    const int ruleX = L.recentLabel.x + labelW + 12;
    hairline(renderer, ruleX, L.recentLabel.y + jm.labelLine / 2, L.recentLabel.x + L.recentLabel.width - ruleX);
  }
  if (!L.recentHasColumns) {
    renderer.drawText(UI_10_FONT_ID, L.recentColumns[0].x, L.recentColumns[0].y, tr(STR_NO_RECENT_BOOKS));
  } else {
    if (rows > 1) {
      renderer.drawLine(L.recentDividerX, L.recentColumns[0].y, L.recentDividerX,
                        L.recentColumns[0].y + L.recentColumns[0].height - 1, true);
    }
    for (int i = 0; i < rows && i < 2; i++) {
      const Shown& s = shown[i + 1];
      const Rect& col = L.recentColumns[i];
      const int coverW = L.recentCoverWidth;
      const int coverH = std::min(L.recentCoverHeight, col.height);
      journalCoverBox[i + 1] = {coverW, coverH};
      drawJournalCover(i + 1, col.x, col.y, coverW, coverH);
      const int tx = col.x + coverW + journal::kRecentTextGap;
      const int tw = col.x + col.width - tx;
      int ty = col.y;
      const auto measure = [&](const std::string& str) {
        return renderer.getTextWidth(UI_10_FONT_ID, str.c_str(), EpdFontFamily::BOLD);
      };
      const auto lines = journal::wrapTitle(s.book.title, tw, L.recentTitleLines, measure);
      for (const auto& line : lines) {
        renderer.drawText(UI_10_FONT_ID, tx, ty, line.c_str(), true, EpdFontFamily::BOLD);
        ty += jm.bodyLine;
      }
      if (!s.book.author.empty() && L.recentAuthorDy >= 0) {
        const auto author = renderer.truncatedText(SMALL_FONT_ID, s.book.author.c_str(), tw);
        renderer.drawText(SMALL_FONT_ID, tx, std::min(ty, col.y + L.recentAuthorDy), author.c_str());
      }
      char meta[32];
      char pct[8];
      RecentBookProgress::formatPercent(s.percent, pct, sizeof(pct));  // "" when unknown
      if (s.downloaded && pct[0]) {
        snprintf(meta, sizeof(meta), "%s · %s", pct, tr(STR_BO_DOWNLOADED));
      } else {
        snprintf(meta, sizeof(meta), "%s", s.downloaded ? tr(STR_BO_DOWNLOADED) : pct);
      }
      if (meta[0] && L.recentMetaDy >= 0) renderer.drawText(SMALL_FONT_ID, tx, col.y + L.recentMetaDy, meta);
      if (focus == i + 1) {
        JournalTheme::drawCropBrackets(renderer, col, journal::kBracketOutset, journal::kBracketLength,
                                       journal::kBracketStroke);
      }
    }
  }

  // Utility row: 書庫 / 搜尋 / 設定 between vertical rules, over a plain rule above the hints.
  hairline(renderer, contentX, L.navRuleY, contentW);
  for (int t = 0; t < TILE_COUNT; t++) {
    const Rect& cell = L.navCells[t];
    const bool focused = focus == tileFocus(static_cast<Tile>(t));
    if (t > 0) renderer.drawLine(cell.x, cell.y + 6, cell.x, cell.y + cell.height - 7, true);
    const auto style = focused ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const auto label = renderer.truncatedText(UI_10_FONT_ID, navLabels[t], cell.width - 16, style);
    const int lw = renderer.getTextWidth(UI_10_FONT_ID, label.c_str(), style);
    renderer.drawText(UI_10_FONT_ID, cell.x + (cell.width - lw) / 2, cell.y + journal::kNavPaddingY, label.c_str(),
                      true, style);
    if (focused) {
      JournalTheme::drawCropBrackets(renderer, Rect{cell.x + 8, cell.y + 2, cell.width - 16, cell.height - 4}, 0,
                                     journal::kBracketLength, journal::kBracketStroke);
    }
  }

  const auto hints =
      mappedInput.mapLabels(shown.empty() ? "" : tr(STR_RESUME), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);

  if (!firstRenderDone) {
    firstRenderDone = true;
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    LOG_DBG("BOHOME", "First paint done");
  } else {
    renderer.displayBuffer();
  }
  // After paint only: at most one missing cover per pass, so each repaint shows the next cover.
  if (loadNextJournalCover()) requestUpdate();
}

bool BookOrbitHomeActivity::loadNextJournalCover() {
  // Same rules as loadHeroCover(): metadata cache must already exist (never built here), EPUB only,
  // and one attempt per slot. generateThumbBmp(w, h) writes an empty placeholder on a decode
  // failure, so a bad cover is not retried on the next Home visit either.
  for (int slot = 0; slot < bookSlots() && slot < MAX_RECENT; slot++) {
    if (journalCoverTried[slot]) continue;
    const CoverBox box = journalCoverBox[slot];
    if (box.width <= 0 || box.height <= 0) continue;  // slot not laid out this pass
    journalCoverTried[slot] = true;
    const RecentBook& book = shown[slot].book;
    const std::string thumb = journalThumbPath(book, box.width, box.height);
    if (thumb.empty() || Storage.exists(thumb.c_str()) || !FsHelpers::hasEpubExtension(book.path)) continue;
    Epub epub(book.path, "/.crosspoint");
    if (!epub.load(/*buildIfMissing=*/false, /*skipLoadingCss=*/true)) continue;
    if (epub.generateThumbBmp(box.width, box.height)) return true;
  }
  return false;
}
