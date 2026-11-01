#include "BookOrbitCatalogBrowserActivity.h"

#include <Arduino.h>
#include <BookOrbitCredentialStore.h>
#include <CalendarTime.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <KOReaderDocumentId.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "components/themes/journal/JournalTheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"
#include "util/BookOrbitCatalogBrowserPolicy.h"
#include "util/BookOrbitStatsSync.h"
#include "util/InputReleaseGuard.h"

namespace {
constexpr int DOWNLOAD_PROGRESS_STEP_PERCENT = 5;
constexpr unsigned long DOWNLOAD_PROGRESS_MIN_UPDATE_MS = 5000;
// Synthetic root rows: never sent to the server (BookOrbit section ids never start with "__").
constexpr char ROOT_DOWNLOADED[] = "__downloaded";
constexpr char ROOT_SEARCH[] = "__search";

using BOClient = BookOrbitCatalogClient;

uint32_t nowEpochSeconds() {
  uint32_t minutes = 0;
  // Before 2024 the RTC is unset; record "unknown" (0) rather than a wrong date.
  if (!halClock.getEpochMinutes(minutes) || minutes < CalendarTime::epochMinutes(2024, 1, 1, 0, 0)) return 0;
  return minutes * 60u;
}

std::string formatSavedDate(const uint32_t epochSeconds) {
  if (epochSeconds == 0) return "-";
  int year = 0;
  unsigned month = 0;
  unsigned day = 0;
  CalendarTime::civilFromDays(static_cast<int64_t>(epochSeconds) / 86400, year, month, day);
  char buf[16];
  snprintf(buf, sizeof(buf), "%04d/%02u/%02u", year, month, day);
  return buf;
}

std::string formatSize(const uint64_t bytes) {
  char buf[24];
  if (bytes >= 1024u * 1024u) {
    snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else {
    snprintf(buf, sizeof(buf), "%u KB", static_cast<unsigned>((bytes + 1023) / 1024));
  }
  return buf;
}

bool wifiConnected() { return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0); }

// Transport-level failure: the request never got an HTTP answer. Only these fall back to the
// offline cache; HTTP/auth errors are reported as such, never masked by stale data.
bool isTransportFailure(const BOClient::Error error) { return error == BOClient::NETWORK_ERROR; }
const char* sectionLabel(const std::string& id) {
  if (id == ROOT_SEARCH) return tr(STR_BO_FIND);
  if (id == ROOT_DOWNLOADED) return tr(STR_BO_LOCAL);
  if (id == "continue-reading") return tr(STR_BO_CONTINUE);
  if (id == "all-books") return tr(STR_BO_ALL_BOOKS);
  if (id == "recent") return tr(STR_BO_RECENT);
  if (id == "authors") return tr(STR_BO_AUTHORS);
  if (id == "series") return tr(STR_BO_SERIES);
  if (id == "libraries") return tr(STR_BO_LIBRARIES);
  if (id == "collections") return tr(STR_BO_COLLECTIONS);
  if (id == "smart-scopes") return tr(STR_BO_SMART);
  return nullptr;
}
constexpr const char* SORTS[] = {"title", "author", "series", "recently_added", "recently_read", "recently_updated"};
constexpr const char* FILTERS[] = {"", "unread", "reading", "finished"};
const char* sortLabel(int i) {
  const StrId ids[] = {StrId::STR_BO_TITLE, StrId::STR_BO_AUTHOR, StrId::STR_BO_SERIES, StrId::STR_BO_RECENT, StrId::STR_BO_RECENT_READ,
                       StrId::STR_BO_RECENT_UPDATE};
  return I18N.get(ids[i]);
}
const char* filterLabel(int i) {
  const StrId ids[] = {StrId::STR_BO_ALL_STATES, StrId::STR_BO_UNREAD, StrId::STR_BO_READING, StrId::STR_BO_FINISHED};
  return I18N.get(ids[i]);
}
std::string sortFilterLabel(const BOClient::BookQuery& query) {
  int sort = 0, filter = 0;
  for (int i = 0; i < 6; ++i) if (query.sort == SORTS[i]) sort = i;
  for (int i = 0; i < 4; ++i) if (query.readStatus == FILTERS[i]) filter = i;
  return std::string(tr(STR_BO_SORT)) + ": " + sortLabel(sort) + " / " + filterLabel(filter);
}
const char* statusLabel(const std::string& status) {
  using State = BookOrbitCatalogBrowserPolicy::ReadState;
  switch (BookOrbitCatalogBrowserPolicy::readState(status)) {
    case State::Unread: return tr(STR_BO_UNREAD);
    case State::WantToRead: return tr(STR_BO_WANT);
    case State::Reading: return tr(STR_BO_READING);
    case State::OnHold: return tr(STR_BO_HOLD);
    case State::Finished: return tr(STR_BO_FINISHED);
    case State::Abandoned: return tr(STR_BO_ABANDONED);
    case State::Other: return tr(STR_BO_OTHER_STATE);
  }
  return tr(STR_BO_OTHER_STATE);
}
std::string statusValue(const std::string& status, float progress) {
  const auto value = BookOrbitCatalogBrowserPolicy::readState(status);
  using State = BookOrbitCatalogBrowserPolicy::ReadState;
  if ((value == State::Reading || value == State::Other || status.empty()) && progress >= 0) {
    return std::to_string(static_cast<int>(progress + 0.5f)) + "%";
  }
  return value == State::Unread ? "" : statusLabel(status);
}
std::string seriesText(const std::string& name, const std::string& index) {
  return name + (name.empty() || index.empty() ? "" : " #" + index);
}
const char* localizedError(BOClient::Error error) {
  switch (error) {
    case BOClient::NO_CREDENTIALS:
    case BOClient::AUTH_FAILED: return tr(STR_BO_AUTH_ERROR);
    case BOClient::NETWORK_ERROR: return tr(STR_BO_CONNECT_ERROR);
    case BOClient::SERVER_ERROR: return tr(STR_BO_SERVER_ERROR);
    case BOClient::BAD_REQUEST: return tr(STR_BO_REJECT_ERROR);
    case BOClient::JSON_ERROR: return tr(STR_BO_DATA_ERROR);
    case BOClient::LOW_MEMORY: return tr(STR_BO_MEMORY_ERROR);
    case BOClient::UNSUPPORTED: return tr(STR_BO_UNSUPPORTED_ERROR);
    case BOClient::FILE_ERROR: return tr(STR_BO_FILE_ERROR);
    case BOClient::ABORTED: return tr(STR_BO_CANCEL_ERROR);
    case BOClient::OK: return "";
  }
  return tr(STR_BO_SERVER_ERROR);
}
}  // namespace

