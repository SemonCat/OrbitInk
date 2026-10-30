#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Utf8.h>
#include <Xtc.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "DashboardHomeModel.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "PinnedBookStore.h"
#include "RecentBooksStore.h"
#include "activities/reader/EpubReaderUtils.h"
#include "components/UITheme.h"
#include "components/themes/dashboard/DashboardTheme.h"
#include "components/themes/lyra/LyraCarouselTheme.h"
#include "fontIds.h"
#include "util/KOReaderAutoSync.h"

namespace {
constexpr unsigned long KOSYNC_NOTICE_DURATION_MS = 2500;
}

int HomeActivity::getMenuItemCount() const {
  int count = 4;  // File Browser, Recents, Transfer & Sync, Settings
  if (!recentBooks.empty()) {
    count += recentBooks.size();
  }
  if (pinnedBookVisible) {
    count++;
  }
  return count;
}

void HomeActivity::loadRecentBooks(int maxBooks, const std::string& excludePath) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(static_cast<int>(books.size()), maxBooks));

  // The pinned book (if visible) gets its own Home entry and stays put regardless of reading
  // activity, so it's excluded here to keep "most recently read" reflecting whatever book was
  // actually most recent besides it -- unless that's the only book, in which case there's nothing
  // else to show and it appears in both places.
  bool excludedAny = false;
  for (const RecentBook& book : books) {
    if (!excludePath.empty() && book.path == excludePath) {
      excludedAny = true;
      continue;
    }
    if (RecentBooksStore::isMissing(book)) continue;
    if (static_cast<int>(recentBooks.size()) >= maxBooks) break;
    recentBooks.push_back(book);
  }

  if (recentBooks.empty() && excludedAny) {
    for (const RecentBook& book : books) {
      if (RecentBooksStore::isMissing(book)) continue;
      if (static_cast<int>(recentBooks.size()) >= maxBooks) break;
      recentBooks.push_back(book);
    }
  }
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;

  // Carousel needs two purpose-sized caches (center box, side trapezoids) instead of the one
  // height-keyed cache every other theme's single Continue Reading card uses -- see
  // LyraCarouselTheme::kCenterThumbW/kSideCoverW's comment for why sharing one cache produced
  // near-blank covers for off-aspect-ratio art.
  const bool isCarouselTheme =
      static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_CAROUSEL;

  int progress = 0;
  for (RecentBook& book : recentBooks) {
    if (!book.coverBmpPath.empty()) {
      // Self-heal a stale coverBmpPath template. Confirmed on real hardware: a book added to
      // Recents by a different build's cover-path scheme (e.g. a "[WIDTH]x[HEIGHT]"-shaped
      // template no CrossFade code has ever produced -- this fork's own history never contained
      // that string) leaves a path that getCoverThumbPath's substitution -- it only knows
      // "[HEIGHT]" -- can never fully resolve, so it looks up a filename like
      // "thumb_[WIDTH]x400.bmp" that will never exist, on every theme, forever. The template is
      // wrong, not merely the file missing, so re-derive it fresh every time rather than retrying
      // a path that can't succeed -- cheap (no I/O until .load()), and guarantees this always
      // matches what EpubReaderActivity writes on open and what generateThumbBmp below writes.
      std::string freshTemplate;
      if (FsHelpers::hasEpubExtension(book.path)) {
        freshTemplate = Epub(book.path, "/.crosspoint").getThumbBmpPath();
      } else if (FsHelpers::hasXtcExtension(book.path)) {
        freshTemplate = Xtc(book.path, "/.crosspoint").getThumbBmpPath();
      }
      if (!freshTemplate.empty() && freshTemplate != book.coverBmpPath) {
        book.coverBmpPath = freshTemplate;
        RECENT_BOOKS.updateBook(book.path, book.title, book.author, freshTemplate);
        // Also invalidate coverBufferStored, not just coverRendered: HomeActivity::render()
        // restores the stored buffer (if any) BEFORE calling into the theme on every render, off
        // whatever coverBufferStored was left at here. Leaving it true would let that restore
        // keep re-serving the pre-fix (stale) buffer, silently suppressing the redraw this
        // coverRendered reset was meant to force -- same class of bug as the centerIdx gate race.
        coverRendered = false;
        coverBufferStored = false;
        requestUpdate();
      }

      bool needsCenter = false;
      bool needsSide = false;
      bool needsLegacy = false;
      if (isCarouselTheme) {
        const std::string centerPath =
            UITheme::getCoverThumbPath(book.coverBmpPath, LyraCarouselTheme::kCenterThumbW,
                                       LyraCarouselTheme::kCenterThumbH);
        const std::string sidePath = UITheme::getCoverThumbPath(book.coverBmpPath, LyraCarouselTheme::kSideCoverW,
                                                                 LyraCarouselTheme::kSideCoverH);
        needsCenter = !Storage.exists(centerPath.c_str());
        needsSide = !Storage.exists(sidePath.c_str());
      } else {
        const std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
        needsLegacy = !Storage.exists(coverPath.c_str());
      }

      if (needsCenter || needsSide || needsLegacy) {
        // If epub, try to load the metadata for title/author and cover
        if (FsHelpers::hasEpubExtension(book.path)) {
          Epub epub(book.path, "/.crosspoint");
          // Skip loading css since we only need metadata here. buildIfMissing=false, so a book
          // whose book.bin cache isn't already built (anything except the just-opened book, whose
          // cache is guaranteed fresh) legitimately fails to load here -- previously this return
          // value was ignored and generateThumbBmp() got called anyway, almost always failing too,
          // which permanently wiped coverBmpPath (see below) for a book that may just need its
          // cache built later, not one with no cover. Skip it for this pass instead; loadRecentBooks
          // reloads coverBmpPath fresh next time Home is entered, so this isn't a permanent loss.
          if (epub.load(false, true)) {
            // Try to generate thumbnail image for Continue Reading card
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
            bool success = true;
            if (isCarouselTheme) {
              if (needsCenter)
                success = epub.generateThumbBmp(LyraCarouselTheme::kCenterThumbW, LyraCarouselTheme::kCenterThumbH) &&
                          success;
              if (needsSide)
                success =
                    epub.generateThumbBmp(LyraCarouselTheme::kSideCoverW, LyraCarouselTheme::kSideCoverH) && success;
            } else {
              success = epub.generateThumbBmp(coverHeight);
            }
            if (!success) {
              RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
              book.coverBmpPath = "";
            }
            // coverBufferStored too, not just coverRendered -- see the self-heal block's comment
            // above: leaving it true lets HomeActivity::render()'s next buffer-restore keep
            // re-serving the pre-generation (placeholder) buffer and silently swallow this reset.
            coverRendered = false;
            coverBufferStored = false;
            requestUpdate();
          }
        } else if (FsHelpers::hasXtcExtension(book.path)) {
          // Handle XTC file
          Xtc xtc(book.path, "/.crosspoint");
          if (xtc.load()) {
            // Try to generate thumbnail image for Continue Reading card
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
            bool success = true;
            if (isCarouselTheme) {
              if (needsCenter)
                success = xtc.generateThumbBmp(LyraCarouselTheme::kCenterThumbW, LyraCarouselTheme::kCenterThumbH) &&
                          success;
              if (needsSide)
                success =
                    xtc.generateThumbBmp(LyraCarouselTheme::kSideCoverW, LyraCarouselTheme::kSideCoverH) && success;
            } else {
              success = xtc.generateThumbBmp(coverHeight);
            }
            if (!success) {
              RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
              book.coverBmpPath = "";
            }
            coverRendered = false;
            coverBufferStored = false;
            requestUpdate();
          }
        }
      }
    }
    progress++;
  }

  recentsLoaded = true;
  recentsLoading = false;
}

