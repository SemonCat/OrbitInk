#include "BookOrbitCatalogBrowserActivity.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>

#include "components/UITheme.h"
#include "fontIds.h"

namespace {
using CatalogClient = BookOrbitCatalogClient;
constexpr const char* TABS[] = {"", "reading", "unread", "finished"};

const char* tabLabel(int tab) {
  const StrId ids[] = {StrId::STR_BO_ALL_TAB, StrId::STR_BO_READING, StrId::STR_BO_UNREAD, StrId::STR_BO_FINISHED};
  return I18N.get(ids[tab]);
}

const char* readingLabel(const std::string& status) {
  using State = BookOrbitCatalogBrowserPolicy::ReadState;
  switch (BookOrbitCatalogBrowserPolicy::readState(status)) {
    case State::Unread: return tr(STR_BO_UNREAD);
    case State::WantToRead: return tr(STR_BO_WANT);
    case State::Reading: return tr(STR_BO_READING);
    case State::OnHold: return tr(STR_BO_HOLD);
    case State::Finished: return tr(STR_BO_FINISHED);
    case State::Abandoned: return tr(STR_BO_ABANDONED);
    default: return tr(STR_BO_OTHER_STATE);
  }
}

// Both wrapping and rendering use the actual resident font and the same scale.
void text(GfxRenderer& r, int x, int y, const std::string& value, int height, int width, int lines = 1,
          bool bold = false, bool black = true, int lineStep = 0) {
  const auto style = bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  const auto wrapped = BookOrbitCatalogBrowserPolicy::wrapTitle(value, width, lines, [&](const std::string& s) {
    return r.getTextWidthSized(UI_10_FONT_ID, s.c_str(), height, style);
  });
  for (size_t i = 0; i < wrapped.size(); ++i)
    r.drawTextSized(UI_10_FONT_ID, x, y + static_cast<int>(i) * (lineStep ? lineStep : height),
                    wrapped[i].c_str(), height, black, style);
}

void outline(GfxRenderer& r, int x, int y, int width, int height, int thickness) {
  for (int i = 0; i < thickness; ++i) r.drawRect(x + i, y + i, width - 2 * i, height - 2 * i);
}
}  // namespace

bool BookOrbitCatalogBrowserActivity::isBookBrowse() const {
  return listKind == ListKind::BOOKS || listKind == ListKind::DOWNLOADED;
}

void BookOrbitCatalogBrowserActivity::openTools() {
  choiceIndex = 0;
  state = BrowserState::TOOLS;
  requestUpdate();
}

void BookOrbitCatalogBrowserActivity::changeTab(int tab) {
  if (tab < 0 || tab > 3) return;
  // Downloaded entries contain no authoritative reading state. Selecting a status
  // explicitly returns to the server catalog; never label local-only rows "All".
  if (listKind == ListKind::DOWNLOADED) {
    currentQuery = CatalogClient::BookQuery{};
    currentQuery.sort = "title";
    location = tr(STR_BO_ALL_BOOKS);
  }
  const auto previous = currentQuery;
  const auto previousKind = listKind;
  const int savedPage = currentPage, savedIndex = selectorIndex;
  currentQuery.readStatus = TABS[tab];
  paging = previousKind == ListKind::BOOKS;
  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdate(true);
  loadBooks(0);
  paging = false;
  if (!listNotice.empty()) { currentQuery = previous; currentPage = savedPage; selectorIndex = savedIndex; }
  requestUpdate();
}

void BookOrbitCatalogBrowserActivity::pageBrowse(int direction) {
  browseIdleAt = millis();
  browseFocus = -1;
  const int n = listPageItems();
  const int first = selectorIndex / n * n;
  const int target = first + direction * n;
  if (target >= 0 && target < static_cast<int>(itemCount())) {
    selectorIndex = target;
    requestUpdate();
    return;
  }
  if ((direction < 0 && currentPage == 0) || (direction > 0 && !currentPageHasNext)) {
    requestUpdate();
    return;
  }
  const int next = currentPage + direction;
  paging = true;
  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdate(true);
  if (listKind == ListKind::BOOKS) loadBooks(next);
  else loadDownloaded(next);
  paging = false;
  if (state == BrowserState::BROWSING && currentPage == next && direction < 0)
    selectorIndex = std::max(0, (static_cast<int>(itemCount()) - 1) / n * n);
  requestUpdate();
}

