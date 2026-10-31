#pragma once
#include <BookOrbitCatalogClient.h>

#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/BookOrbitCatalogBrowserPolicy.h"
#include "util/BookOrbitCatalogCache.h"
#include "util/BookOrbitLibraryStore.h"
#include "util/ButtonNavigator.h"

/**
 * The single BookOrbit library (docs/bookorbit-home-library.md): BookOrbit's catalog -- root
 * sections, paged book listings with search/facet filters, book detail with read-status/rating
 * edits -- plus this account's downloaded books as the 已下載 section of that same library.
 * Button-driven, two-line rows through the theme's list component, no touch gestures.
 *
 * Books carry their availability: downloaded (open the local copy, offline too), downloadable,
 * or failed this session. Downloads land at an owner-scoped stable path and are indexed in the
 * owner-scoped BookOrbitLibrary::Store; a pre-existing file at the legacy author/title path is
 * linked only when the server's fileHash matches it. With no network the library opens offline:
 * 已下載 plus the last cached root/first pages, never a dead-end error.
 *
 * Exits while WiFi is up still reboot to defragment the heap (as before), but now to where the
 * user was going: the reader for 開啟閱讀, Home's 書庫 tile for Back.
 *
 * Deliberately does NOT implement (see docs/bookorbit-feature-matrix.md):
 *   - Dashboard scrollers, manifest/cursor bulk download, covers in lists.
 */
class BookOrbitCatalogBrowserActivity final : public Activity {
 public:
  enum class BrowserState { CHECK_WIFI, WIFI_SELECTION, LOADING, BROWSING, DETAIL, DOWNLOADING, ERROR, SEARCH_INPUT };
  // Settings: the original BookOrbit settings action (Back returns there). Home / HomeSearch: the
  // BookOrbit Home's library entry points (Back returns to Home's 書庫 tile).
  enum class EntryMode { Settings, Home, HomeSearch };

  explicit BookOrbitCatalogBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           EntryMode entryMode = EntryMode::Settings)
      : Activity("BookOrbitCatalogBrowser", renderer, mappedInput), buttonNavigator(), entryMode(entryMode) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class ListKind { ROOT, BOOKS, FACETS, DOWNLOADED };
  enum class FacetKind { NONE, AUTHOR, SERIES, LIBRARY, COLLECTION, SMART_SCOPE };
  // Why the activity is leaving, so onExit()'s WiFi heap-defrag reboot lands in the right place.
  enum class ExitTarget { Caller, Reader, HomeLibrary };

  struct NavFrame {
    ListKind listKind = ListKind::ROOT;
    FacetKind facetKind = FacetKind::NONE;
    BookOrbitCatalogClient::BookQuery query;
    int page = 0;
    int selectorIndex = 0;
  };

  // One selectable row of the DETAIL screen, built by buildDetailRows() from the book's state.
  enum class DetailRowKind { OPEN_LOCAL, DOWNLOAD, OFFLINE_UNAVAILABLE, READ_STATUS, RATING };
  struct DetailRow {
    DetailRowKind kind;
    int fileIndex = -1;  // DOWNLOAD only
  };

  ButtonNavigator buttonNavigator;
  EntryMode entryMode;
  BrowserState state = BrowserState::LOADING;
  ListKind listKind = ListKind::ROOT;
  FacetKind facetKind = FacetKind::NONE;
  std::vector<BookOrbitCatalogClient::Section> sections;
  std::vector<BookOrbitCatalogClient::BookSummary> books;
  std::vector<uint8_t> booksDownloaded;  // parallel to `books`: 1 when a local copy is indexed
  std::vector<BookOrbitCatalogClient::FacetEntry> facets;
  std::vector<BookOrbitLibrary::Entry> downloaded;  // one page of the 已下載 section
  BookOrbitCatalogBrowserPolicy::SingleServerSessionCache<BookOrbitCatalogClient::Session> catalogSessionCache;
  BookOrbitCatalogBrowserPolicy::NavigationHistory<NavFrame> navigationHistory;
  BookOrbitCatalogClient::BookQuery currentQuery;
  int currentPage = 0;
  bool currentPageHasNext = false;
  int selectorIndex = 0;
  bool consumeConfirm = false;
  bool consumeBack = false;
  bool backReleasePending = true;  // a Back still held from the previous activity is not ours
  std::string errorMessage;
  std::string statusMessage;
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;
  bool cancelDownload = false;

  // Offline: no network this session (WiFi unavailable/declined, or a transport-level failure).
  bool offline = false;
  // Non-empty while the shown list came from the offline cache: "YYYY/MM/DD" of when it was saved.
  std::string offlineSavedDate;
  bool pendingSearch = false;  // HomeSearch: open search once the root is ready

  // Owner-scoped stores, created on entry when BookOrbit credentials exist (null otherwise).
  std::unique_ptr<BookOrbitLibrary::Store> library;
  std::unique_ptr<BookOrbitCatalogCache::Cache> catalogCache;

  // Populated while state == DETAIL.
  BookOrbitCatalogClient::BookDetail detail;
  std::string detailLocalPath;  // indexed, existing local copy; empty when none
  std::string detailError;      // last download failure for this book, shown on the detail screen
  std::vector<DetailRow> detailRows;
  int detailSelectorIndex = 0;

  ExitTarget exitTarget = ExitTarget::Caller;
  std::string exitReaderPath;

  size_t itemCount() const;
  size_t displayCount() const;
  bool isPrevPageRow(size_t displayIndex) const;
  bool isNextPageRow(size_t displayIndex) const;
  size_t toItemIndex(size_t displayIndex) const;
  int listPageItems() const;

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void enterOffline();
  void retryNetworkIfLinkUp();
  BookOrbitCatalogClient::Session& catalogSession();
  // Session::PrepareHeapFn for every catalog request: drops the rebuildable SD-font page arenas
  // the status screen rendered with, under the RenderLock, right before the TLS heap gate.
  static void prepareCatalogHeap(void* activity);
  void loadRoot();
  void loadBooks(int page);
  void loadFacets(int page);
  void loadDownloaded(int page);
  void markDownloadedBooks();
  void activateSelected();
  void openBookDetail(const BookOrbitCatalogClient::BookSummary& book);
  void openDownloadedDetail(const BookOrbitLibrary::Entry& entry);
  void resolveLocalCopy();
  void buildDetailRows();
  void navigateBack();
  void goBackOrHome();
  void downloadFile(int fileIndex);
  void openLocalBook(const std::string& path);
  void cycleReadStatus();
  void cycleRating();
  void launchSearch();
  void performSearch(const std::string& query);
  void reportSessionError(BookOrbitCatalogClient::Error error);
  void renderDetail(int pageWidth, int pageHeight);
  bool preventAutoSleep() override { return true; }
};