const DashboardTheme* HomeActivity::dashboardTheme() { return UITheme::getInstance().getDashboardTheme(); }

void HomeActivity::loadDashboardBrowseList() {
  const auto& books = RECENT_BOOKS.getBooks();
  const std::string& pinnedPath = PINNED_BOOK.getPinnedPath();
  const bool pinEnabled = SETTINGS.pinBookToHome && PINNED_BOOK.hasPinned();

  dashboard::BrowseOrderInput input;
  input.recentCount = static_cast<int>(books.size());
  input.pinEnabled = pinEnabled;
  // One existence probe for the pin, plus one per recent below -- the same per-book check
  // loadRecentBooks() already performs for every other theme, not a new class of Home I/O.
  input.pinnedValid = pinEnabled && Storage.exists(pinnedPath.c_str());
  input.maxEntries = DASHBOARD_BROWSE_MAX;
  for (size_t i = 0; i < books.size(); ++i) {
    if (books[i].path == pinnedPath) {
      input.pinnedRecentIndex = static_cast<int>(i);
      break;
    }
  }

  int order[DASHBOARD_BROWSE_MAX] = {};
  const int count = dashboard::buildBrowseOrder(
      input,
      [](const void* ctx, const int index) {
        return !RecentBooksStore::isMissing((*static_cast<const std::vector<RecentBook>*>(ctx))[index]);
      },
      &books, order, DASHBOARD_BROWSE_MAX);

  dashboardBrowse.clear();
  dashboardBrowse.reserve(count);
  for (int i = 0; i < count; ++i) {
    if (order[i] == dashboard::kPinnedSlot) {
      // Pinned but never in Recents: PinnedBookStore keeps only a path and a title, so the card
      // shows those and simply has no author line -- rather than inventing one.
      RecentBook pinnedEntry;
      pinnedEntry.path = pinnedPath;
      pinnedEntry.title = PINNED_BOOK.getPinnedTitle();
      dashboardBrowse.push_back(std::move(pinnedEntry));
    } else {
      dashboardBrowse.push_back(books[order[i]]);
    }
  }
  dashboardPinnedFirst = input.pinEnabled && input.pinnedValid && !dashboardBrowse.empty();
  dashboardBrowseIndex = 0;
  syncDashboardSelection();
}