void BookOrbitCatalogBrowserActivity::fetchVisibleCover() {
  if (listKind != ListKind::BOOKS || offline || !coverCache || browseFocus >= 0 ||
      millis() - browseIdleAt < 350 || mappedInput.isPressed(MappedInputManager::Button::Back)) return;
  const size_t first = static_cast<size_t>(selectorIndex / listPageItems() * listPageItems());
  const size_t end = std::min(books.size(), first + listPageItems());
  for (size_t i = first; i < end && i < coverAttempts.size(); ++i) {
    if (coverAttempts[i] || !books[i].hasCover) continue;
    coverAttempts[i] = 1;
    if (coverCache->available(books[i].id)) continue;
    std::vector<std::string> visibleIds;
    for (size_t j = first; j < end; ++j) visibleIds.push_back(books[j].id);
    const bool fetched = coverCache->fetch(catalogSession(), books[i].id, [this] {
      gpio.update();
      return mappedInput.wasAnyPressed();
    }, visibleIds);
    if (fetched) requestUpdate();
    // Only one optional request per loop; input always gets another turn.
    return;
  }
}

void BookOrbitCatalogBrowserActivity::renderCover(const CatalogClient::BookSummary& book, int x, int y, int w, int h) {
  renderer.drawRect(x, y, w, h, 1);
  bool rendered = false;
  if (coverCache && coverCache->available(book.id)) {
    HalFile f;
    if (Storage.openFileForRead("BOCover", coverCache->pathFor(book.id), f)) {
      Bitmap bitmap(f);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        const float scale = std::min(static_cast<float>(w - 2) / bitmap.getWidth(),
                                     static_cast<float>(h - 2) / bitmap.getHeight());
        const int dw = std::min(w - 2, static_cast<int>(bitmap.getWidth() * std::min(1.0f, scale)));
        const int dh = std::min(h - 2, static_cast<int>(bitmap.getHeight() * std::min(1.0f, scale)));
        renderer.drawBitmap(bitmap, x + (w - dw) / 2, y + (h - dh) / 2, dw, dh);
        rendered = true;
      }
    }
  }
  if (!rendered) {
    renderer.drawLine(x + 5, y + 1, x + 5, y + h - 2);
    if (w > 60) {
      text(renderer, x + 11, y + 10, book.title, 20, w - 17, 3, true);
      text(renderer, x + 11, y + h - 35, book.authors, 16, w - 17);
    } else {
      text(renderer, x + 8, y + 10, book.title, 17, w - 12, 2);
    }
  }
}