void BookOrbitCatalogBrowserActivity::onEnter() {
  Activity::onEnter();

  state = BrowserState::CHECK_WIFI;
  listKind = ListKind::ROOT;
  facetKind = FacetKind::NONE;
  sections.clear();
  books.clear();
  booksDownloaded.clear();
  facets.clear();
  downloaded.clear();
  catalogSessionCache.clear();
  navigationHistory.clear();
  currentQuery = BOClient::BookQuery{};
  currentPage = 0;
  currentPageHasNext = false;
  selectorIndex = 0;
  totalBooks = 0;
  location = tr(STR_BO_LIBRARY);
  listNotice.clear();
  paging = detailChanged = false;
  consumeConfirm = false;
  consumeBack = false;
  backReleasePending = true;
  errorMessage.clear();
  offline = false;
  offlineSavedDate.clear();
  pendingSearch = entryMode == EntryMode::HomeSearch;
  exitTarget = ExitTarget::Caller;
  exitReaderPath.clear();

  const BookOrbitOwnerId owner = BookOrbitStats::currentOwner();
  if (!owner.isZero()) {
    library = makeUniqueNoThrow<BookOrbitLibrary::Store>(owner, bookOrbitFirmwareSha256());
    catalogCache = makeUniqueNoThrow<BookOrbitCatalogCache::Cache>(owner, bookOrbitFirmwareSha256());
    if (!library || !catalogCache) LOG_ERR("BOCatalog", "OOM: library stores");
  }
  statusMessage = tr(STR_CHECKING_WIFI);
  requestUpdate();

  checkAndConnectWifi();
}

void BookOrbitCatalogBrowserActivity::onExit() {
  Activity::onExit();
  catalogSessionCache.clear();
  std::vector<BOClient::Section>().swap(sections);
  std::vector<BOClient::BookSummary>().swap(books);
  std::vector<uint8_t>().swap(booksDownloaded);
  std::vector<BOClient::FacetEntry>().swap(facets);
  std::vector<BookOrbitLibrary::Entry>().swap(downloaded);
  navigationHistory.clear();
  library.reset();
  catalogCache.reset();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    // The heap-defrag reboot still happens, but it lands where the user was going
    // (docs/bookorbit-home-library.md §9 finding 1). silentRestart*() are no-ops while a deep
    // sleep is in progress, in which case the normal ActivityManager destination stands.
    switch (exitTarget) {
      case ExitTarget::Reader:
        APP_STATE.openEpubPath = exitReaderPath;
        APP_STATE.saveToFile();
        silentRestartToReaderWithSync();
        break;
      case ExitTarget::HomeLibrary:
        silentRestartToHomeLibrary();
        break;
      case ExitTarget::Caller:
        silentRestart();
        break;
    }
  }
}

size_t BookOrbitCatalogBrowserActivity::itemCount() const {
  switch (listKind) {
    case ListKind::ROOT:
      return sections.size();
    case ListKind::BOOKS:
      return books.size();
    case ListKind::FACETS:
      return facets.size();
    case ListKind::DOWNLOADED:
      return downloaded.size();
  }
  return 0;
}

size_t BookOrbitCatalogBrowserActivity::displayCount() const {
  return itemCount() + (listKind == ListKind::BOOKS ? 1 : 0);
}

size_t BookOrbitCatalogBrowserActivity::toItemIndex(const size_t displayIndex) const {
  return listKind == ListKind::BOOKS ? displayIndex - 1 : displayIndex;
}

int BookOrbitCatalogBrowserActivity::listTop() const {
  const auto& m = UITheme::getInstance().getMetrics();
  return m.topPadding + m.headerHeight + m.verticalSpacing + renderer.getLineHeight(SMALL_FONT_ID) * 2 + 12;
}

int BookOrbitCatalogBrowserActivity::rowHeight() const {
  const bool twoLine = listKind == ListKind::BOOKS || listKind == ListKind::DOWNLOADED;
  const auto& m = UITheme::getInstance().getMetrics();
  return std::max(twoLine ? m.listWithSubtitleRowHeight : m.listRowHeight,
                  renderer.getLineHeight(UI_10_FONT_ID) + (twoLine ? renderer.getLineHeight(SMALL_FONT_ID) : 0) + 14);
}

int BookOrbitCatalogBrowserActivity::listPageItems() const {
  const auto& m = UITheme::getInstance().getMetrics();
  return std::max(1, (renderer.getScreenHeight() - listTop() - m.buttonHintsHeight - m.verticalSpacing) / rowHeight());
}

void BookOrbitCatalogBrowserActivity::moveSelection(int delta) {
  const auto move = BookOrbitCatalogBrowserPolicy::moveInList(currentPage, selectorIndex,
      static_cast<int>(displayCount()), currentPageHasNext, delta);
  if (!move.fetch || listKind == ListKind::ROOT) {
    selectorIndex = move.index;
    requestUpdate();
    return;
  }
  paging = true;
  listNotice.clear();
  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdate(true);
  switch (listKind) {
    case ListKind::BOOKS: loadBooks(move.page); break;
    case ListKind::FACETS: loadFacets(move.page); break;
    case ListKind::DOWNLOADED: loadDownloaded(move.page); break;
    case ListKind::ROOT: break;
  }
  paging = false;
  if (state == BrowserState::BROWSING && currentPage == move.page) {
    selectorIndex = move.index < 0 ? std::max(0, static_cast<int>(displayCount()) - 1) : move.index;
  }
  requestUpdate();
}

void BookOrbitCatalogBrowserActivity::loop() {
  if (state == BrowserState::WIFI_SELECTION || state == BrowserState::SEARCH_INPUT) return;
  if (InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Back, backReleasePending)) {
    return;
  }

  if (consumeConfirm && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    consumeConfirm = false;
    return;
  }
  if (consumeBack && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    consumeBack = false;
    return;
  }

  if (state == BrowserState::OPTIONS || state == BrowserState::READ_STATUS || state == BrowserState::RATING) {
    const int count = state == BrowserState::OPTIONS ? 11 : state == BrowserState::READ_STATUS ? 6 : 6;
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = state == BrowserState::OPTIONS ? BrowserState::BROWSING : BrowserState::DETAIL;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      applyChoice();
    } else {
      buttonNavigator.onNextRelease([this, count] { choiceIndex = std::min(choiceIndex + 1, count - 1); requestUpdate(); });
      buttonNavigator.onPreviousRelease([this] { choiceIndex = std::max(0, choiceIndex - 1); requestUpdate(); });
    }
    return;
  }

  if (state == BrowserState::ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (retryDetail && wifiConnected()) {
        const auto book = retryBook;
        openBookDetail(book);
        return;
      }
      if (wifiConnected()) {
        // An explicit Retry with the link up always tries the server again: a transport failure
        // earlier in the session must not pin the library offline (final review r1, finding 1).
        offline = false;
        state = BrowserState::LOADING;
        statusMessage = tr(STR_LOADING);
        requestUpdate();
        switch (listKind) {
          case ListKind::ROOT:
            loadRoot();
            break;
          case ListKind::BOOKS:
            loadBooks(currentPage);
            break;
          case ListKind::FACETS:
            loadFacets(currentPage);
            break;
          case ListKind::DOWNLOADED:
            loadDownloaded(currentPage);
            break;
        }
      } else {
        launchWifiSelection();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      if (retryDetail) {
        retryDetail = false;
        state = BrowserState::BROWSING;
        requestUpdate();
      } else {
        navigateBack();
      }
    }
    return;
  }

  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state == BrowserState::CHECK_WIFI ? goBackOrHome() : navigateBack();
    }
    return;
  }

  if (state == BrowserState::DOWNLOADING) return;  // Back is polled inside the transfer

  if (state == BrowserState::DETAIL) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (detailSelectorIndex >= 0 && detailSelectorIndex < static_cast<int>(detailRows.size())) {
        const DetailRow row = detailRows[detailSelectorIndex];
        switch (row.kind) {
          case DetailRowKind::OPEN_LOCAL:
            openLocalBook(detailLocalPath);
            return;
          case DetailRowKind::DOWNLOAD:
            downloadFile(row.fileIndex);
            return;
          case DetailRowKind::READ_STATUS:
            choiceIndex = 0;
            for (int i = 0; i < 6; ++i) {
              if (BookOrbitCatalogBrowserPolicy::readState(detail.readStatus) ==
                  BookOrbitCatalogBrowserPolicy::readState(BOClient::SETTABLE_READ_STATUSES[i])) choiceIndex = i;
            }
            state = BrowserState::READ_STATUS;
            requestUpdate();
            return;
          case DetailRowKind::RATING:
            choiceIndex = std::max(0, detail.rating);
            state = BrowserState::RATING;
            requestUpdate();
            return;
          case DetailRowKind::OFFLINE_UNAVAILABLE:
            return;
        }
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnFromDetail();
    } else {
      const int rowCount = static_cast<int>(detailRows.size());
      if (rowCount > 0) {
        buttonNavigator.onNextRelease([this, rowCount] {
          detailSelectorIndex = ButtonNavigator::nextIndex(detailSelectorIndex, rowCount);
          requestUpdate();
        });
        buttonNavigator.onPreviousRelease([this, rowCount] {
          detailSelectorIndex = ButtonNavigator::previousIndex(detailSelectorIndex, rowCount);
          requestUpdate();
        });
      }
    }
    return;
  }

  if (state == BrowserState::BROWSING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activateSelected();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
      return;
    }
    if (displayCount() > 0) {
      buttonNavigator.onNextRelease([this] { moveSelection(1); });
      buttonNavigator.onPreviousRelease([this] { moveSelection(-1); });
      buttonNavigator.onNextContinuous([this] { moveSelection(listPageItems()); });
      buttonNavigator.onPreviousContinuous([this] { moveSelection(-listPageItems()); });
    }
  }
}