void HomeActivity::syncDashboardSelection() {
  recentBooks.clear();
  dashboardHasStats = false;
  dashboardStats = BookReadingStats{};
  if (dashboardBrowse.empty()) return;
  dashboardBrowseIndex = dashboard::clampBrowseIndex(dashboardBrowseIndex, static_cast<int>(dashboardBrowse.size()));
  // recentBooks holds exactly the shown book, so every flat-selector path already in this
  // activity (activateSelection, the Back shortcut, the cover-tile tap) resolves to it unchanged.
  recentBooks.push_back(dashboardBrowse[dashboardBrowseIndex]);
  loadDashboardBookStats(recentBooks.front().path);
}

void HomeActivity::loadDashboardBookStats(const std::string& path) {
  // The ONLY file this Home reads per shown book, and it is a ~19-byte stats.bin. The cache path
  // comes from Epub's constructor, which is pure string arithmetic (a std::hash of the path plus a
  // prefix -- no I/O, no logging); nothing here opens the book.
  //
  // In particular this deliberately does NOT compute a reading percentage. Book-level progress
  // needs book.bin's cumulative spine sizes, and every route to those opens the book:
  // Epub::load(), or BookMetadataCache::load() against the same file. Home does not open books, so
  // the card carries no progress readout at all -- see DashboardLayout.h. Title and author come
  // from the RecentBooksStore entry that is already in memory, and no cover is loaded or generated
  // either, which is why this Home is materially cheaper than the one every other theme draws.
  dashboardHasStats = false;
  dashboardStats = BookReadingStats{};
  if (SETTINGS.shouldTrackReadingStats() && FsHelpers::hasEpubExtension(path)) {
    const Epub epub(path, "/.crosspoint");
    dashboardStats = BookReadingStats::load(epub.getCachePath());
    // A book with no qualifying session yet is indistinguishable from "tracking was off", so the
    // card omits the whole stats panel rather than printing a misleading set of zeros.
    dashboardHasStats = dashboardStats.sessionCount > 0;
  }
}