void BookOrbitCatalogBrowserActivity::renderLibrary() {
  const bool localOnly = listKind == ListKind::DOWNLOADED;
  const bool reading = view == View::Covers && !localOnly && currentQuery.readStatus == "reading";
  const int n = listPageItems();
  const int first = selectorIndex / n * n;
  const int end = std::min(static_cast<int>(itemCount()), first + n);
  const uint32_t total = localOnly ? localBookCount : totalBooks;
  text(renderer, 20, 14, "BookOrbit", 26, 300, 1, true);
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawBatteryRight(renderer, Rect{508 - metrics.batteryWidth, 16, metrics.batteryWidth, metrics.batteryHeight}, true);
  text(renderer, 20, 51, localOnly ? tr(STR_BO_LOCAL) : tr(STR_BO_MY_LIBRARY), 39, 360, 1, true);
  char count[32];
  snprintf(count, sizeof(count), tr(STR_BO_BOOK_COUNT), total);
  text(renderer, 400, 66, count, 24, 108);
  int activeTab = 0;
  for (int i = 0; i < 4; ++i) if (currentQuery.readStatus == TABS[i]) activeTab = i;
  for (int i = 0; i < 4; ++i) {
    const int x = 20 + i * 124;
    const bool active = !localOnly && i == activeTab;
    if (active) renderer.fillRect(x, 107, 112, 38);
    if (browseFocus == i) outline(renderer, x - 3, 103, 118, 46, 2);
    text(renderer, x + 12, 114, tabLabel(i), 22, 100, 1, true, !active);
  }
  const char* sort = tr(STR_BO_TITLE);
  const StrId sorts[] = {StrId::STR_BO_TITLE, StrId::STR_BO_AUTHOR, StrId::STR_BO_SERIES,
                        StrId::STR_BO_RECENT, StrId::STR_BO_RECENT_READ, StrId::STR_BO_RECENT_UPDATE};
  const char* values[] = {"title", "author", "series", "recently_added", "recently_read", "recently_updated"};
  for (int i = 0; i < 6; ++i) if (currentQuery.sort == values[i]) sort = I18N.get(sorts[i]);
  std::string scope = localOnly ? tr(STR_BO_LOCAL) : std::string(sort) + " / " + tr(STR_BO_SORT);
  if (!currentQuery.searchText.empty() || !currentQuery.authorId.empty() || !currentQuery.seriesId.empty() ||
      !currentQuery.libraryId.empty() || !currentQuery.collectionId.empty() || !currentQuery.smartScopeId.empty())
    scope = location;
  text(renderer, 20, 161, scope, 20, 300);
  text(renderer, 330, 161, (view == View::Covers ? tr(STR_BO_COVER_RAIL) : tr(STR_BO_TITLE_RAIL)), 20, 178);
  if (browseFocus == 4) outline(renderer, 16, 154, 496, 35, 2);
  renderer.drawLine(20, 193, 508, 193);

  for (int i = first; i < end; ++i) {
    CatalogClient::BookSummary book;
    bool local;
    if (localOnly) {
      book.id = downloaded[i].bookId;
      book.title = downloaded[i].title;
      book.authors = downloaded[i].authors;
      local = true;
    } else {
      book = books[i];
      local = i < static_cast<int>(booksDownloaded.size()) && booksDownloaded[i];
    }
    const int slot = i - first;
    const bool selected = browseFocus == -1 && i == selectorIndex;
    std::string status = localOnly ? tr(STR_BO_DOWNLOADED) : readingLabel(book.readStatus);
    if (!localOnly && book.progressPercent >= 0 && currentQuery.readStatus != "finished")
      status += " " + std::to_string(static_cast<int>(book.progressPercent + 0.5f)) + "%";
    const char* availability = (local ? tr(STR_BO_OFFLINE_READY) : tr(STR_BO_ONLINE_ONLY));
    if (reading) {
      const int y = 215 + slot * 220;
      outline(renderer, 20, y, 488, 203, selected ? 4 : 1);
      renderCover(book, 34, y + 16, 112, 166);
      text(renderer, 166, y + 14, book.title, 28, 326, 2, true, true, 32);
      text(renderer, 166, y + 80, book.authors, 22, 326);
      text(renderer, 166, y + 115, availability, 20, 326);
      renderer.drawRect(166, y + 151, 309, 8);
      if (book.progressPercent > 0)
        renderer.fillRect(167, y + 152, static_cast<int>(307 * std::min(100.0f, book.progressPercent) / 100), 6);
      text(renderer, 166, y + 169, status, 20, 326);
    } else if (view == View::Titles) {
      const int y = 201 + slot * 80;
      if (selected) outline(renderer, 20, y, 488, 78, 3);
      else renderer.drawLine(20, y + 78, 508, y + 78);
      renderCover(book, 29, y + 6, 44, 65);
      text(renderer, 86, y + 3, book.title, 23, 410, 2, true, true, 27);
      text(renderer, 86, y + 56, book.authors, 17, 210);
      text(renderer, 310, y + 56, std::string(local ? tr(STR_BO_LOCAL_PREFIX) : tr(STR_BO_ONLINE)) + " / " + status,
           17, 188);
    } else {
      const int x = 20 + (slot % 2) * 250, y = 205 + (slot / 2) * 168;
      if (selected) outline(renderer, x, y, 238, 157, 4);
      renderCover(book, x + 10, y + 9, 85, 135);
      text(renderer, x + 105, y + 12, book.title, 22, 127, 3, true, true, 27);
      // Keep availability in the metadata column, clear of the closed focus border.
      text(renderer, x + 105, y + 96, book.authors, 17, 127);
      text(renderer, x + 105, y + 115, local ? tr(STR_BO_LOCAL_PREFIX) : tr(STR_BO_ONLINE), 14, 127);
      text(renderer, x + 105, y + 137, status, 16, 127);
    }
  }
  if (itemCount() == 0) {
    text(renderer, 40, 330, (localOnly ? tr(STR_BO_NO_DOWNLOADS) : tr(STR_BO_NO_MATCH)), 32, 448, 2);
    text(renderer, 40, 415, tr(STR_BO_CHANGE_FILTER), 24, 448, 2);
  }
  std::string notice = offline ? tr(STR_BO_OFFLINE) : listNotice;
  if (notice.empty()) {
    snprintf(count, sizeof(count), tr(STR_BO_LOCAL_COUNT), localBookCount);
    notice = count;
  }
  text(renderer, 20, 701, notice, 18, 285);
  const int offset = currentPage * CatalogClient::PAGE_SIZE;
  const std::string range = (itemCount() ? std::to_string(offset + first + 1) + "-" + std::to_string(offset + end) : "0") +
                            " / " + std::to_string(total);
  text(renderer, 345, 701, range, 18, 163);
  renderer.drawLine(20, 721, 508, 721);
  text(renderer, 20, 738, (browseFocus >= 0 ? tr(STR_BO_FOCUS_HINT) : tr(STR_BO_BROWSE_HINT)), 20, 488);
  text(renderer, 20, 770, tr(STR_BO_PAGE_FOCUS_HINT), 16, 488);
  renderer.displayBuffer();
}