namespace {
std::string facetRowTitle(const BOClient::FacetEntry& facet) {
  return facet.title;
}
std::string ratingDisplayText(int rating) {
  return rating < 0 ? tr(STR_BO_NO_RATING) : std::to_string(rating) + "/5";
}

}  // namespace

void BookOrbitCatalogBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const bool choices = state == BrowserState::OPTIONS || state == BrowserState::READ_STATUS || state == BrowserState::RATING;
  const char* title = state == BrowserState::OPTIONS ? tr(STR_BO_OPTIONS) :
                      state == BrowserState::READ_STATUS ? tr(STR_BOOKORBIT_READ_STATUS) :
                      state == BrowserState::RATING ? tr(STR_BOOKORBIT_RATING) : tr(STR_BO_LIBRARY);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title);
  const int x = metrics.contentSidePadding;
  const int width = pageWidth - 2 * x;
  int contextY = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  std::string path = location;
  // Remove complete leading UTF-8 characters until the tail fits, preserving the current level.
  while (!path.empty() && renderer.getTextWidth(SMALL_FONT_ID, path.c_str()) > width) {
    size_t n = 1;
    while (n < path.size() && (static_cast<unsigned char>(path[n]) & 0xc0) == 0x80) ++n;
    path.erase(0, n);
  }
  renderer.drawText(SMALL_FONT_ID, x, contextY, path.c_str());
  contextY += renderer.getLineHeight(SMALL_FONT_ID);
  std::string position;
  if (offline) {
    position = tr(STR_BO_OFFLINE);
    if (!offlineSavedDate.empty()) {
      char buf[80];
      snprintf(buf, sizeof(buf), tr(STR_BO_OFFLINE_SAVED), offlineSavedDate.c_str());
      position = buf;
    }
  } else if (!listNotice.empty()) {
    position = listNotice;
  } else if (state == BrowserState::BROWSING && listKind != ListKind::ROOT) {
    const int first = (selectorIndex / listPageItems()) * listPageItems();
    const int offset = listKind == ListKind::BOOKS ? 1 : 0;
    const int pageSize = listKind == ListKind::BOOKS ? BOClient::PAGE_SIZE :
                         listKind == ListKind::FACETS ? BOClient::MAX_FACET_PAGE_ITEMS : listPageItems();
    const int start = currentPage * pageSize + std::max(first - offset, 0) + 1;
    const int end = currentPage * pageSize + std::max(0, std::min(first + listPageItems(), static_cast<int>(displayCount())) - offset);
    position = itemCount() == 0 ? "0" : std::to_string(start) + "-" + std::to_string(end);
    if (totalBooks && listKind == ListKind::BOOKS) position += " / " + std::to_string(totalBooks);
    else position += "  < " + std::to_string(currentPage + 1) + (currentPageHasNext ? " >" : "");
  } else if (listKind == ListKind::ROOT) {
    position = "BookOrbit";
  }
  auto info = renderer.truncatedText(SMALL_FONT_ID, position.c_str(), width);
  renderer.drawText(SMALL_FONT_ID, x, contextY, info.c_str());
  renderer.drawLine(x, listTop() - 7, pageWidth - x, listTop() - 7);

  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING || state == BrowserState::ERROR ||
      state == BrowserState::DOWNLOADING) {
    const char* message = state == BrowserState::ERROR ? errorMessage.c_str() :
                          state == BrowserState::DOWNLOADING ? tr(STR_BO_DOWNLOADING) : statusMessage.c_str();
    UITheme::drawCenteredWrappedText(renderer, Rect{x, pageHeight / 2 - 40, width, 120}, UI_10_FONT_ID, message,
                                     3, true, EpdFontFamily::REGULAR, UITheme::TextVerticalAlignment::TOP);
    if (state == BrowserState::DOWNLOADING && downloadTotal > 0) {
      GUI.drawProgressBar(renderer, Rect{50, pageHeight / 2 + 100, pageWidth - 100, 20}, downloadProgress, downloadTotal);
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), state == BrowserState::ERROR ? tr(STR_RETRY) : "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
  if (state == BrowserState::DETAIL) { renderDetail(pageWidth, pageHeight); return; }

  if (choices) {
    const int count = state == BrowserState::OPTIONS ? 11 : 6;
    const auto label = [this](int i) -> std::string {
      if (state == BrowserState::OPTIONS) {
        if (i < 6) return std::string(tr(STR_BO_SORT)) + ": " + sortLabel(i);
        if (i < 10) return std::string(tr(STR_BO_FILTER)) + ": " + filterLabel(i - 6);
        return tr(STR_BO_SEARCH_LIST);
      }
      if (state == BrowserState::READ_STATUS) return statusLabel(BOClient::SETTABLE_READ_STATUSES[i]);
      return i == 0 ? tr(STR_BO_NO_RATING) : std::to_string(i) + "/5";
    };
    const auto value = [this](int i) -> std::string {
      bool current = false;
      if (state == BrowserState::OPTIONS) {
        current = i < 6 ? (currentQuery.sort.empty() ? i == 0 : currentQuery.sort == SORTS[i]) :
                  i < 10 && currentQuery.readStatus == FILTERS[i - 6];
      } else if (state == BrowserState::READ_STATUS) {
        current = BookOrbitCatalogBrowserPolicy::readState(detail.readStatus) ==
                  BookOrbitCatalogBrowserPolicy::readState(BOClient::SETTABLE_READ_STATUSES[i]);
      } else current = i == std::max(0, detail.rating);
      return current ? tr(STR_BO_CURRENT) : "";
    };
    GUI.drawList(renderer, Rect{0, listTop(), pageWidth, pageHeight - listTop() - metrics.buttonHintsHeight - 10},
                 count, choiceIndex, label, nullptr, nullptr, value);
  } else {
    const int count = static_cast<int>(displayCount());
    if (itemCount() == 0) {
      const char* empty = listKind == ListKind::DOWNLOADED ? tr(STR_BO_NO_DOWNLOADS) :
                          (!currentQuery.searchText.empty() || !currentQuery.readStatus.empty()) ? tr(STR_BO_NO_MATCH) :
                          tr(STR_BO_EMPTY_LIST);
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 30, empty);
      if (!currentQuery.searchText.empty() || !currentQuery.readStatus.empty()) {
        const auto hint = renderer.truncatedText(SMALL_FONT_ID, tr(STR_BO_CHANGE_FILTER), width);
        renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 75, hint.c_str());
      }
    }
    const bool twoLine = listKind == ListKind::BOOKS || listKind == ListKind::DOWNLOADED;
    const int height = rowHeight();
    const int first = selectorIndex / listPageItems() * listPageItems();
    for (int i = first; i < count && i < first + listPageItems(); ++i) {
      const int y = listTop() + (i - first) * height;
      const bool selected = i == selectorIndex;
      const bool journal = SETTINGS.uiTheme == CrossPointSettings::JOURNAL;
      if (selected) {
        if (journal) JournalTheme::drawCropBrackets(renderer, Rect{x - 4, y, width + 8, height - 4}, 0, 10, 2);
        else renderer.fillRect(0, y - 2, pageWidth, height);
      }
      const bool black = journal || !selected;
      std::string label, sub, value;
      if (listKind == ListKind::BOOKS && i == 0) {
        label = sortFilterLabel(currentQuery);
        sub = currentQuery.searchText.empty() ? tr(STR_BO_SEARCH_LIST) :
              std::string(tr(STR_SEARCH)) + ": " + currentQuery.searchText;
      } else {
        const size_t item = toItemIndex(i);
        if (listKind == ListKind::ROOT) {
          label = sections[item].title;
          if (sections[item].id == ROOT_DOWNLOADED) {
            char buf[32]; snprintf(buf, sizeof(buf), tr(STR_BO_BOOK_COUNT), localBookCount); value = buf;
          }
        }
        else if (listKind == ListKind::FACETS) {
          label = facetRowTitle(facets[item]);
          if (facets[item].hasBookCount) {
            char buf[32]; snprintf(buf, sizeof(buf), tr(STR_BO_BOOK_COUNT), facets[item].bookCount); value = buf;
          }
        } else if (listKind == ListKind::DOWNLOADED) {
          label = downloaded[item].title; sub = downloaded[item].authors; value = tr(STR_BO_LOCAL_PREFIX);
        } else {
          const auto& book = books[item];
          label = book.title;
          sub = item < booksDownloaded.size() && booksDownloaded[item] ? std::string(tr(STR_BO_LOCAL_PREFIX)) + " / " : "";
          sub += book.authors;
          if (!book.seriesName.empty()) sub += " / " + seriesText(book.seriesName, book.seriesIndex);
          value = statusValue(book.readStatus, book.progressPercent);
        }
      }
      // Status occupies the second line so long titles retain the full screen width.
      const int valueWidth = value.empty() ? 0 : renderer.getTextWidth(twoLine ? SMALL_FONT_ID : UI_10_FONT_ID, value.c_str()) + 12;
      const auto text = renderer.truncatedText(UI_10_FONT_ID, label.c_str(), width - (twoLine ? 0 : valueWidth));
      renderer.drawText(UI_10_FONT_ID, x, y, text.c_str(), black);
      if (twoLine) {
        const auto textSub = renderer.truncatedText(SMALL_FONT_ID, sub.c_str(), std::max(0, width - valueWidth));
        renderer.drawText(SMALL_FONT_ID, x, y + renderer.getLineHeight(UI_10_FONT_ID) + 2, textSub.c_str(), black);
      }
      if (!value.empty()) renderer.drawText(twoLine ? SMALL_FONT_ID : UI_10_FONT_ID, pageWidth - x - valueWidth + 12,
          y + (twoLine ? renderer.getLineHeight(UI_10_FONT_ID) + 2 : 0), value.c_str(), black);
      if (journal && i + 1 < count && i + 1 < first + listPageItems())
        renderer.drawLine(x, y + height - 1, pageWidth - x, y + height - 1);
    }
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), displayCount() > 0 || choices ? tr(STR_OPEN) : "",
                                           tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void BookOrbitCatalogBrowserActivity::renderDetail(const int pageWidth, const int pageHeight) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int x = metrics.contentSidePadding;
  const int width = pageWidth - 2 * metrics.contentSidePadding;
  int y = listTop();

  const int titleLine = renderer.getLineHeight(UI_12_FONT_ID);
  const auto titleLines = renderer.wrappedText(UI_12_FONT_ID, detail.title.c_str(), width, 3, EpdFontFamily::BOLD);
  for (const auto& line : titleLines) {
    renderer.drawText(UI_12_FONT_ID, x, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += titleLine;
  }
  const int bodyLine = renderer.getLineHeight(UI_10_FONT_ID);
  if (!detail.authors.empty()) {
    const auto authors = renderer.truncatedText(UI_10_FONT_ID, detail.authors.c_str(), width);
    renderer.drawText(UI_10_FONT_ID, x, y, authors.c_str(), true);
    y += bodyLine;
  }
  if (!detail.seriesName.empty()) {
    const auto series = renderer.truncatedText(UI_10_FONT_ID, seriesText(detail.seriesName, detail.seriesIndex).c_str(), width);
    renderer.drawText(UI_10_FONT_ID, x, y, series.c_str());
    y += bodyLine;
  }
  std::string reading = statusLabel(detail.readStatus);
  if (detail.progressPercent >= 0) reading += " / " + std::to_string(static_cast<int>(detail.progressPercent + 0.5f)) + "%";
  renderer.drawText(UI_10_FONT_ID, x, y, reading.c_str());
  y += bodyLine;
  // One state line: local copy, or the size of the first downloadable file, or why nothing can be done.
  std::string stateLine;
  if (!detailLocalPath.empty()) {
    stateLine = std::string("EPUB / ") + tr(STR_BO_DOWNLOADED);
  } else if (!detail.files.empty() && detail.files[0].sizeBytes > 0) {
    stateLine = "EPUB / " + formatSize(detail.files[0].sizeBytes);
  } else if (detail.files.empty() && !offline) {
    stateLine = tr(STR_BOOKORBIT_NO_COMPATIBLE_FILES);
  }
  if (!stateLine.empty()) {
    renderer.drawText(UI_10_FONT_ID, x, y, stateLine.c_str(), true);
    y += bodyLine;
  }
  if (!detailError.empty()) {
    const auto err = renderer.truncatedText(UI_10_FONT_ID, detailError.c_str(), width);
    renderer.drawText(UI_10_FONT_ID, x, y, err.c_str(), true);
    y += bodyLine;
  }
  y += metrics.verticalSpacing;

  const auto rowLabel = [this](const int i) -> std::string {
    const DetailRow& row = detailRows[i];
    switch (row.kind) {
      case DetailRowKind::OPEN_LOCAL:
        return tr(STR_BO_OPEN_BOOK);
      case DetailRowKind::DOWNLOAD: {
        std::string label = detailError.empty() ? tr(STR_DOWNLOAD) : tr(STR_BO_RETRY_DOWNLOAD);
        const auto& file = detail.files[row.fileIndex];
        if (detail.files.size() > 1) label += " #" + std::to_string(row.fileIndex + 1);
        if (file.sizeBytes > 0) label += " (" + formatSize(file.sizeBytes) + ")";
        return label;
      }
      case DetailRowKind::OFFLINE_UNAVAILABLE:
        return tr(STR_BO_DOWNLOAD_WHEN_ONLINE);
      case DetailRowKind::READ_STATUS:
        return std::string(tr(STR_BOOKORBIT_READ_STATUS)) + ": " + statusLabel(detail.readStatus);
      case DetailRowKind::RATING:
        return std::string(tr(STR_BOOKORBIT_RATING)) + ": " + ratingDisplayText(detail.rating);
    }
    return {};
  };
  const auto rowDimmed = [this](const int i) { return detailRows[i].kind == DetailRowKind::OFFLINE_UNAVAILABLE; };
  const int listHeight = pageHeight - y - metrics.buttonHintsHeight - metrics.verticalSpacing;
  GUI.drawList(renderer, Rect{0, y, pageWidth, std::max(listHeight, metrics.listRowHeight)},
               static_cast<int>(detailRows.size()), detailSelectorIndex, rowLabel, nullptr, nullptr, nullptr, false,
               rowDimmed);

  const char* confirm = "";
  if (detailSelectorIndex >= 0 && detailSelectorIndex < static_cast<int>(detailRows.size())) {
    switch (detailRows[detailSelectorIndex].kind) {
      case DetailRowKind::OPEN_LOCAL:
        confirm = tr(STR_OPEN);
        break;
      case DetailRowKind::DOWNLOAD:
        confirm = tr(STR_DOWNLOAD);
        break;
      case DetailRowKind::READ_STATUS:
      case DetailRowKind::RATING:
        confirm = tr(STR_OPEN);
        break;
      case DetailRowKind::OFFLINE_UNAVAILABLE:
        break;
    }
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirm, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
void BookOrbitCatalogBrowserActivity::reportSessionError(const BOClient::Error error) {
  if (paging) {
    state = BrowserState::BROWSING;
    listNotice = localizedError(error);
  } else {
    state = BrowserState::ERROR;
    errorMessage = localizedError(error);
  }
  requestUpdate();
}

void BookOrbitCatalogBrowserActivity::prepareCatalogHeap(void* activity) {
  // Every catalog screen renders through the SD CJK UI font, whose page arena (and the reader's,
  // if the library was opened from a book) stays resident after the render. KOSyncRequest's TLS
  // gate needs a 20 KB contiguous block, so measure it with that rebuildable arena dropped, as the
  // sync and OTA clients do. The RenderLock waits out a render the status update above may have
  // started (requestUpdate(true) notifies the render task immediately) and keeps the next one from
  // starting until the release is done, so no glyph pointer is freed under an in-flight draw.
  auto* self = static_cast<BookOrbitCatalogBrowserActivity*>(activity);
  RenderLock lock;
  if (auto* fcm = self->renderer.getFontCacheManager()) fcm->forceReleaseTransientCaches();
}

BOClient::Session& BookOrbitCatalogBrowserActivity::catalogSession() {
  // The full configured base URL is a conservative trust scope: repeated catalog paths on that
  // server retain SecureHttpClient's keep-alive connection, while any settings change destroys
  // the old transport before reuse.
  return *catalogSessionCache.get(BOOKORBIT_STORE.getBaseUrl(), [this] {
    auto session = std::make_unique<BOClient::Session>();
    session->setPrepareHeap(&BookOrbitCatalogBrowserActivity::prepareCatalogHeap, this);
    return session;
  });
}

void BookOrbitCatalogBrowserActivity::retryNetworkIfLinkUp() {
  // Offline after a TRANSPORT failure is not sticky (final review r1, finding 1): every load of the
  // root, a list or a detail page tries the server again while the WiFi link is still up, and only
  // falls back to the offline cache if that attempt fails too. A device with no link stays offline.
  if (offline && wifiConnected()) {
    offline = false;
    offlineSavedDate.clear();
  }
}

void BookOrbitCatalogBrowserActivity::enterOffline() {
  offline = true;
  catalogSessionCache.clear();
  loadRoot();  // offline branch: cached root (if any) + 已下載
}

void BookOrbitCatalogBrowserActivity::loadRoot() {
  std::vector<BOClient::Section> result;
  offlineSavedDate.clear();
  retryNetworkIfLinkUp();
  if (!offline) {
    const BOClient::Error error = catalogSession().fetchRootSections(result);
    if (error == BOClient::OK) {
      if (catalogCache) catalogCache->saveRoot(result, nowEpochSeconds());
    } else if (isTransportFailure(error)) {
      offline = true;
      catalogSessionCache.clear();
    } else {
      reportSessionError(error);
      return;
    }
  }
  if (offline) {
    result.clear();
    BookOrbitCatalogCache::Meta meta;
    if (catalogCache && catalogCache->loadRoot(result, meta)) offlineSavedDate = formatSavedDate(meta.savedAt);
  }
  std::vector<BOClient::Section> rows;
  rows.reserve(result.size() + 2);
  if (!offline) rows.push_back({ROOT_SEARCH, tr(STR_BO_FIND)});
  auto addSection = [&](const char* id) {
    for (auto& section : result) if (section.id == id) rows.push_back({id, sectionLabel(id)});
  };
  addSection("continue-reading");
  if (library) rows.push_back({ROOT_DOWNLOADED, tr(STR_BO_LOCAL)});
  for (const char* id : {"all-books", "recent", "authors", "series", "libraries", "collections", "smart-scopes"}) addSection(id);
  for (auto& section : result) if (!sectionLabel(section.id)) rows.push_back(std::move(section));
  location = tr(STR_BO_LIBRARY);
  localBookCount = library ? library->count() : 0;
  listNotice.clear();
  totalBooks = 0;

  listKind = ListKind::ROOT;
  facetKind = FacetKind::NONE;
  sections = std::move(rows);
  std::vector<BOClient::BookSummary>().swap(books);
  std::vector<uint8_t>().swap(booksDownloaded);
  std::vector<BOClient::FacetEntry>().swap(facets);
  std::vector<BookOrbitLibrary::Entry>().swap(downloaded);
  currentPage = 0;
  currentPageHasNext = false;
  selectorIndex = 0;
  state = BrowserState::BROWSING;
  requestUpdate();
  if (pendingSearch && !offline) {
    pendingSearch = false;
    launchSearch();
  }
}

void BookOrbitCatalogBrowserActivity::markDownloadedBooks() {
  booksDownloaded.assign(books.size(), 0);
  if (!library || books.empty()) return;
  std::vector<std::string> ids;
  ids.reserve(books.size());
  for (const auto& b : books) ids.push_back(b.id);
  library->markDownloaded(ids.data(), ids.size(), booksDownloaded.data());
}

void BookOrbitCatalogBrowserActivity::loadBooks(const int page) {
  retryNetworkIfLinkUp();
  // The ERROR screen's Retry reloads `listKind` at `currentPage`, so both name THIS request from
  // the start -- otherwise a failed list load would retry the previous screen (the root) instead.
  listKind = ListKind::BOOKS;
  BOClient::BookQuery request = currentQuery;
  request.page = page;
  BOClient::BookPage result;
  offlineSavedDate.clear();
  bool fromCache = false;
  if (!offline) {
    const BOClient::Error error = catalogSession().fetchBooks(request, result);
    if (error == BOClient::OK) {
      if (catalogCache && page == 0) catalogCache->saveBooks(request, result, nowEpochSeconds());
    } else if (isTransportFailure(error)) {
      offline = true;
      catalogSessionCache.clear();
    } else {
      reportSessionError(error);
      return;
    }
  }
  if (offline) {
    BookOrbitCatalogCache::Meta meta;
    if (!catalogCache || !catalogCache->loadBooks(request, result, meta)) {
      reportSessionError(BOClient::NETWORK_ERROR);
      return;
    }
    fromCache = true;
    offlineSavedDate = formatSavedDate(meta.savedAt);
  }
  listKind = ListKind::BOOKS;
  books = std::move(result.items);
  currentQuery = request;
  totalBooks = result.total;
  listNotice.clear();
  std::vector<BOClient::Section>().swap(sections);
  std::vector<BOClient::FacetEntry>().swap(facets);
  std::vector<BookOrbitLibrary::Entry>().swap(downloaded);
  markDownloadedBooks();
  currentPage = page;
  // Offline, only the cached page exists: never offer a next page that cannot be loaded.
  currentPageHasNext = fromCache ? false : result.hasNext;
  selectorIndex = 0;
  state = BrowserState::BROWSING;
  requestUpdate();
}

void BookOrbitCatalogBrowserActivity::loadFacets(const int page) {
  retryNetworkIfLinkUp();
  listKind = ListKind::FACETS;  // see loadBooks(): Retry must reload this request
  if (offline) {
    reportSessionError(BOClient::NETWORK_ERROR);
    return;
  }
  std::string sectionId;
  switch (facetKind) {
    case FacetKind::SERIES:
      sectionId = "series";
      break;
    case FacetKind::LIBRARY:
      sectionId = "libraries";
      break;
    case FacetKind::COLLECTION:
      sectionId = "collections";
      break;
    case FacetKind::SMART_SCOPE:
      sectionId = "smart-scopes";
      break;
    case FacetKind::AUTHOR:
    case FacetKind::NONE:
    default:
      sectionId = "authors";
      break;
  }
  BOClient::FacetPage result;
  const BOClient::Error error = catalogSession().fetchSectionEntries(sectionId, page, result);
  if (error != BOClient::OK) {
    if (isTransportFailure(error)) {
      offline = true;
      catalogSessionCache.clear();
      reportSessionError(error);
      return;
    }
    reportSessionError(error);
    return;
  }
  listKind = ListKind::FACETS;
  facets = std::move(result.items);
  listNotice.clear();
  totalBooks = 0;
  std::vector<BOClient::Section>().swap(sections);
  std::vector<BOClient::BookSummary>().swap(books);
  std::vector<uint8_t>().swap(booksDownloaded);
  currentPage = page;
  currentPageHasNext = result.hasNext;
  selectorIndex = 0;
  state = BrowserState::BROWSING;
  requestUpdate();
}

void BookOrbitCatalogBrowserActivity::loadDownloaded(const int page) {
  listKind = ListKind::DOWNLOADED;
  const int pageSize = listPageItems();
  std::vector<BookOrbitLibrary::Entry> rows(static_cast<size_t>(pageSize) + 1);
  size_t got = 0;
  if (library) got = library->page(static_cast<size_t>(page) * pageSize, rows.data(), rows.size());
  rows.resize(got);
  currentPageHasNext = got > static_cast<size_t>(pageSize);
  if (currentPageHasNext) rows.pop_back();
  listKind = ListKind::DOWNLOADED;
  facetKind = FacetKind::NONE;
  downloaded = std::move(rows);
  std::vector<BOClient::Section>().swap(sections);
  std::vector<BOClient::BookSummary>().swap(books);
  std::vector<uint8_t>().swap(booksDownloaded);
  std::vector<BOClient::FacetEntry>().swap(facets);
  offlineSavedDate.clear();
  currentPage = page;
  selectorIndex = 0;
  state = BrowserState::BROWSING;
  requestUpdate();
}
void BookOrbitCatalogBrowserActivity::activateSelected() {
  const size_t count = displayCount();
  if (count == 0 || static_cast<size_t>(selectorIndex) >= count) return;

  if (listKind == ListKind::BOOKS && selectorIndex == 0) {
    retryNetworkIfLinkUp();
    if (!offline) { choiceIndex = 0; state = BrowserState::OPTIONS; requestUpdate(); }
    return;
  }

  const size_t itemIndex = toItemIndex(static_cast<size_t>(selectorIndex));
  switch (listKind) {
    case ListKind::ROOT: {
      const BOClient::Section section = sections[itemIndex];
      if (section.id == ROOT_SEARCH) {
        launchSearch();
        return;
      }
      navigationHistory.push({ListKind::ROOT, FacetKind::NONE, {}, 0, selectorIndex, location});
      location = std::string(tr(STR_BO_LIBRARY)) + " / " + section.title;
      state = BrowserState::LOADING;
      statusMessage = tr(STR_LOADING);
      requestUpdate(true);
      currentQuery = BOClient::BookQuery{};
      if (section.id == ROOT_DOWNLOADED) {
        loadDownloaded(0);
      } else if (section.id == "authors") {
        facetKind = FacetKind::AUTHOR;
        loadFacets(0);
      } else if (section.id == "series") {
        facetKind = FacetKind::SERIES;
        loadFacets(0);
      } else if (section.id == "libraries") {
        facetKind = FacetKind::LIBRARY;
        loadFacets(0);
      } else if (section.id == "collections") {
        facetKind = FacetKind::COLLECTION;
        loadFacets(0);
      } else if (section.id == "smart-scopes") {
        facetKind = FacetKind::SMART_SCOPE;
        loadFacets(0);
      } else {
        // Every other root id is a plain book listing with an optional sort/status; an unknown
        // future id falls back to the unfiltered listing.
        facetKind = FacetKind::NONE;
        if (section.id == "continue-reading") {
          currentQuery.sort = "recently_read";
          currentQuery.readStatus = "reading";
        } else if (section.id == "recent") {
          currentQuery.sort = "recently_added";
        } else if (section.id == "all-books") {
          currentQuery.sort = "title";
        }
        loadBooks(0);
      }
      break;
    }
    case ListKind::FACETS: {
      const BOClient::FacetEntry facet = facets[itemIndex];
      navigationHistory.push({ListKind::FACETS, facetKind, currentQuery, currentPage, selectorIndex, location});
      location += " / " + facet.title;
      state = BrowserState::LOADING;
      statusMessage = tr(STR_LOADING);
      requestUpdate(true);
      currentQuery = BOClient::BookQuery{};
      switch (facetKind) {
        case FacetKind::LIBRARY:
          currentQuery.libraryId = facet.id;
          break;
        case FacetKind::COLLECTION:
          currentQuery.collectionId = facet.id;
          break;
        case FacetKind::SMART_SCOPE:
          currentQuery.smartScopeId = facet.id;
          break;
        case FacetKind::AUTHOR:
          currentQuery.authorId = facet.id;
          break;
        case FacetKind::SERIES:
        case FacetKind::NONE:
        default:
          currentQuery.seriesId = facet.id;
          break;
      }
      facetKind = FacetKind::NONE;
      loadBooks(0);
      break;
    }
    case ListKind::BOOKS:
      openBookDetail(books[itemIndex]);
      break;
    case ListKind::DOWNLOADED:
      openDownloadedDetail(downloaded[itemIndex]);
      break;
  }
}

void BookOrbitCatalogBrowserActivity::openBookDetail(const BOClient::BookSummary& book) {
  retryDetail = false;
  retryBook = book;
  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdate(true);

  BOClient::BookDetail result;
  bool online = false;
  retryNetworkIfLinkUp();
  if (!offline) {
    const BOClient::Error error = catalogSession().fetchBookDetail(book.id, BookOrbitStats::statsDeviceId(), result);
    if (error == BOClient::OK) {
      online = true;
    } else if (isTransportFailure(error)) {
      offline = true;
      catalogSessionCache.clear();
    } else {
      retryDetail = true;
      reportSessionError(error);
      return;
    }
  }
  if (!online) {
    // Offline: what the list already showed, plus the local copy if this account has one.
    result = BOClient::BookDetail{};
    result.id = book.id;
    result.title = book.title;
    result.authors = book.authors;
    result.readStatus = book.readStatus;
    result.seriesName = book.seriesName;
    result.seriesIndex = book.seriesIndex;
    result.progressPercent = book.progressPercent;
  }
  detail = std::move(result);
  detailError.clear();
  detailChanged = false;
  resolveLocalCopy();
  buildDetailRows();
  detailSelectorIndex = 0;
  state = BrowserState::DETAIL;
  requestUpdate();
}

void BookOrbitCatalogBrowserActivity::openDownloadedDetail(const BookOrbitLibrary::Entry& entry) {
  BOClient::BookSummary summary;
  summary.id = entry.bookId;
  summary.title = entry.title;
  summary.authors = entry.authors;
  openBookDetail(summary);
}

void BookOrbitCatalogBrowserActivity::resolveLocalCopy() {
  detailLocalPath.clear();
  if (!library) return;

  BookOrbitLibrary::Entry entry;
  if (library->find(detail.id, entry) && Storage.exists(entry.path.c_str())) {
    detailLocalPath = entry.path;
    return;
  }
  // Re-link (index lost/evicted, or a download from before the index existed) only on content
  // identity: the server's fileHash must equal the file's KOReader partial MD5. A same-named file
  // from another account or edition therefore stays unlinked, and 下載 goes to the owner-scoped
  // path instead of aliasing it (§9 finding 5).
  const BookOrbitOwnerId owner = BookOrbitStats::currentOwner();
  for (const auto& file : detail.files) {
    if (file.fileHash.empty()) continue;
    const std::string candidates[] = {
        BookOrbitLibrary::downloadPath(owner, detail.id, file.id, detail.title, bookOrbitFirmwareSha256()),
        BookOrbitCatalogBrowserPolicy::downloadPath(detail.authors, detail.title, file.id, detail.files.size()),
    };
    for (const auto& path : candidates) {
      if (!Storage.exists(path.c_str())) continue;
      if (KOReaderDocumentId::calculate(path) != file.fileHash) {
        LOG_INF("BOCatalog", "Local file does not match the server copy; leaving it unlinked");
        continue;
      }
      BookOrbitLibrary::Entry linked;
      linked.bookId = detail.id;
      linked.fileId = file.id;
      linked.fileHash = file.fileHash;
      linked.path = path;
      linked.title = detail.title;
      linked.authors = detail.authors;
      linked.sizeBytes = static_cast<uint32_t>(std::min<uint64_t>(file.sizeBytes, UINT32_MAX));
      linked.downloadedAt = nowEpochSeconds();
      if (library->upsert(linked)) LOG_INF("BOCatalog", "Re-linked a verified local copy");
      detailLocalPath = path;
      return;
    }
  }
}

void BookOrbitCatalogBrowserActivity::buildDetailRows() {
  detailRows.clear();
  detailRows.reserve(detail.files.size() + 3);
  if (!detailLocalPath.empty()) detailRows.push_back({DetailRowKind::OPEN_LOCAL});
  if (!offline) {
    // Re-download stays available (e.g. the server copy changed) but is never the first row
    // when a local copy exists.
    for (size_t i = 0; i < detail.files.size(); i++) {
      detailRows.push_back({DetailRowKind::DOWNLOAD, static_cast<int>(i)});
    }
    detailRows.push_back({DetailRowKind::READ_STATUS});
    detailRows.push_back({DetailRowKind::RATING});
  } else if (detailLocalPath.empty()) {
    detailRows.push_back({DetailRowKind::OFFLINE_UNAVAILABLE});
  }
}

void BookOrbitCatalogBrowserActivity::downloadFile(const int fileIndex) {
  if (fileIndex < 0 || static_cast<size_t>(fileIndex) >= detail.files.size() || !library) return;
  const BOClient::BookFile file = detail.files[fileIndex];
  const BookOrbitOwnerId owner = BookOrbitStats::currentOwner();
  const std::string dest =
      BookOrbitLibrary::downloadPath(owner, detail.id, file.id, detail.title, bookOrbitFirmwareSha256());

  state = BrowserState::DOWNLOADING;
  statusMessage = detail.title;
  downloadProgress = downloadTotal = 0;
  cancelDownload = false;
  detailError.clear();
  requestUpdate(true);

  // A download owns a separate streaming HTTP client: release the idle catalog keep-alive first
  // so the X3 never carries two wolfSSL connections through the transfer.
  catalogSessionCache.clear();
  Storage.ensureDirectoryExists(BookOrbitLibrary::DOWNLOAD_DIR);
  int lastRenderedPercent = -1;
  unsigned long lastProgressUpdateMs = 0;
  BOClient::Error result = BOClient::OK;
  {
    BOClient::Session session;
    session.setPrepareHeap(&BookOrbitCatalogBrowserActivity::prepareCatalogHeap, this);
    result = session.downloadFile(
        file.id, dest,
        [this, &lastRenderedPercent, &lastProgressUpdateMs](const size_t done, const size_t total) {
          downloadProgress = done;
          downloadTotal = total;
          // Back cancels: the transfer loop runs on this task, so poll the button here.
          gpio.update();
          if (mappedInput.isPressed(MappedInputManager::Button::Back)) cancelDownload = true;
          const int percent = total > 0 ? static_cast<int>(static_cast<uint64_t>(done) * 100 / total) : 0;
          const unsigned long now = millis();
          if (percent >= 100 || lastRenderedPercent < 0 ||
              percent >= lastRenderedPercent + DOWNLOAD_PROGRESS_STEP_PERCENT ||
              now - lastProgressUpdateMs >= DOWNLOAD_PROGRESS_MIN_UPDATE_MS) {
            lastRenderedPercent = percent;
            lastProgressUpdateMs = now;
            requestUpdate(true);
          }
        },
        &cancelDownload);
  }

  if (result == BOClient::OK) {
    clearBookCache(dest);
    if (!file.fileHash.empty() && KOReaderDocumentId::calculate(dest) != file.fileHash) {
      // Not fatal (the server may serve a converted variant), but never claim a verified identity.
      LOG_ERR("BOCatalog", "Downloaded file does not match the server fileHash");
    }
    BookOrbitLibrary::Entry entry;
    entry.bookId = detail.id;
    entry.fileId = file.id;
    entry.fileHash = file.fileHash;
    entry.path = dest;
    entry.title = detail.title;
    entry.authors = detail.authors;
    entry.sizeBytes = static_cast<uint32_t>(std::min<uint64_t>(file.sizeBytes, UINT32_MAX));
    entry.downloadedAt = nowEpochSeconds();
    if (!library->upsert(entry)) LOG_ERR("BOCatalog", "Downloaded, but could not index the book");
    detailLocalPath = dest;
    buildDetailRows();
    detailSelectorIndex = 0;  // 開啟閱讀
  } else {
    LOG_ERR("BOCatalog", "Download failed: %s", BOClient::errorString(result));
    detailError = result == BOClient::ABORTED
                      ? std::string(tr(STR_BO_DOWNLOAD_FAILED))
                      : std::string(tr(STR_BO_DOWNLOAD_FAILED)) + ": " + localizedError(result);
    if (isTransportFailure(result) && !wifiConnected()) {
      offline = true;
      buildDetailRows();
    }
    consumeBack = cancelDownload;
  }
  state = BrowserState::DETAIL;
  requestUpdate();
}

void BookOrbitCatalogBrowserActivity::openLocalBook(const std::string& path) {
  if (path.empty() || !Storage.exists(path.c_str())) {
    detailLocalPath.clear();
    buildDetailRows();
    requestUpdate();
    return;
  }
  exitTarget = ExitTarget::Reader;
  exitReaderPath = path;
  // With WiFi up, onExit() reboots straight into this book (with the open sync); otherwise this
  // is the normal open, with the same remote-progress check Home uses.
  activityManager.goToReader(path, /*allowFastInitialRefresh=*/false, /*checkRemoteProgress=*/!wifiConnected());
}

void BookOrbitCatalogBrowserActivity::goBackOrHome() {
  if (entryMode == EntryMode::Settings) {
    finish();
    return;
  }
  exitTarget = ExitTarget::HomeLibrary;
  activityManager.goHome(HomeMenuItem::BOOKORBIT_LIBRARY);
}

void BookOrbitCatalogBrowserActivity::navigateBack() {
  NavFrame frame;
  if (!navigationHistory.popBack(frame)) {
    if (listKind == ListKind::ROOT || state == BrowserState::ERROR) {
      goBackOrHome();
    } else {
      state = BrowserState::LOADING;
      requestUpdate();
      loadRoot();
    }
    return;
  }

  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdate();

  location = frame.location;
  facetKind = frame.facetKind;
  currentQuery = frame.query;
  switch (frame.listKind) {
    case ListKind::ROOT:
      loadRoot();
      break;
    case ListKind::BOOKS:
      loadBooks(frame.page);
      break;
    case ListKind::FACETS:
      loadFacets(frame.page);
      break;
    case ListKind::DOWNLOADED:
      loadDownloaded(frame.page);
      break;
  }
  if (state == BrowserState::BROWSING) {
    const size_t count = displayCount();
    selectorIndex = count == 0 ? 0 : std::min(frame.selectorIndex, static_cast<int>(count) - 1);
    requestUpdate();
  }
}
void BookOrbitCatalogBrowserActivity::applyChoice() {
  if (state == BrowserState::READ_STATUS) { setReadStatus(choiceIndex); return; }
  if (state == BrowserState::RATING) { setRating(choiceIndex); return; }
  if (choiceIndex == 10) { launchSearch(); return; }
  const auto previous = currentQuery;
  const int previousPage = currentPage, previousIndex = selectorIndex;
  if (choiceIndex < 6) currentQuery.sort = SORTS[choiceIndex];
  else currentQuery.readStatus = FILTERS[choiceIndex - 6];
  paging = true;  // Keep the visible list and its query if this replacement fails.
  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdate(true);
  loadBooks(0);
  paging = false;
  if (!listNotice.empty()) { currentQuery = previous; currentPage = previousPage; selectorIndex = previousIndex; }
  requestUpdate();
}

void BookOrbitCatalogBrowserActivity::returnFromDetail() {
  detailError.clear();
  markDownloadedBooks();
  for (auto& book : books) if (book.id == detail.id) {
    book.readStatus = detail.readStatus;
    book.progressPercent = detail.progressPercent;
  }
  state = BrowserState::BROWSING;
  if (detailChanged && listKind == ListKind::BOOKS) {
    const int savedIndex = selectorIndex;
    const int page = currentPage;
    paging = true;
    loadBooks(page);
    // A status change can empty the last server page. Move to the preceding page once.
    if (listNotice.empty() && books.empty() && page > 0) loadBooks(page - 1);
    paging = false;
    selectorIndex = std::max(0, std::min(savedIndex, static_cast<int>(displayCount()) - 1));
    // If the refresh failed, never leave a known nonmatching row under the active filter.
    if (!listNotice.empty() && !currentQuery.readStatus.empty()) {
      books.erase(std::remove_if(books.begin(), books.end(), [this](const BOClient::BookSummary& book) {
        return !BookOrbitCatalogBrowserPolicy::matchesReadFilter(book.readStatus, currentQuery.readStatus);
      }), books.end());
      markDownloadedBooks();
      selectorIndex = std::min(selectorIndex, static_cast<int>(displayCount()) - 1);
    }
  }
  detailChanged = false;
  requestUpdate();
}

void BookOrbitCatalogBrowserActivity::setReadStatus(int index) {
  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdate(true);
  std::string outStatus;
  const auto error = catalogSession().setReadStatus(detail.id, BOClient::SETTABLE_READ_STATUSES[index], outStatus);
  if (error == BOClient::OK) { detail.readStatus = outStatus; detailChanged = true; }
  else detailError = localizedError(error);
  state = BrowserState::DETAIL;
  requestUpdate();
}

void BookOrbitCatalogBrowserActivity::setRating(int index) {
  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdate(true);
  int rating = -1;
  const auto error = catalogSession().setRating(detail.id, index == 0 ? -1 : index, rating);
  if (error == BOClient::OK) { detail.rating = rating; detailChanged = true; }
  else detailError = localizedError(error);
  state = BrowserState::DETAIL;
  requestUpdate();
}

void BookOrbitCatalogBrowserActivity::launchSearch() {
  consumeConfirm = true;
  state = BrowserState::SEARCH_INPUT;
  requestUpdate();

  auto keyboard = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH));
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    state = BrowserState::BROWSING;
    if (!result.isCancelled) {
      performSearch(std::get<KeyboardResult>(result.data).text);
    } else {
      requestUpdate();
    }
  });
}

void BookOrbitCatalogBrowserActivity::performSearch(const std::string& query) {
  navigationHistory.push({listKind, facetKind, currentQuery, currentPage, selectorIndex, location});
  if (listKind != ListKind::BOOKS) currentQuery = BOClient::BookQuery{};
  location += " / " + std::string(tr(STR_SEARCH)) + ": " + query;
  currentQuery.searchText = query;
  facetKind = FacetKind::NONE;
  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdate(true);
  loadBooks(0);
}

void BookOrbitCatalogBrowserActivity::checkAndConnectWifi() {
  if (wifiConnected()) {
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate();
    loadRoot();
    return;
  }
  launchWifiSelection();
}

void BookOrbitCatalogBrowserActivity::launchWifiSelection() {
  state = BrowserState::WIFI_SELECTION;
  requestUpdate();

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void BookOrbitCatalogBrowserActivity::onWifiSelectionComplete(const bool connected) {
  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdate(true);
  if (connected) {
    offline = false;
    loadRoot();
  } else {
    // No network: the library still opens, with 已下載 and the last cached root (never a dead end).
    enterOffline();
  }
}