void HomeActivity::stepDashboardBrowse(const int delta) {
  const int count = static_cast<int>(dashboardBrowse.size());
  const int next = delta >= 0 ? dashboard::nextBrowseIndex(dashboardBrowseIndex, count)
                              : dashboard::prevBrowseIndex(dashboardBrowseIndex, count);
  // Pull the Home selection onto the card even when the index did not move (a one-book list), so
  // Left/Right followed by Confirm always opens the book on screen.
  const bool bookChanged = next != dashboardBrowseIndex;
  if (!bookChanged && selectorIndex == 0) return;  // nothing would change; skip the repaint
  selectorIndex = 0;
  if (bookChanged) {
    dashboardBrowseIndex = next;
    syncDashboardSelection();  // only here: this is what pays for the shown book's stats.bin read
  }
  requestUpdate();
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  showKoSyncSuccess = KOReaderAutoSync::consumeSuccessNotice();
  if (showKoSyncSuccess) koSyncSuccessTime = millis();

  hasOpdsServers = OPDS_STORE.hasServers();
  const auto& metrics = UITheme::getInstance().getMetrics();

  if (dashboardTheme() != nullptr) {
    // The card IS the pinned book (and Continue Reading), so neither gets a menu row of its own
    // and the pinned-row fit check below does not apply. Everything downstream -- getMenuItemCount,
    // indexToMenuItem, activateSelection -- then works unchanged off recentBooks holding exactly
    // the one shown book.
    pinnedBookVisible = false;
    loadDashboardBrowseList();
    selectorIndex = initialMenuItem == HomeMenuItem::NONE
                        ? 0
                        : static_cast<int>(recentBooks.size()) + menuItemToIndex(initialMenuItem, false);
    requestUpdate();
    return;
  }

  bool pinAvailable =
      SETTINGS.pinBookToHome && PINNED_BOOK.hasPinned() && Storage.exists(PINNED_BOOK.getPinnedPath().c_str());
  if (pinAvailable) {
    // Continue Reading (in themes where it renders as its own menu row) needs at least one
    // non-missing recent book to show at all -- independent of whether loadRecentBooks below ends
    // up excluding the pinned path from that book, since the "show both" fallback means recentBooks
    // ends up non-empty in exactly the same cases this loop does.
    bool anyNonMissingRecent = false;
    for (const auto& book : RECENT_BOOKS.getBooks()) {
      if (!RecentBooksStore::isMissing(book)) {
        anyNonMissingRecent = true;
        break;
      }
    }
    const int continueReadingRow = (metrics.homeContinueReadingInMenu && anyNonMissingRecent) ? 1 : 0;
    const int totalFlatItems = 4 /* Browse, Recents, Transfer & Sync, Settings */ + continueReadingRow + 1 /* Pinned */;
    const int menuTop = metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset;
    const int bottomEdge = GUI.getMenuBottomEdge(renderer, menuTop, totalFlatItems);
    // metrics.buttonHintsHeight is already 0 when hints are hidden (touch device or
    // SETTINGS.hideButtonHints -- see UITheme::getMetrics()), so this naturally reflects whichever
    // state is currently active without a separate branch.
    const int availableBottom = renderer.getScreenHeight() - metrics.buttonHintsHeight;
    // Safety margin, not measurement slack -- per-theme (see ThemeMetrics::homePinnedRowFitBuffer):
    // 16px by default (a close call is resolved by hiding the row rather than rendering a few
    // pixels into the hints band), reduced only for themes whose real margin has been judged
    // acceptable on-device (currently RoundedRaff -- see RoundedRaffMetrics's comment).
    pinAvailable = (bottomEdge + metrics.homePinnedRowFitBuffer) <= availableBottom;
  }
  pinnedBookVisible = pinAvailable;

  loadRecentBooks(metrics.homeRecentBooksCount, pinnedBookVisible ? PINNED_BOOK.getPinnedPath() : "");

  const auto base = static_cast<int>(recentBooks.size());
  selectorIndex =
      initialMenuItem == HomeMenuItem::NONE ? 0 : base + menuItemToIndex(initialMenuItem, pinnedBookVisible);

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();

  // Free the stored cover buffer if any
  freeCoverBuffer();
}

bool HomeActivity::storeCoverBuffer() {
  // render() must have already set the cover rect; without it we'd be back to
  // cloning the whole framebuffer.
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  freeCoverBuffer();
  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (needed == 0) return false;
  coverBuffer = static_cast<uint8_t*>(malloc(needed));
  if (!coverBuffer) {
    LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", (unsigned)needed);
    return false;
  }
  coverBufferSize = needed;
  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize)) {
    free(coverBuffer);
    coverBuffer = nullptr;
    coverBufferSize = 0;
    return false;
  }
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer || coverRectW <= 0 || coverRectH <= 0) return false;
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize);
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferSize = 0;
  coverBufferStored = false;
}

void HomeActivity::loop() {
  if (showKoSyncSuccess && millis() - koSyncSuccessTime >= KOSYNC_NOTICE_DURATION_MS) {
    showKoSyncSuccess = false;
    requestUpdate();
  }
  const int menuCount = getMenuItemCount();
  const auto& metrics = UITheme::getInstance().getMetrics();

  auto activateSelection = [this] {
    if (selectorIndex < recentBooks.size()) {
      onSelectBook(recentBooks[selectorIndex].path);
      return;
    }
    const int menuIndex = selectorIndex - static_cast<int>(recentBooks.size());
    switch (indexToMenuItem(menuIndex, pinnedBookVisible)) {
      case HomeMenuItem::PINNED:
        onSelectBook(PINNED_BOOK.getPinnedPath());
        break;
      case HomeMenuItem::FILE_BROWSER:
        onFileBrowserOpen();
        break;
      case HomeMenuItem::RECENTS:
        onRecentsOpen();
        break;
      case HomeMenuItem::TRANSFER_AND_SYNC:
        onTransferAndSyncOpen();
        break;
      case HomeMenuItem::SETTINGS_MENU:
        onSettingsOpen();
        break;
      default:
        break;
    }
  };

  const bool isCarouselTheme =
      static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_CAROUSEL;

  if (dashboardTheme() != nullptr) {
    // Front Left/Right browse the card's book; side Up/Down move the flat selector (card, then
    // the menu rows). Bound to the physical buttons directly rather than through
    // NavNext/NavPrevious, which composite side Up/Down WITH front Left/Right into a single axis
    // (see MappedInputManager::mapButton) and would make Right do both jobs at once -- the same
    // split the carousel branch below already needs.
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] { stepDashboardBrowse(1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] { stepDashboardBrowse(-1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this, menuCount] {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
      requestUpdate();
    });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this, menuCount] {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
      requestUpdate();
    });
  } else if (isCarouselTheme) {
    // Two-level nav, this theme only: Left/Right move within whichever level selectorIndex is
    // currently in (carousel books, or menu icons); Up/Down -- either one, since there are only
    // two levels here, so both mean "switch level" -- cross between them, restoring each level's
    // own last position (lastCarouselIndex/lastMenuIndex) instead of resetting to index 0.
    // Bypasses NavNext/NavPrevious (which composite side Up/Down with front Left/Right into one
    // axis -- see MappedInputManager::mapButton) so the two physical pairs can mean different
    // things here. Back/Confirm are untouched, so Resume/Select on the front-left buttons behave
    // exactly as before.
    const int bookCount = static_cast<int>(recentBooks.size());
    const int menuOnlyCount = menuCount - bookCount;

    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this, bookCount, menuOnlyCount] {
      if (selectorIndex < bookCount) {
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, bookCount);
      } else {
        selectorIndex = bookCount + ButtonNavigator::nextIndex(selectorIndex - bookCount, menuOnlyCount);
      }
      requestUpdate();
    });

    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this, bookCount, menuOnlyCount] {
      if (selectorIndex < bookCount) {
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, bookCount);
      } else {
        selectorIndex = bookCount + ButtonNavigator::previousIndex(selectorIndex - bookCount, menuOnlyCount);
      }
      requestUpdate();
    });

    const auto switchLevel = [this, bookCount, menuOnlyCount] {
      if (selectorIndex < bookCount) {
        if (menuOnlyCount <= 0) return;
        lastCarouselIndex = selectorIndex;
        selectorIndex = bookCount + std::clamp(lastMenuIndex, 0, menuOnlyCount - 1);
      } else {
        if (bookCount <= 0) return;
        lastMenuIndex = selectorIndex - bookCount;
        selectorIndex = std::clamp(lastCarouselIndex, 0, bookCount - 1);
      }
      requestUpdate();
    };
    buttonNavigator.onPress({MappedInputManager::Button::Up, MappedInputManager::Button::Down}, switchLevel);
  } else {
    buttonNavigator.onNext([this, menuCount] {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
      requestUpdate();
    });

    buttonNavigator.onPrevious([this, menuCount] {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
      requestUpdate();
    });
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    requestUpdate();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) backPressSeen = true;

  // Back is otherwise unused on the home menu: open the most recently read
  // book directly (recentBooks is most-recent-first and already pruned of
  // files missing from the SD card). backPressSeen guards against the stale
  // release of the Back press that closed the previous activity.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && backPressSeen && !recentBooks.empty()) {
    onSelectBook(recentBooks[0].path);
    return;
  }

  int tx = 0;
  int ty = 0;
  if (!recentBooks.empty() && mappedInput.wasScreenTouchDown(tx, ty) && tx >= 0 && tx < renderer.getScreenWidth() &&
      ty >= metrics.homeTopPadding && ty < metrics.homeTopPadding + metrics.homeCoverTileHeight) {
    if (selectorIndex != 0) {
      selectorIndex = 0;
      requestUpdate();
    }
    return;
  }

  if (!recentBooks.empty() &&
      mappedInput.wasTapInRect(0, metrics.homeTopPadding, renderer.getScreenWidth(), metrics.homeCoverTileHeight)) {
    selectorIndex = 0;
    activateSelection();
    return;
  }

  const int menuTop = metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset;
  const int renderedMenuSelection =
      metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - recentBooks.size();
  const int renderedMenuCount =
      menuCount - (metrics.homeContinueReadingInMenu ? 0 : static_cast<int>(recentBooks.size()));
  int menuRow = -1;
  const auto menuTouch = mappedInput.rowTouch(menuRow, menuTop, metrics.menuRowHeight + metrics.menuSpacing,
                                              renderedMenuCount, 0, INT32_MAX, metrics.menuRowHeight);
  if (menuTouch != MappedInputManager::RowTouch::None) {
    const int touchedIndex =
        metrics.homeContinueReadingInMenu ? menuRow : menuRow + static_cast<int>(recentBooks.size());
    if (menuTouch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != touchedIndex) {
        selectorIndex = touchedIndex;
        requestUpdate();
      }
    } else {
      selectorIndex = touchedIndex;
      activateSelection();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelection();
  }
}

void HomeActivity::renderDashboard(const ThemeMetrics& metrics, const DashboardTheme& theme) {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  // nullptr title: the Dashboard header is a restrained status band, not a titled header.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding}, nullptr);

  DashboardCardData card;
  card.book = recentBooks.empty() ? nullptr : &recentBooks.front();
  card.selected = !recentBooks.empty() && selectorIndex == 0;
  card.pinned = dashboardPinnedFirst && dashboardBrowseIndex == 0;
  card.hasStats = dashboardHasStats;
  card.stats = dashboardStats;
  card.browseIndex = dashboardBrowseIndex;
  card.browseCount = static_cast<int>(dashboardBrowse.size());
  // The card sizes itself to its content, so the action rows go directly beneath whatever it
  // turned out to be rather than at a fixed offset from the band -- which would strand them under
  // a short card (no author, no recorded reading time) with a band-sized gap above them.
  const int cardBottom =
      theme.drawHomeCard(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight}, card);

  // Same four rows, same routes, same handlers as every other theme -- Browse Books (which honours
  // the cover/titles view setting), Recent Books, Transfer & Sync (BookOrbit/OPDS or File
  // Transfer) and Settings. Only their appearance is this theme's.
  static constexpr StrId menuLabels[] = {StrId::STR_BROWSE_FILES, StrId::STR_MENU_RECENT_BOOKS,
                                         StrId::STR_TRANSFER_AND_SYNC, StrId::STR_SETTINGS_TITLE};
  constexpr int menuItemCount = static_cast<int>(sizeof(menuLabels) / sizeof(menuLabels[0]));
  const int menuTop = cardBottom + metrics.homeMenuTopOffset;
  GUI.drawButtonMenu(
      renderer, Rect{0, menuTop, pageWidth, pageHeight - menuTop - metrics.buttonHintsHeight}, menuItemCount,
      selectorIndex - static_cast<int>(recentBooks.size()),
      [](const int index) { return std::string(I18N.get(menuLabels[index])); }, nullptr);

  // Front Left/Right browse books here (see loop()'s dashboard branch), so the hints say so --
  // the flat selector moves on the side Up/Down buttons, which Home never labels.
  const auto labels = mappedInput.mapLabels(recentBooks.empty() ? "" : tr(STR_RESUME), tr(STR_SELECT), tr(STR_DIR_LEFT),
                                            tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Entering Home on this theme repaints the whole screen with large filled areas (light-gray
  // card, inverted pill, filled menu row), so the first paint gets the flashing waveform and
  // everything after it -- Left/Right browsing included -- stays on the default non-flashing one.
  // A conservative default chosen from what the display API offers, NOT from measured ghosting:
  // the simulator cannot show panel behaviour at all, so this wants confirming on real hardware.
  if (!dashboardFirstRenderDone) {
    dashboardFirstRenderDone = true;
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  } else {
    renderer.displayBuffer();
  }

  if (showKoSyncSuccess) GUI.drawPopup(renderer, tr(STR_ALREADY_SYNCED));

  // No loadRecentCovers() pass: this card is typographic, so Home never loads or generates a
  // cover thumbnail. firstRenderDone is still latched so the shared paths that consult it behave
  // the same, but there is no deferred second pass to schedule.
  firstRenderDone = true;
  recentsLoaded = true;
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  if (const DashboardTheme* dashboard = dashboardTheme()) {
    renderDashboard(metrics, *dashboard);
    return;
  }

  renderer.clearScreen();
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding},
                 metrics.homeContinueReadingInMenu && !recentBooks.empty() ? recentBooks[0].title.c_str() : nullptr);

  // Record the tile rect so storeCoverBuffer (called from the theme) knows
  // which sub-region of the framebuffer to snapshot. ~16 KB in Portrait
  // instead of the 48 KB full framebuffer the previous bind captured.
  coverRectX = 0;
  coverRectY = metrics.homeTopPadding;
  coverRectW = pageWidth;
  coverRectH = metrics.homeCoverTileHeight;

  const float recentProgressPercent =
      (!recentBooks.empty() && selectorIndex >= 0 && selectorIndex < static_cast<int>(recentBooks.size()))
          ? EpubReaderUtils::recentBookProgressPercent(recentBooks[selectorIndex].path)
          : -1.0f;
  GUI.drawRecentBookCover(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                          recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&HomeActivity::storeCoverBuffer, this), recentProgressPercent);

  // Build menu items dynamically
  std::vector<const char*> menuItems = {tr(STR_BROWSE_FILES), tr(STR_MENU_RECENT_BOOKS), tr(STR_TRANSFER_AND_SYNC),
                                        tr(STR_SETTINGS_TITLE)};
  std::vector<UIIcon> menuIcons = {Folder, Recent, Transfer, Settings};

  if (pinnedBookVisible) {
    // Pinned Book's selectorIndex slot (see indexToMenuItem) sits directly above the base menu
    // items and below Continue Reading's slot -- inserted here, before the Continue Reading
    // insert below, so a later Continue Reading insert pushes it to sit visually above Pinned
    // Book, matching their selectorIndex order.
    menuItems.insert(menuItems.begin(), PINNED_BOOK.getPinnedTitle().c_str());
    menuIcons.insert(menuIcons.begin(), Pin);
  }

  if (metrics.homeContinueReadingInMenu && !recentBooks.empty()) {
    // Insert Continue Reading at the top if enabled in theme
    menuItems.insert(menuItems.begin(), tr(STR_CONTINUE_READING));
    menuIcons.insert(menuIcons.begin(), Book);
  }

  GUI.drawButtonMenu(
      renderer,
      Rect{0, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset, pageWidth,
           pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing +
                         metrics.homeMenuTopOffset + metrics.buttonHintsHeight)},
      static_cast<int>(menuItems.size()),
      metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - recentBooks.size(),
      [&menuItems](int index) { return std::string(menuItems[index]); },
      [&menuIcons](int index) { return menuIcons[index]; });

  // Carousel's front Left/Right buttons move between books/menu icons (see loop()'s
  // isCarouselTheme branch), not the flat Up/Down-labeled Next/Previous every other theme uses --
  // the hint must match what the buttons actually do now.
  const bool isCarouselTheme =
      static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_CAROUSEL;
  const auto labels = isCarouselTheme
                          ? mappedInput.mapLabels(recentBooks.empty() ? "" : tr(STR_RESUME), tr(STR_SELECT),
                                                  tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT))
                          : mappedInput.mapLabels(recentBooks.empty() ? "" : tr(STR_RESUME), tr(STR_SELECT),
                                                  tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();

  if (showKoSyncSuccess) GUI.drawPopup(renderer, tr(STR_ALREADY_SYNCED));

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
  } else if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    loadRecentCovers(metrics.homeCoverHeight);
  }
}

void HomeActivity::onSelectBook(const std::string& path) {
  activityManager.goToReader(path, /*allowFastInitialRefresh=*/false, /*checkRemoteProgress=*/true);
}

void HomeActivity::onFileBrowserOpen() {
  // Grouping needs every book's series known before the first page can render correctly, and the
  // same rebuild also pre-renders any missing cover thumbnails (see LibraryIndexRebuildActivity /
  // LibraryIndexBuilder's coverBackfill) -- so Covers always routes through an index check first,
  // even with SETTINGS.groupBySeries off, or covers for never-opened books would only ever appear
  // lazily as their page happens to be scrolled to. Near-instant if nothing changed since the last
  // build, a cancellable rebuild otherwise. Titles only needs the index for grouping, so it stays
  // gated behind groupBySeries. The stock file browser never groups and is unaffected.
  switch (SETTINGS.fileBrowserView) {
    case CrossPointSettings::FILE_BROWSER_COVERS:
      activityManager.goToLibraryIndexRebuild([] { activityManager.goToCoverGridBrowser(); });
      break;
    case CrossPointSettings::FILE_BROWSER_TITLES:
      if (SETTINGS.groupBySeries) {
        activityManager.goToLibraryIndexRebuild([] { activityManager.goToLibraryList(); });
      } else {
        activityManager.goToLibraryList();
      }
      break;
    default:
      activityManager.goToFileBrowser();
      break;
  }
}

void HomeActivity::onRecentsOpen() {
  if (SETTINGS.recentBooksView == CrossPointSettings::RECENT_BOOKS_COVERS) {
    activityManager.goToCoverGridRecentBooks();
  } else {
    activityManager.goToRecentBooks();
  }
}

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onTransferAndSyncOpen() {
  if (hasOpdsServers) {
    activityManager.goToTransferAndSync();
  } else {
    // No OPDS servers configured -- go straight to File Transfer, the same one-tap experience
    // this row replaced when there was nothing to pick between.
    activityManager.goToFileTransfer();
  }
}
