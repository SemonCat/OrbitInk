#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_system.h>

#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>

#include "../../util/BookOrbitStatsSync.h"
#include "../../util/BookmarkFile.h"
#include "BookStatsActivity.h"
#include "BookmarkEntry.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "CustomZhTwProfile.h"
#include "DictionaryWordSelectActivity.h"
#include "EpubReaderBookmarksActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderUtils.h"
#include "FinishedBooksStore.h"
#include "KOReaderDocumentId.h"
#include "MappedInputManager.h"
#include "PinnedBookStore.h"
#include "ProgressMapper.h"
#include "ReadingPositionPolicy.h"
#include "QrDisplayActivity.h"
#include "QuickResumeEligibility.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/settings/TextSettingsActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookmarkUtil.h"
#include "util/KOReaderAutoSync.h"
#include "util/ProgressSyncDeferred.h"
#include "util/ScreenshotUtil.h"

namespace {
#if CROSSFADE_CUSTOM_ZH_TW
#define READER_PERF_LOG LOG_INF
#else
#define READER_PERF_LOG LOG_DBG
#endif
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
// pages per minute, first item is 1 to prevent division by zero if accessed
constexpr int PAGE_TURN_RATES[] = {1, 1, 3, 6, 12};
constexpr size_t initialBookmarkCacheCapacity = 16;
constexpr float bookmarkProgressEpsilon = 0.0001f;

bool renderWasSuperseded(const void* context) { return static_cast<const RenderLock*>(context)->isStale(); }

// Reading-stats dwell gating (see EpubReaderActivity::recordPageDwellTime). A dwell longer than
// the idle threshold means the reader was left open (sleep, distraction), not actively read; one
// shorter than the minimum is an accidental/instant double-turn, not a read page.
// The dwell thresholds themselves now live on PageDwellTracker, next to the code that applies
// them; these aliases keep the surrounding comments and any other reader references honest.
constexpr uint32_t READING_STATS_IDLE_THRESHOLD_SECONDS = PageDwellTracker::IDLE_THRESHOLD_SECONDS;
constexpr uint32_t READING_STATS_MIN_DWELL_SECONDS = PageDwellTracker::MIN_DWELL_SECONDS;
// A session under this doesn't count as a qualifying "open" for sessionCount purposes -- a quick
// peek shouldn't inflate how many times a book was "read".
constexpr uint32_t READING_STATS_MIN_SESSION_SECONDS = 60;

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

// SD card folder finished books are moved into. Single source of truth for the path.
// constexpr ⇒ lives in flash .rodata, no DRAM cost.
constexpr char READ_FOLDER[] = "/read";

// True if path is inside READ_FOLDER (starts with "<READ_FOLDER>/"). Non-allocating so
// it is cheap to call from loop(), and avoids reintroducing a separate "/Read/" literal.
bool isInReadFolder(const std::string& path) {
  constexpr size_t n = sizeof(READ_FOLDER) - 1;  // length of "/Read" (excludes NUL)
  return path.size() > n && path.compare(0, n, READ_FOLDER) == 0 && path[n] == '/';
}

struct ProgressRange {
  float start;
  float end;
};

ProgressRange getPageProgressRange(const std::shared_ptr<Epub>& epub, const int spineIndex, const int page,
                                   const int pageCount) {
  if (pageCount <= 1) {
    return {epub->calculateProgress(spineIndex, 0.0f), epub->calculateProgress(spineIndex, 1.0f)};
  }

  const float step = 1.0f / static_cast<float>(pageCount - 1);
  const float anchor = std::clamp(static_cast<float>(page) * step, 0.0f, 1.0f);
  const float start = std::max(0.0f, anchor - (step * 0.5f));
  const float end = std::min(1.0f, anchor + (step * 0.5f));
  return {epub->calculateProgress(spineIndex, start), epub->calculateProgress(spineIndex, end)};
}

bool bookmarkMatchesProgress(const BookmarkEntry& bookmark, const int spineIndex, const int page, const int pageCount,
                             const ProgressRange& pageRange) {
  if (bookmark.computedSpineIndex == spineIndex && bookmark.computedChapterPageCount == pageCount &&
      bookmark.computedChapterProgress == page) {
    return true;
  }

  const float bookmarkProgress = std::clamp(bookmark.percentage, 0.0f, 1.0f);
  return bookmarkProgress + bookmarkProgressEpsilon >= pageRange.start &&
         bookmarkProgress - bookmarkProgressEpsilon <= pageRange.end;
}

// Pick a non-colliding destination path inside /Read/ for a finished book.
// Mirrors the suffixing scheme used elsewhere: "name.epub" -> "name (2).epub", etc.
std::string buildReadFolderDestination(const std::string& srcPath) {
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;

  Storage.mkdir(READ_FOLDER);
  std::string dstPath = std::string(READ_FOLDER) + "/" + filename;
  if (!Storage.exists(dstPath.c_str())) {
    return dstPath;
  }

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = std::string(READ_FOLDER) + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
}

// Relocate a finished book and its cache dir into /read/, keep it in recents by
// repointing its entry to the new path, and repoint the resume pointer too.
// On rename failure: LOG_ERR and leave everything in place (no UI alert subsystem here).
void moveFinishedBookToReadFolder(const std::string& srcPath, const std::string& dstPath,
                                  const std::string& oldCachePath) {
  LOG_INF("ERS", "Moving finished epub: %s -> %s", srcPath.c_str(), dstPath.c_str());
  if (!Storage.rename(srcPath.c_str(), dstPath.c_str())) {
    LOG_ERR("ERS", "Failed to move finished book to '/Read' folder");
    return;
  }

  // Cache dir is keyed by hash of the epub path (see Epub ctor), so it must be re-keyed.
  const std::string newCachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(dstPath));
  if (!oldCachePath.empty() && Storage.exists(oldCachePath.c_str())) {
    if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("ERS", "Failed to rename cache dir %s -> %s (non-fatal)", oldCachePath.c_str(), newCachePath.c_str());
    }
  }

  // Keep the book in recents (crossink behavior): repoint the entry to its new
  // location instead of dropping it. updatePath persists on success.
  RECENT_BOOKS.updatePath(srcPath, dstPath, oldCachePath, newCachePath);
  FINISHED_BOOKS.updatePath(srcPath, dstPath);
  PINNED_BOOK.updatePath(srcPath, dstPath);
  if (APP_STATE.openEpubPath == srcPath) {
    APP_STATE.openEpubPath = dstPath;
    APP_STATE.saveToFile();
  }
}

}  // namespace

void EpubReaderActivity::onEnter() {
  Activity::onEnter();

  showKoSyncSuccess = KOReaderAutoSync::consumeSuccessNotice();
  if (showKoSyncSuccess) koSyncSuccessTime = millis();

  if (!epub) {
    return;
  }

  ImageBlock::clearSessionRenderFailures();
  // Lazy image extraction: section builds only header-probe images, so the first
  // render of an image page pulls the file out of the EPUB through this hook.
  ImageBlock::setExtractor(epub.get(), [](void* ctx, const char* src, const char* dest) {
    return static_cast<Epub*>(ctx)->extractItemToFile(src, dest);
  });

  // Configure screen orientation based on settings
  // NOTE: This affects layout math and must be applied before any render calls.
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  epub->setupCacheDir();

  sessionReadingSeconds = 0;
  sessionPaceSampleSeconds = 0;
  sessionPaceSampleCount = 0;
  if (SETTINGS.shouldTrackReadingStats()) {
    stats = BookReadingStats::load(epub->getCachePath());
    globalStats = GlobalReadingStats::load();
  } else {
    stats = BookReadingStats{};
    globalStats = GlobalReadingStats{};
  }
  pageDwell.pageShown(static_cast<uint32_t>(millis()));

  // BookOrbit page-stats capture. Resolved once per session: it needs the reading-stats setting
  // on, credentials configured, and the server already confirmed as BookOrbit (see
  // BookOrbitStats::captureEnabled()). The clock is anchored from the RTC here so every event in
  // the session shares one anchor; an invalid/unset RTC leaves it unanchored, which disables
  // capture for the session while leaving normal progress sync completely unaffected.
  bookOrbitEventCount = 0;
  bookOrbitClock = BookOrbitSessionClock{};
  statsSession.reset();
  bookOrbitCaptureEnabled = BookOrbitStats::captureEnabled();
  // Bind this session's events to the server/account that captured them, so a later
  // reconfiguration cannot upload them to a different one.
  bookOrbitOwner = BookOrbitStats::currentOwner();
  bookOrbitDocumentHash.clear();
  if (bookOrbitCaptureEnabled) {
    // The queue is keyed by document hash, not by the cache path, so it survives a book move or
    // rename the same way the server's own view of the book does.
    //
    // ALWAYS the binary partial MD5, whatever the user's progress match method is: BookOrbit can
    // only ever resolve a page-stats hash intrinsically (or through a manual link that already
    // exists), and its page-stats endpoint never records an unmatched hash anywhere a user could
    // create that link from. A filename hash would therefore queue events that come back
    // unmatched forever. Computed only on this gated path, since it reads from the book file.
    bookOrbitDocumentHash = KOReaderDocumentId::calculateForStats(epub->getPath());
    if (bookOrbitDocumentHash.empty()) {
      LOG_INF("BOStats", "No document hash for this book; skipping reading-stats capture");
      bookOrbitCaptureEnabled = false;
    }
  }
  if (bookOrbitCaptureEnabled) {
    uint32_t epochMinutes = 0;
    if (!halClock.getEpochMinutes(epochMinutes) || !bookOrbitClock.anchor(epochMinutes, millis())) {
      LOG_INF("BOStats", "No valid RTC time; skipping reading-stats capture this session");
      bookOrbitCaptureEnabled = false;
    } else {
      LOG_INF("BOStats", "Capturing page stats; queue holds %u event(s) at open",
              static_cast<unsigned>(BookOrbitStatsQueue::queuedCount(bookOrbitDocumentHash, bookOrbitOwner)));
    }
  }

  const bool hadSavedProgress = EpubReaderUtils::loadProgress(epub->getCachePath(), currentSpineIndex,
                                                              nextPageNumber, cachedChapterTotalPageCount,
                                                              progressEventTimestamp, progressIsLegacy);
  if (hadSavedProgress) {
    cachedSpineIndex = currentSpineIndex;
    LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
  }
  // We may want a better condition to detect if we are opening for the first time.
  // This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0 && !hadSavedProgress) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }
  if (!hadSavedProgress) readingNavigationPending = true;
  if (const auto target = ProgressSyncDeferred::readerSpine(epub)) {
    currentSpineIndex = *target;
    nextPageNumber = 0;
    cachedChapterTotalPageCount = 0;
    remotePositionPending = true;
    readingNavigationPending = false;
    skipInitialRender = false;
  }

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());

  loadCachedBookmarks();

  if (skipInitialRender) {
    skipInitialRender = false;
    if (tryQuickResumeWithoutRender()) {
      // The already-restored sleep-frame pixels match this state exactly -- no render needed.
      return;
    }
    // Any validation failure (cache miss/partial/stale spec/out-of-range page) falls back to the
    // normal path below, identical to a boot with no Quick Resume at all.
  }

  // Trigger first update
  requestUpdate();
}

void EpubReaderActivity::onExit() {
  Activity::onExit();

  // Reading-stats session commit: final dwell on whatever page is still showing, then a single
  // batched write per book/global file for the whole session -- not per page turn (see
  // recordPageDwellTime, called throughout the session already; this just closes it out and
  // persists). Must run before section/epub reset below, since refreshEstimatedTimeLeft() and the
  // cache-path save both need them.
  // Normally the session's one and only commit. When manual KOSync was launched it has already
  // run (before that path released epub/section for the TLS handshake) and the latch makes this
  // a no-op rather than a double count.
  finalizeReadingStatsSession();

  // The extractor holds a raw pointer to this activity's epub; drop it before
  // the activity (and the shared_ptr) goes away.
  ImageBlock::setExtractor(nullptr, nullptr);

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();

  // Flush any debounced progress now: onExit() runs synchronously before deep sleep
  // (enterDeepSleep -> goToSleep -> onExit, before pushOnSleep/startDeepSleep) and before every
  // book transition, so this is what bounds crash-only loss to the debounce window while
  // guaranteeing sleep/exit/KOSync always see the exact current position on disk.
  if (!flushPendingProgress()) {
    LOG_ERR("ERS", "Failed to flush progress on exit");
    // enterDeepSleep() already optimistically saved quickResumeSkipRenderEligible=true (it must
    // read the pre-sleep activity before goToSleep() replaces currentActivity, ahead of this
    // flush). A failed flush leaves progress.bin stale relative to the sleep-frame snapshot the
    // next boot will restore -- withdraw the bit so Quick Resume falls back to a full render
    // instead of trusting a page number that disagrees with what's on screen.
    APP_STATE.quickResumeSkipRenderEligible = false;
    APP_STATE.saveToFile();
  }

  section.reset();
  if (pendingReadFolderMove && epub) {
    const std::string srcPath = epub->getPath();
    const std::string oldCachePath = epub->getCachePath();
    const std::string dstPath = buildReadFolderDestination(srcPath);
    epub.reset();  // release the Epub (and any open handles) before renaming on the SD card
    moveFinishedBookToReadFolder(srcPath, dstPath, oldCachePath);
  } else {
    epub.reset();
  }
}

void EpubReaderActivity::openReaderMenu() {
  ensureCurrentPageLoaded();  // the FOOTNOTES hint below needs accurate currentPageFootnotes
  const int currentPage = section ? section->currentPage + 1 : 0;
  const int totalPages = section ? section->estimatedTotalPages() : 0;
  float bookProgress = 0.0f;
  if (epub->getBookSize() > 0 && section && section->estimatedTotalPages() > 0) {
    const float chapterProgress =
        static_cast<float>(section->currentPage) / static_cast<float>(section->estimatedTotalPages());
    bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
  }
  const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
  startActivityForResult(std::make_unique<EpubReaderMenuActivity>(
                             renderer, mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent,
                             SETTINGS.orientation, !currentPageFootnotes.empty(), !cachedBookmarks.empty(),
                             FINISHED_BOOKS.isFinished(epub->getPath()), SETTINGS.shouldTrackReadingStats()),
                         [this](const ActivityResult& result) {
                           // Always apply orientation change even if the menu was cancelled
                           const auto& menu = std::get<MenuResult>(result.data);
                           applyOrientation(menu.orientation);
                           toggleAutoPageTurn(menu.pageTurnOption);
                           if (!result.isCancelled) {
                             onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
                           }
                         });
}

bool EpubReaderActivity::buildTickHeapGate() {
  const size_t freeHeap = ESP.getFreeHeap();
  const size_t maxBlock = ESP.getMaxAllocHeap();
  // Below the floors: just wait. The tick is deferrable — page-turn transients
  // free up between turns and the tick retries every loop pass. Track the
  // paused state so skipLoopDelay() stops pinning the CPU at full speed while
  // no build work is actually happening (the gate can stay closed for a long
  // stretch if the retained build context itself holds the heap down).
  buildHeapPaused = freeHeap < BACKGROUND_BUILD_MIN_FREE_HEAP || maxBlock < BACKGROUND_BUILD_MIN_MAX_ALLOC;
  return !buildHeapPaused;
}

void EpubReaderActivity::showBuildPopup() {
  // Mid-build indexing popup: only during onEnter's blocking build-to-target phase
  // (buildPopupPending), at most once, and only when the framebuffer isn't on loan.
  // If it fires while the loan is active (e.g. the parser's size-based call during
  // startBuild), pending stays set and the deadline check retries after the loan.
  if (!buildPopupPending || !renderer.hasFrameBuffer()) return;
  GUI.drawPopup(renderer, tr(STR_INDEXING));
  // HALF-clear the popup when the page replaces it, else "INDEXING" ghosts.
  pagesUntilFullRefresh = 1;
  buildPopupPending = false;
}

void EpubReaderActivity::openDictionaryWordSelect() {
  if (SETTINGS.dictionaryName[0] == '\0') {
    showDictionaryMessage = true;
    dictionaryMessageTime = millis();
    requestUpdate();
    return;
  }
  if (!section) return;
  auto page = section->loadPage(section->currentPage);
  if (!page) return;

  // Word geometry must match render(): viewable-area margins plus screen margin.
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;

  startActivityForResult(std::make_unique<DictionaryWordSelectActivity>(renderer, mappedInput, std::move(page),
                                                                        orientedMarginLeft, orientedMarginTop),
                         [this](const ActivityResult&) { requestUpdate(); });
}

void EpubReaderActivity::loop() {
  if (!epub) {
    // Should never happen
    finish();
    return;
  }

  // Idle glyph prewarm for the likely next page (currentPage + 1). The scan
  // pass draws nothing (FCM scan mode suppresses pixels), so the displayed
  // framebuffer is untouched; endScanAndPrewarm loads only glyphs not already
  // cached. Debounced past rapid page-flipping, one attempt per position, and
  // deferred while a render/build owns the CPU or the heap is at the render
  // floor. Cross-chapter prewarm is deliberately out of scope (next spine's
  // section isn't loaded).
  constexpr unsigned long IDLE_PREWARM_DEBOUNCE_MS = 400;
  if (CustomZhTwProfile::IDLE_PAGE_PREWARM_ENABLED && section && !section->isBuilding() && !RenderLock::peek() &&
      renderer.hasFrameBuffer() && lastRenderCompleteMs != 0 &&
      millis() - lastRenderCompleteMs > IDLE_PREWARM_DEBOUNCE_MS && ESP.getFreeHeap() > RENDER_MIN_FREE_HEAP &&
      ESP.getMaxAllocHeap() > BACKGROUND_BUILD_MIN_MAX_ALLOC &&
      (idlePrewarmSpine != currentSpineIndex || idlePrewarmPage != section->currentPage)) {
    RenderLock lock(true, true);  // page table stable; generation cancels superseded work
    // Re-check under the lock: peek() and acquisition are not atomic, so the render
    // task may have reset/replaced the section or moved the page in between.
    if (section && !section->isBuilding() &&
        (idlePrewarmSpine != currentSpineIndex || idlePrewarmPage != section->currentPage)) {
      const int nextPage = section->currentPage + 1;
      bool completed = true;
      if (nextPage < static_cast<int>(section->pageCount)) {
        if (const auto p = section->loadPage(nextPage)) {
          if (auto* fcm = renderer.getFontCacheManager()) {
            const auto t0 = millis();
            auto scope = fcm->createPrewarmScope();
            const PageRenderCancellation cancellation{renderWasSuperseded, &lock};
            completed = p->render(renderer, SETTINGS.getReaderFontId(), 0, 0, &cancellation) &&
                        scope.endScanAndPrewarm(renderWasSuperseded, &lock) && !lock.isStale();
            if (!completed) scope.cancel();
            LOG_DBG("ERS", "Idle prewarm: page %d in %lums (%s)", nextPage, millis() - t0,
                    completed ? "complete" : "cancelled");
          }
        }
      }
      // Mark only completed attempts. A cancelled scan must remain eligible to
      // retry after the pending page turn settles.
      if (completed) {
        idlePrewarmSpine = currentSpineIndex;
        idlePrewarmPage = section->currentPage;
      }
    }
  }

  // Lazily resume a partial's extension build once the reader nears its watermark. Far from
  // it the rebuild is all cost (whole-chapter re-layout from page 0) and no benefit this
  // session, so reopening a partial deliberately does NOT start it (see the deferral in
  // render()); crossing this margin is the signal that the reader will actually need pages
  // past the watermark soon. Uses the last render's viewport so pagination matches the
  // partial being extended.
  if (section && !section->isBuilding() && section->isPartial() && !RenderLock::peek() && buildViewportWidth > 0 &&
      !partialRebuildStartFailed &&
      (remotePositionPending ||
       section->currentPage + PARTIAL_REBUILD_START_MARGIN >= static_cast<int>(section->pageCount))) {
    RenderLock lock;
    // Reuse the last render's viewport so the extension paginates identically to the partial.
    const ReaderRenderSpec buildSpec = SETTINGS.readerRenderSpec(buildViewportWidth, buildViewportHeight);
    if (!section->startBuild(buildSpec)) {
      // Not fatal: the partial keeps serving its pages; crossing the watermark falls back to
      // the blocking extension in render(). Don't retry every tick.
      partialRebuildStartFailed = true;
      if (remotePositionPending) {
        remoteBuildFailed = true;
        requestUpdate();
      }
      LOG_ERR("ERS", "Failed to start deferred partial extension build");
    } else {
      LOG_DBG("ERS", "Reader near partial watermark (%d/%d), resuming extension build", section->currentPage,
              section->pageCount);
    }
  }

  // Drive any in-progress incremental section build forward, off the page-turn critical path,
  // but only within a small window ahead of the reader: an unbounded build monopolized the
  // RenderLock and locked out page turns. The build follows the reader instead, and instant
  // reopen comes from suspendBuild() persisting the laid-out pages as a partial on exit.
  // Skip while the render mutex is busy so we never delay a pending render; re-check
  // isBuilding() under the lock since render() may have just finished it.
  // While extending a partial (rebuild from a previous session), pageCount is pinned at the
  // partial's watermark until the build catches up, so the window check would wrongly read
  // "far enough ahead" and stall the build at 0 pages -- then the first turn past the
  // watermark re-parses the whole chapter synchronously. Keep ticking until it finalizes.
  if (section && !RenderLock::peek() && hasBuildWorkInWindow() && buildTickHeapGate()) {
    RenderLock lock;
    // Re-check the same predicate under the lock: render() (which also holds the RenderLock) may
    // have finalized the build -- or advanced the reader past/into the look-ahead window --
    // between the outer check and acquiring the lock here, in which case buildSomeMore() would
    // fail and wrongly reset the section. The heap gate must be re-read
    // too: a render that won the lock race can expand retained glyph buffers, invalidating the
    // pre-lock heap reading. (The cppcheck knownConditionTrueFalse suppression that used to sit
    // here was for the old bare section->isBuilding() re-check; the analyzer does not flag the
    // predicate call, so it would now be a dead directive.)
    if (hasBuildWorkInWindow() && buildTickHeapGate()) {
      if (!section->buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK, BACKGROUND_BUILD_BUDGET_MS)) {
        LOG_ERR("ERS", "Background section build failed");
        remoteBuildFailed = remotePositionPending;
        section.reset();
        requestUpdate();
      } else if (section->isBuildComplete() && applyDeferredReposition()) {
        // The chapter re-paginated since the saved progress (settings changed): we now know the
        // real page count, so re-render at the remapped page. No-op for an unchanged resume.
        requestUpdate();
      }
    }
  }

  if (readerbuild::requiredHeapWaitExpired(remoteBuildHeapPausedSince,
                                           remotePositionPending && section && section->isBuilding(), buildHeapPaused,
                                           static_cast<uint32_t>(millis())) &&
      !RenderLock::peek()) {
    RenderLock lock;
    // Recheck after acquiring the render lock: a completed render may have freed the heap.
    if (remotePositionPending && hasBuildWorkInWindow() && !buildTickHeapGate()) {
      LOG_ERR("ERS", "Required remote pagination stopped after persistent heap pressure");
      remoteBuildFailed = true;
      section.reset();  // Release retained parser state; the durable raw anchor stays pending.
      requestUpdate();
    } else {
      remoteBuildHeapPausedSince.reset();
    }
  }

  if (remotePositionPending) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
      return;
    }
    if (!remoteApplyAttempted && section && !section->isBuilding() && !section->isPartial() && !RenderLock::peek()) {
      RenderLock lock;
      remoteApplyAttempted = true;
      DeferredProgress pending;
      const bool applied = ProgressSyncDeferred::applyForBook(epub->getPath(), renderer);
      if ((applied || !ProgressSyncDeferred::pendingForBook(epub->getPath(), pending)) &&
          EpubReaderUtils::loadProgress(epub->getCachePath(), currentSpineIndex, nextPageNumber,
                                        cachedChapterTotalPageCount, progressEventTimestamp, progressIsLegacy)) {
        remotePositionPending = false;
        pageDwell.pageShown(static_cast<uint32_t>(millis()));
        section.reset();
        requestUpdate();
      } else {
        // Keep the durable raw anchor for reopen; do not repeat failed SD writes every tick.
        GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
      }
    }
    // Back and power remain available while the raw anchor awaits compatible pagination.
    // Do not stamp/save a placeholder page or count indexing time as reading time.
    return;
  }

  // Bound the debounce window even when the user isn't turning pages: render() only observes a
  // position change on an actual page/state change, so a lone pending change from a while ago
  // would otherwise sit unflushed indefinitely while the reader just sits on a page. Cheap peek
  // before the lock, same pattern as the build-tick check above.
  if (footnoteDepth == 0 && progressDebounce.hasPendingChange() && !RenderLock::peek() &&
      progressDebounce.isFlushDue(millis())) {
    RenderLock lock(true, true);
    if (section && footnoteDepth == 0 && progressDebounce.isFlushDue(millis())) {
      if (saveProgress(currentSpineIndex, section->currentPage, section->estimatedTotalPages())) {
        progressDebounce.markFlushed();
      }
    }
  }

  // End-of-Book screen reached (currentSpineIndex == spine count) means the book is
  // finished. Two independent finished-book features key off this same condition.
  const bool atEndOfBook = currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount();

  // Drop this book from the Recent Books list; if the reader then pages back into the book,
  // re-add it. So removal only sticks if the reader leaves while still on the End-of-Book
  // screen. Acts only on the transition (guarded by recentsEntryRemoved) — no per-frame writes.
  if (SETTINGS.removeReadBooksFromRecents) {
    if (atEndOfBook && !recentsEntryRemoved) {
      // Only treat the book as "removed by us" if it was actually in the list, so the
      // re-add branch below doesn't insert a book the feature never removed.
      recentsEntryRemoved = RECENT_BOOKS.removeByPath(epub->getPath());
    } else if (!atEndOfBook && recentsEntryRemoved) {
      // Re-add (goes to front of the list via addBook — accepted ordering side effect).
      RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
      recentsEntryRemoved = false;
    }
  }

  // Arm the move here so ANY exit path (Back, Home, file browser) relocates the book into
  // /Read/ in onExit(); paging back off the end screen disarms it (book not actually
  // finished). If removeReadBooksFromRecents also fired, RecentBooksStore::updatePath in the
  // move path becomes a safe no-op since the entry was already removed.
  if (atEndOfBook) {
    pendingReadFolderMove = SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath());
  } else {
    pendingReadFolderMove = false;
  }

  const auto touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back) || ReaderUtils::isTouchMenuGesture(mappedInput)) {
      automaticPageTurnActive = false;
      // updates chapter title space to indicate page turn disabled
      requestUpdate();
      return;
    }

    if (!section) {
      requestUpdate();
      return;
    }

    // Skips page turn if renderingMutex is busy
    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      pageTurn(true);
      return;
    }
  }

  if (showBookmarkMessage && (millis() - bookmarkMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showBookmarkMessage = false;
    requestUpdate();
  }

  if (showDictionaryMessage && (millis() - dictionaryMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showDictionaryMessage = false;
    requestUpdate();
  }

  if (showKoSyncSuccess && (millis() - koSyncSuccessTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showKoSyncSuccess = false;
    requestUpdate();
  }

  // While the end screen suggestion menu is showing it owns Confirm/Back/navigation
  // input. Anything it doesn't handle (e.g. long-press Back to the file browser) falls
  // through to the regular handlers below; page turns are absorbed by the end-of-book
  // block. A Confirm release after a long-press function (bookmark/sync) fired is left
  // to the regular Confirm handler below, which consumes it via ignoreNextConfirmRelease.
  if (atEndOfBook && endOfBookOptions.menuActive() &&
      !(ignoreNextConfirmRelease && mappedInput.wasReleased(MappedInputManager::Button::Confirm))) {
    std::string openPath;
    switch (endOfBookOptions.handleMenuInput(mappedInput, &openPath)) {
      case EndOfBookOptions::Action::OpenBook:
        activityManager.goToReader(openPath, /*allowFastInitialRefresh=*/false,
                                   /*checkRemoteProgress=*/true);
        return;
      case EndOfBookOptions::Action::GoHome:
        onGoHome();
        return;
      case EndOfBookOptions::Action::LastPage:
        currentSpineIndex = std::max(epub->getSpineItemsCount() - 1, 0);
        nextPageNumber = 0;
        pendingPageJump = std::numeric_limits<uint16_t>::max();
        requestUpdate();
        return;
      case EndOfBookOptions::Action::Redraw:
        requestUpdate();
        return;
      case EndOfBookOptions::Action::None:
        break;
    }
  }

  // Enter reader menu activity on short-press Confirm or a downward swipe from the top edge. A long-press
  // that fired a bound function (bookmark or KOReader sync) sets ignoreNextConfirmRelease so the release
  // following the hold does not also open the menu.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || ReaderUtils::isTouchMenuGesture(mappedInput)) {
    if (ignoreNextConfirmRelease) {
      ignoreNextConfirmRelease = false;
    } else {
      openReaderMenu();
    }
  }

  // Long-press Confirm runs the user-selected function (SETTINGS.longPressMenuFunction).
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    switch (SETTINGS.longPressMenuFunction) {
      case CrossPointSettings::LP_MENU_BOOKMARK:
        // Hold ~0.4s drops a bookmark at the current page.
        if (mappedInput.getHeldTime() >= ReaderUtils::BOOKMARK_HOLD_MS && !showBookmarkMessage) {
          addBookmark();
          showBookmarkMessage = true;
          ignoreNextConfirmRelease = true;  // Prevent accidental menu open after adding bookmark
          bookmarkMessageTime = millis();
          requestUpdate();
        }
        break;
      case CrossPointSettings::LP_MENU_DICTIONARY:
        // Hold ~0.4s starts dictionary word selection on the current page.
        if (mappedInput.getHeldTime() >= ReaderUtils::BOOKMARK_HOLD_MS && !showDictionaryMessage) {
          ignoreNextConfirmRelease = true;  // Prevent menu open on the release that follows
          openDictionaryWordSelect();
          return;
        }
        break;
      case CrossPointSettings::LP_MENU_DISABLED:
      default:
        break;
    }
  }

  // Short press Back restores position when viewing a footnote (takes priority over navigation)
  if (footnoteDepth > 0 && mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_BACK_OR_HOME_MS) {
    restoreSavedPosition();
    return;
  }

  if (ReaderUtils::handleBackNavigation(mappedInput, activityManager, epub ? epub->getPath().c_str() : "",
                                        {this, [](void* ctx) { static_cast<EpubReaderActivity*>(ctx)->onGoHome(); }})) {
    return;
  }

  // auto [prevTriggered, nextTriggered] = ReaderUtils::detectPageTurn(mappedInput);

  // Handle short power button press for footnotes
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FOOTNOTES &&
      mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      !mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
    } else {
      ensureCurrentPageLoaded();
      if (currentPageFootnotes.size() == 1) {
        navigateToHref(currentPageFootnotes[0].href, true);
      } else if (currentPageFootnotes.size() > 1) {
        startActivityForResult(
            std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                navigateToHref(footnoteResult.href, true);
              }
              requestUpdate();
            });
      }
    }
    return;
  }

  auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  prevTriggered = prevTriggered || touch.prev;
  nextTriggered = nextTriggered || touch.next;
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // At end of the book with no suggestion menu, forward button goes home and back
  // button returns to last page
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    if (endOfBookOptions.menuActive()) {
      // Selection movement was handled above; absorb leftover page-turn triggers so
      // e.g. "previous" at the top of the list doesn't jump back into the book
      return;
    }
    if (nextTriggered) {
      onGoHome();
    } else {
      currentSpineIndex = epub->getSpineItemsCount() - 1;
      nextPageNumber = 0;
      pendingPageJump = std::numeric_limits<uint16_t>::max();
      requestUpdate();
    }
    return;
  }

  const unsigned long heldMs = (touch.prev || touch.next) ? touch.heldMs : mappedInput.getHeldTime();
  const bool longPress = !fromTilt && heldMs > ReaderUtils::SKIP_HOLD_MS;

  // Don't skip chapter after screenshot
  if (gpio.wasReleased(HalGPIO::BTN_POWER) && gpio.wasReleased(HalGPIO::BTN_DOWN)) {
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP) {
    // Both branches below replace the visible page without going through pageTurn(), so the
    // outgoing page's dwell has to be closed here or its time lands on whatever the skip
    // arrives at -- and, for stats, at that page's position rather than the one actually read.
    leaveCurrentPageForStats();

    if (!nextTriggered && section && section->currentPage > 0) {
      section->currentPage = 0;
      requestUpdate();
      return;
    }

    // We don't want to delete the section mid-render, so grab the semaphore
    {
      RenderLock lock(*this);
      nextPageNumber = 0;
      if (nextTriggered) {
        currentSpineIndex++;
      } else if (currentSpineIndex > 0) {
        currentSpineIndex--;
      }
      section.reset();
    }
    requestUpdate();
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.ORIENTATION_CHANGE) {
    const uint8_t newOrientation =
        nextTriggered ? (SETTINGS.orientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                      : (SETTINGS.orientation + 1) % SETTINGS.ORIENTATION_COUNT;
    applyOrientation(newOrientation);
    requestUpdate();
    return;
  }

  // No current section, attempt to rerender the book
  if (!section) {
    requestUpdate();
    return;
  }

  if (prevTriggered) {
    pageTurn(false);
  } else {
    pageTurn(true);
  }
}

// Translate an absolute percent into a spine index plus a normalized position
// within that spine so we can jump after the section is loaded.
void EpubReaderActivity::jumpToPercent(int percent) {
  leaveCurrentPageForStats();
  if (!epub) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return;
  }

  // Normalize input to 0-100 to avoid invalid jumps.
  percent = clampPercent(percent);

  // Convert percent into a byte-like absolute position across the spine sizes.
  // Use an overflow-safe computation: (bookSize / 100) * percent + (bookSize % 100) * percent / 100
  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    // Ensure the final percent lands inside the last spine item.
    targetSize = bookSize - 1;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return;
  }

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      // Found the spine item containing the absolute position.
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  // Store a normalized position within the spine so it can be applied once loaded.
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (pendingSpineProgress < 0.0f) {
    pendingSpineProgress = 0.0f;
  } else if (pendingSpineProgress > 1.0f) {
    pendingSpineProgress = 1.0f;
  }

  // Reset state so render() reloads and repositions on the target spine.
  {
    RenderLock lock(*this);
    readingNavigationPending = true;
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
  }
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  auto progressChangeResultHandler = [this](const ActivityResult& result) {
    loadCachedBookmarks();
    if (!result.isCancelled) {
      const auto& sync = std::get<ProgressChangeResult>(result.data);
      int targetSpineIndex = sync.spineIndex;
      int targetPage = sync.page;
      const int activeTotalPages = section ? section->estimatedTotalPages() : 0;
      const bool cachedPageMatchesActiveSection = section && sync.totalPages > 0 &&
                                                  currentSpineIndex == sync.spineIndex && sync.page >= 0 &&
                                                  sync.page < sync.totalPages && activeTotalPages == sync.totalPages;

      if (!cachedPageMatchesActiveSection && sync.hasSavedProgress) {
        const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
        CrossPointPosition fallback =
            ProgressMapper::toCrossPoint(epub, {sync.xpath, sync.percentage}, renderer, currentSpineIndex, totalPages);
        targetSpineIndex = fallback.spineIndex;
        targetPage = fallback.pageNumber;
      }

      // Anything below replaces the visible page without a page turn; close its dwell first so
      // the time is credited to the page that earned it, at the position it was read at.
      leaveCurrentPageForStats();

      readingNavigationPending = true;

      if (currentSpineIndex != targetSpineIndex) {
        RenderLock lock(*this);
        currentSpineIndex = targetSpineIndex;
        nextPageNumber = targetPage;
        section.reset();
      } else if (section && section->currentPage != targetPage) {
        RenderLock lock(*this);
        const int clampedTargetPage = std::max(0, targetPage);
        section->currentPage = clampedTargetPage;
      } else if (!section) {
        nextPageNumber = targetPage;
      }
    }
  };

  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      const std::string path = epub->getPath();
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineIdx),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              const auto& chapterResult = std::get<ChapterResult>(result.data);
              leaveCurrentPageForStats();
              readingNavigationPending = true;
              RenderLock lock(*this);

              currentSpineIndex = chapterResult.spineIndex;

              // If anchor is not empty, it will be used later to calculate the page number.
              pendingAnchor = chapterResult.anchor;

              // Otherwise page 0 will be used.
              nextPageNumber = 0;

              section.reset();
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                                 navigateToHref(footnoteResult.href, true);
                               }
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TEXT_SETTINGS: {
      startActivityForResult(std::make_unique<TextSettingsActivity>(renderer, mappedInput, &sdFontSystem.registry(),
                                                                    TextSettingsActivity::Tab::Family),
                             [this](const ActivityResult&) {
                               // TextSettingsActivity saves on each change; no save needed here.
                               // Font/size/spacing/margin changes invalidate the current
                               // layout: preserve position and force a re-layout, mirroring
                               // applyOrientation()'s reflow.
                               RenderLock lock(*this);
                               if (section) {
                                 cachedSpineIndex = currentSpineIndex;
                                 cachedChapterTotalPageCount = section->pageCount;
                                 nextPageNumber = section->currentPage;
                               }
                               section.reset();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      float bookProgress = 0.0f;
      if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
        const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DICTIONARY: {
      openDictionaryWordSelect();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        std::string fullText = section->getTextFromSectionFile();
        if (!fullText.empty()) {
          startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                                 [this](const ActivityResult& result) {});
          break;
        }
      }
      // If no text or page loading failed, just close menu
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      {
        RenderLock lock(*this);
        if (epub && section) {
          uint16_t backupSpine = currentSpineIndex;
          uint16_t backupPage = section->currentPage;
          uint16_t backupPageCount = section->pageCount;
          section.reset();
          epub->clearCache();
          epub->setupCacheDir();
          if (!saveProgress(backupSpine, backupPage, backupPageCount)) {
            LOG_ERR("ERS", "Failed to save progress before cache clear");
          } else {
            progressDebounce.markFlushed();
          }
        }
      }
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock(*this);
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOKMARKS: {
      startActivityForResult(
          std::make_unique<EpubReaderBookmarksActivity>(renderer, mappedInput, epub, epub->getPath()),
          progressChangeResultHandler);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TOGGLE_BOOKMARK: {
      addBookmark();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TOGGLE_FINISHED: {
      const std::string path = epub->getPath();
      FINISHED_BOOKS.setFinished(path, !FINISHED_BOOKS.isFinished(path));
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::READING_STATS: {
      // Refresh the time-left estimate off the current session's pace before showing it --
      // otherwise it's whatever was cached at the last onExit(), which could be a whole session
      // stale.
      refreshEstimatedTimeLeft();
      startActivityForResult(std::make_unique<BookStatsActivity>(renderer, mappedInput, stats),
                             [this](const ActivityResult&) { requestUpdate(); });
      break;
    }
  }
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  leaveCurrentPageForStats();
  // No-op if the selected orientation matches current settings.
  if (SETTINGS.orientation == orientation) {
    return;
  }

  // Preserve current reading position so we can restore after reflow.
  {
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }

    // Persist the selection so the reader keeps the new orientation on next launch.
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();

    // Update renderer orientation to match the new logical coordinate system.
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

    // Reset section to force re-layout in the new orientation.
    section.reset();
  }
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= std::size(PAGE_TURN_RATES)) {
    automaticPageTurnActive = false;
    return;
  }

  lastPageTurnTime = millis();
  // calculates page turn duration by dividing by number of pages
  pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_RATES[selectedPageTurnOption];
  automaticPageTurnActive = true;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  // resets cached section so that space is reserved for auto page turn indicator when None or progress bar only
  if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
    // Preserve current reading position so we can restore after reflow.
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
  }
}

void EpubReaderActivity::recordPageDwellTime(const bool isForwardTurn) {
  if (remotePositionPending || !SETTINGS.shouldTrackReadingStats()) {
    // Still re-baselined so a mid-session setting toggle doesn't read a stale, far-in-the-past
    // timestamp as this page's dwell time -- but nothing below runs, so this costs one millis()
    // call and nothing else.
    pageDwell.pageShown(static_cast<uint32_t>(millis()));
    return;
  }

  // leavePage() closes the outgoing page and starts timing its replacement in one step, applying
  // the minimum-dwell and idle-threshold rules; 0 means the dwell did not qualify. Calling it
  // again immediately measures zero, which is what lets every navigation path invoke the
  // page-leave hook unconditionally.
  const uint32_t elapsedSeconds = pageDwell.leavePage(static_cast<uint32_t>(millis()));
  if (elapsedSeconds == 0) return;

  sessionReadingSeconds =
      sessionReadingSeconds > UINT32_MAX - elapsedSeconds ? UINT32_MAX : sessionReadingSeconds + elapsedSeconds;
  // Captured here, inside the one place that already knows a dwell qualified, rather than in
  // pageTurn(): this runs for every kind of page exit -- forward turn, backward turn, chapter
  // jump, and the final page via onExit() (which is also the deep-sleep path). Hooking
  // pageTurn(forward) instead, as the upstream reference does, silently loses the last page of
  // every session.
  captureBookOrbitPageEvent(elapsedSeconds);
  if (isForwardTurn) {
    stats.recordForwardPageRead(elapsedSeconds);
    stats.totalPagesTurned++;
    globalStats.totalPagesTurned++;
    sessionPaceSampleSeconds = sessionPaceSampleSeconds <= UINT32_MAX - elapsedSeconds
                                   ? sessionPaceSampleSeconds + elapsedSeconds
                                   : UINT32_MAX;
    if (sessionPaceSampleCount < UINT16_MAX) {
      sessionPaceSampleCount++;
    }
  }
}

void EpubReaderActivity::flushBookOrbitEvents() {
  if (bookOrbitEventCount == 0 || bookOrbitDocumentHash.empty()) return;
  if (BookOrbitStatsQueue::appendBatch(bookOrbitDocumentHash, bookOrbitOwner, bookOrbitEvents, bookOrbitEventCount)) {
    bookOrbitEventCount = 0;
  }
  // On storage failure the buffer is deliberately left populated: the events are still the only
  // copy, and the next flush (or onExit) can retry. captureBookOrbitPageEvent() handles the case
  // where it is still full.
}

void EpubReaderActivity::finalizeReadingStatsSession() {
  // Needs epub (cache path, time-left estimate) and, for the final dwell to mean anything,
  // section. Both are alive at every call site; a defensive check keeps a future one honest.
  if (!SETTINGS.shouldTrackReadingStats() || !epub) return;
  if (!statsSession.beginFinalize()) return;

  // Close out the page still on screen, then commit once for the whole session -- not per page
  // turn (recordPageDwellTime has been accumulating throughout).
  recordPageDwellTime(/*isForwardTurn=*/false);
  ReadingStatsSession::applySessionTotals(sessionReadingSeconds, stats, globalStats);
  refreshEstimatedTimeLeft();
  stats.save(epub->getCachePath());
  globalStats.save();

  // Batched SD write for whatever the session buffer still holds -- never one per page turn
  // (a long session flushes every 32 events; see captureBookOrbitPageEvent()). The dwell
  // recorded just above means the page the user actually stopped on is in this batch, including
  // on the deep-sleep path (onExit() runs synchronously before sleep) and on the manual-sync
  // path (finalized before the early teardown). Nothing is written at all unless capture was
  // enabled, which requires the configured server to be a confirmed BookOrbit instance.
  flushBookOrbitEvents();
}

void EpubReaderActivity::captureBookOrbitPageEvent(const uint32_t dwellSeconds) {
  if (!bookOrbitCaptureEnabled || dwellSeconds == 0) return;
  if (!epub || !section || section->pageCount <= 0) return;

  if (bookOrbitEventCount >= BookOrbitStatsQueue::MAX_EVENTS_PER_BATCH) {
    // The session buffer is full. Persist it now rather than stopping capture for the rest of
    // the session -- a long sitting easily passes 32 pages, and silently dropping everything
    // after that would under-report exactly the sessions worth the most. One SD write per 32
    // page turns is well inside the write-throttling rule (it is rarer than the debounced
    // progress saves already happening on this path).
    flushBookOrbitEvents();
    if (bookOrbitEventCount >= BookOrbitStatsQueue::MAX_EVENTS_PER_BATCH) {
      // The card refused the write. Make room by dropping the oldest buffered event, the same
      // rule the SD queue applies at its own cap: recent reading beats stale backlog.
      memmove(bookOrbitEvents, bookOrbitEvents + 1, (bookOrbitEventCount - 1) * sizeof(bookOrbitEvents[0]));
      bookOrbitEventCount--;
    }
  }

  // Fold in a fresh RTC reading before stamping. This only re-anchors on a large divergence
  // (deep-sleep resume, NTP correction); events already captured keep the timestamps they were
  // stored with, which is what the server's (user, book, device, page, startTime) dedup key
  // needs from a retry.
  uint32_t epochMinutes = 0;
  if (halClock.getEpochMinutes(epochMinutes)) bookOrbitClock.reconcile(epochMinutes, millis());

  uint32_t nowEpochSeconds = 0;
  if (!bookOrbitClock.now(millis(), nowEpochSeconds) || nowEpochSeconds <= dwellSeconds) {
    return;  // no usable absolute time for this event; progress sync is unaffected
  }

  // Overall book position at the END of the page just finished. CrossPoint pages are
  // per-chapter and layout-dependent, so a raw page index would mean nothing to the server;
  // BookOrbitStatsQueue::makeEvent turns this into basis points against a fixed 10000
  // denominator, which reproduces the exact reading fraction on any pagination engine.
  //
  // The chapter fraction uses estimatedTotalPages() alongside pageCount -- the same best-known
  // total the time-left estimate uses -- because pageCount alone is only a build watermark for a
  // partially laid-out giant spine, and on its last built page would read as a finished chapter.
  float chapterProgress = 0.0f;
  if (!BookOrbitStatsQueue::chapterProgressAtEndOfPage(section->currentPage, section->pageCount,
                                                       section->estimatedTotalPages(), chapterProgress)) {
    return;
  }
  const float overall = epub->calculateProgress(currentSpineIndex, chapterProgress);

  BookOrbitStatEvent event;
  if (BookOrbitStatsQueue::makeEvent(nowEpochSeconds, dwellSeconds, overall, event)) {
    bookOrbitEvents[bookOrbitEventCount++] = event;
  }
}

void EpubReaderActivity::refreshEstimatedTimeLeft() {
  stats.estimatedTimeLeftSeconds = 0;
  if (!epub || !section || stats.avgSecondsPerForwardPage == 0 || FINISHED_BOOKS.isFinished(epub->getPath())) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  const int totalPagesInSpine = section->estimatedTotalPages();
  if (bookSize == 0 || totalPagesInSpine <= 0) {
    return;
  }

  const size_t prevChapterSize = currentSpineIndex >= 1 ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
  const size_t cumulativeSize = epub->getCumulativeSpineItemSize(currentSpineIndex);
  const size_t spineSize = cumulativeSize > prevChapterSize ? cumulativeSize - prevChapterSize : 0;
  if (spineSize == 0) {
    return;
  }

  // Pages left in the current chapter come from its real pagination; the rest of the book has no
  // layout yet, so it's estimated at this chapter's own bytes-per-page density -- the same
  // technique Epub::calculateProgress() already uses for byte-weighted book progress.
  const int pagesLeftInChapter = std::max(0, totalPagesInSpine - (section->currentPage + 1));
  const size_t bytesAfterChapter = bookSize > cumulativeSize ? bookSize - cumulativeSize : 0;
  const float pagesPerByte = static_cast<float>(totalPagesInSpine) / static_cast<float>(spineSize);
  const float remainingPages =
      static_cast<float>(pagesLeftInChapter) + static_cast<float>(bytesAfterChapter) * pagesPerByte;

  const float estimatedSeconds = remainingPages * static_cast<float>(stats.avgSecondsPerForwardPage);
  stats.estimatedTimeLeftSeconds = static_cast<uint32_t>(std::min(estimatedSeconds, static_cast<float>(UINT32_MAX)));
}

void EpubReaderActivity::pageTurn(bool isForwardTurn) {
  recordPageDwellTime(isForwardTurn);

  const int oldSpine = currentSpineIndex;
  const int oldPage = section ? section->currentPage : nextPageNumber;

  if (isForwardTurn) {
    // Advance within the section while there are (or may still be) more pages: either a built
    // page ahead, or the section is still building (windowed), in which case more pages exist
    // beyond the current watermark and render()'s ensure-built pump will lay them out. Only when
    // the section is fully built AND we're on its last page do we move to the next spine -- using
    // the live pageCount alone would mistake the build watermark for the end of a giant spine.
    if (section->currentPage < section->pageCount - 1 || section->isBuilding()) {
      section->currentPage++;
    } else {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        currentSpineIndex++;
        section.reset();
      }
    }
  } else {
    if (section->currentPage > 0) {
      section->currentPage--;
    } else if (currentSpineIndex > 0) {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        pendingPageJump = std::numeric_limits<uint16_t>::max();
        currentSpineIndex--;
        section.reset();
      }
    }
  }
  const int newPage = section ? section->currentPage : nextPageNumber;
  if (currentSpineIndex != oldSpine || newPage != oldPage) readingNavigationPending = true;
  lastPageTurnTime = millis();
  requestUpdate();
}

// TODO: Failure handling
void EpubReaderActivity::render(RenderLock&& lock) {
  if (!epub) {
    return;
  }

  // A section build failure (e.g. an invalid/corrupt EPUB that fails XML parsing) leaves the
  // "Indexing" popup on screen with no way forward. Surface an explicit error instead of hanging.
  // clearScreen first so the error popup doesn't overlay the stale "Indexing" popup.
  const auto showBuildError = [this]() {
    renderer.clearScreen();
    GUI.drawPopup(renderer, tr(STR_INDEX_FAILED));
    automaticPageTurnActive = false;
  };
  if (remotePositionPending && remoteBuildFailed) {
    showBuildError();
    return;
  }

  // edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  // based bounds of book, show end of book screen
  if (currentSpineIndex > epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount();
  }

  // Show end of book screen
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    // Sole load site: runs on the render task (serialized by RenderLock); the main
    // task only reads the suggestions once the loaded flag is published
    endOfBookOptions.loadOnce(epub->getPath());
    if (lock.isStale()) return;
    renderer.clearScreen();
    endOfBookOptions.render(renderer, mappedInput);
    if (lock.isStale()) return;
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    return;
  }

  // Apply screen viewable areas and additional padding
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

  // reserves space for automatic page turn indicator when no status bar or progress bar only
  if (automaticPageTurnActive &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    orientedMarginBottom +=
        std::max(SETTINGS.screenMargin,
                 static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin));
  } else {
    orientedMarginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);
  }

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
  // Capture for loop()'s lazy partial-extension start (must match this render's layout params).
  buildViewportWidth = viewportWidth;
  buildViewportHeight = viewportHeight;

  const ReaderRenderSpec renderSpec = SETTINGS.readerRenderSpec(viewportWidth, viewportHeight);

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));
    // Fresh section, fresh chance: a failed lazy extension start in a previous
    // section must not suppress watermark-triggered rebuilds for this one.
    partialRebuildStartFailed = false;

    // A finalized cache serves every page as-is. A partial cache (suspended build from a
    // previous session) serves its pages instantly too, but a build must still run to lay
    // out the rest -- it re-parses from the top in the background (HTML already cached,
    // pages are deterministic) and finalizes, so the partial machinery retires itself.
    const bool cacheLoaded = section->loadSectionFile(renderSpec);
    if (lock.isStale()) return;
    if (cacheLoaded) {
      // Matching render params means identical pagination, so the saved page number is valid
      // as-is: consume any pending settings-change reposition. Without this, a chapter total
      // saved while the section was still building (i.e. a watermark, not the real count)
      // would remap the resume page against the finalized count and teleport the reader.
      cachedChapterTotalPageCount = 0;
    }
    const bool cacheComplete = cacheLoaded && !section->isPartial();
    if (!cacheComplete) {
      if (section->isPartial()) {
        LOG_DBG("ERS", "Partial cache found (%d pages), resuming build...", section->pageCount);
      } else {
        LOG_DBG("ERS", "Cache not found, building...");
      }

      // Jumps that need the final pagination or the anchor map -- explicit page jumps,
      // fragment anchors, percent jumps, and cross-setting progress repositioning -- can't
      // resolve their landing page until the whole chapter is laid out, so they take the full
      // (blocking) build with the indexing popup. Everything else -- plain forward reads, resume,
      // and explicit page jumps -- only needs a specific page, so it builds incrementally to that
      // page and finishes the rest in loop(). The settings-change reposition (cachedChapterTotal*)
      // is NOT a full-build trigger: it's deferred to applyDeferredReposition() once the real page
      // count is known, so it never blocks the first page.
      // Only a percent jump truly needs the whole chapter up front (percent -> page needs the final
      // page count). Anchor jumps (TOC / chapter select / footnotes) resolve incrementally below --
      // the anchor is recorded as its page is laid out, so a chapter-top anchor lands on page 0
      // without indexing the whole chapter.
      const bool needsFullBuild = pendingPercentJump;
      if (needsFullBuild) {
        GUI.drawPopup(renderer, tr(STR_INDEXING));
        // The popup's own refresh is a plain FAST, so force the page that replaces it onto the HALF
        // ghost-cleanup path -- otherwise the "INDEXING" text ghosts under the rendered page.
        pagesUntilFullRefresh = 1;
        // No popup redraws while the framebuffer is lent to the build below;
        // the panel holds the popup displayed above (e-ink is persistent).
        const auto popupFn = [this]() {
          if (renderer.hasFrameBuffer()) GUI.drawPopup(renderer, tr(STR_INDEXING));
        };
        // Lend the framebuffer's 48 KB to the blocking full build; restored
        // (white) at scope exit, and the page render below redraws everything.
        GfxRenderer::FrameBufferLoan loan(renderer);
        if (!section->createSectionFile(renderSpec, popupFn)) {
          LOG_ERR("ERS", "Failed to persist page data to SD");
          section.reset();
          loan.end();  // restore before anything draws
          showBuildError();
          return;
        }
        loan.end();
        if (lock.isStale()) return;
      } else {
        // Lay out just enough to show the landing page; loop() builds the rest behind it. Show the
        // indexing popup up front only when the build will actually be slow: a large spine (its
        // whole HTML must be inflated before page 1 can lay out -- the giant single-spine case), or
        // a deep resume/jump that must lay out many pages to reach the landing page. Tiny sections
        // build in a blink and stay popup-free.
        const int target = pendingPageJump.has_value() ? *pendingPageJump : (nextPageNumber < 0 ? 0 : nextPageNumber);
        const bool anchorJump = !pendingAnchor.empty();

        // Landing well inside a partial: the page (or anchor, via the on-disk map) is already
        // servable, so don't restart the extension build now -- it re-lays out the WHOLE chapter
        // from page 0 (minutes of background CPU + SD writes on a giant spine), pure waste when
        // the reader never nears the watermark this session. loop() starts it lazily once the
        // reader is within PARTIAL_REBUILD_START_MARGIN pages of the watermark.
        if (!remotePositionPending && section->isPartial() &&
            (anchorJump ? section->getPageForAnchor(pendingAnchor).has_value()
                        : target + PARTIAL_REBUILD_START_MARGIN < static_cast<int>(section->pageCount))) {
          LOG_DBG("ERS", "Partial covers target %d of %d; deferring extension build", target, section->pageCount);
        } else {
          const size_t spineBytes =
              epub->getCumulativeSpineItemSize(currentSpineIndex) -
              (currentSpineIndex > 0 ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0);
          // Popup only when the build will actually be slow: a big spine whose HTML still needs
          // inflating (the multi-second cost), or a deep page target. A reopen with cached HTML builds
          // fast, so no popup -- that's what made an already-indexed book look like it was reindexing.
          // A partial cache that already covers the target page shows it instantly: never popup.
          const bool willInflate = !section->hasHtmlCache();
          bool showPopup;
          if (anchorJump) {
            // An anchor jump's cost is bounded by the anchor's page, not `target`. An anchor already
            // in the on-disk map (partial or finalized cache) lands instantly: no popup. Otherwise it
            // lies beyond the indexed watermark and the build may lay out the whole spine to find it,
            // so gate on spine size alone -- laying out a big spine takes seconds even with cached
            // HTML. Ordinary chapter-top TOC jumps resolve on page 0 and stay popup-free.
            showPopup = !section->findAnchor(pendingAnchor).has_value() && spineBytes > BUILD_POPUP_BYTE_THRESHOLD;
          } else {
            const bool targetAvailable = target < static_cast<int>(section->pageCount);
            showPopup = !targetAvailable && ((spineBytes > BUILD_POPUP_BYTE_THRESHOLD && willInflate) ||
                                             target > BUILD_POPUP_PAGE_THRESHOLD);
          }
          if (showPopup) {
            GUI.drawPopup(renderer, tr(STR_INDEXING));
            // HALF-clear the popup when the page replaces it, else "INDEXING" ghosts under the page.
            pagesUntilFullRefresh = 1;
          }
          // Mid-build popup surfacing for slow builds the predictive gates can't
          // see (image extraction/probing inside a single page, or any chunk
          // overrunning the deadline). The parser fires the callback before the
          // first image probe; buildPopupPending gates it to this blocking phase
          // so a background build in loop() can never draw over a displayed page.
          buildPopupPending = !showPopup;
          const unsigned long buildStartMs = millis();
          bool started;
          {
            // Lend the framebuffer's 48 KB to startBuild only (the spine HTML
            // inflation peak). The chunk loop below runs without it so the popup
            // can draw mid-build; background chunks never had the loan either.
            GfxRenderer::FrameBufferLoan loan(renderer);
            started = section->startBuild(renderSpec, [this] { showBuildPopup(); });
          }
          if (lock.isStale()) {
            buildPopupPending = false;
            return;
          }
          if (!started) {
            LOG_ERR("ERS", "Failed to start section build");
            section.reset();
            buildPopupPending = false;
            showBuildError();
            return;
          }
          while (!remotePositionPending && !section->isBuildComplete() &&
                 (anchorJump ? !section->findAnchor(pendingAnchor) : static_cast<int>(section->pageCount) <= target)) {
            // Anchor jump: build until the anchor's page is laid out (usually page 0), checking a
            // partial's on-disk anchor map too so an already-indexed anchor resolves immediately.
            // Otherwise: build until the target page exists. loop() builds the rest behind it.
            if (buildPopupPending && millis() - buildStartMs >= BUILD_POPUP_DEADLINE_MS) {
              // The predictive gates guessed fast but the build blew the silent budget.
              showBuildPopup();
            }
            const bool built = section->buildSomeMore(BUILD_PAGES_PER_CHUNK);
            if (lock.isStale()) {
              buildPopupPending = false;
              return;
            }
            if (!built) {
              LOG_ERR("ERS", "Failed during incremental section build");
              section.reset();
              buildPopupPending = false;
              showBuildError();
              return;
            }
          }
          buildPopupPending = false;
        }
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
    }

    if (pendingPageJump.has_value()) {
      section->currentPage = *pendingPageJump;
      pendingPageJump.reset();
    } else {
      section->currentPage = nextPageNumber;
      if (section->currentPage < 0) {
        section->currentPage = 0;
      }
    }

    if (!pendingAnchor.empty()) {
      // Resolve from the pages laid out so far and/or the on-disk map (finalized or partial).
      const auto page = section->findAnchor(pendingAnchor);
      if (page) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      } else {
        LOG_DBG("ERS", "Anchor '%s' not found in section %d", pendingAnchor.c_str(), currentSpineIndex);
      }
      pendingAnchor.clear();
    }

    if (pendingPercentJump && section->pageCount > 0) {
      // Apply the pending percent jump now that we know the new section's page count.
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) {
        newPage = section->pageCount - 1;
      }
      section->currentPage = newPage;
      pendingPercentJump = false;
    }
  }

  // Extend the build to the requested page if needed (for partials and in-progress builds).
  // This runs every render, so it covers both the first page and any forward turn that gets
  // ahead of the background builder; pages already built do no work here.
  //
  // Crossing a partial's watermark before the extension rebuild has caught up means a
  // synchronous wait spanning the remaining prefix re-layout -- potentially tens of
  // seconds on a giant spine. Show the indexing popup so it isn't a silent freeze
  // (the page that replaces it takes the HALF ghost-cleanup path). Ordinary window
  // catch-ups on a non-partial build are a page or two and stay popup-free.
  if (section->isPartial() && section->currentPage >= static_cast<int>(section->pageCount)) {
    GUI.drawPopup(renderer, tr(STR_INDEXING));
    pagesUntilFullRefresh = 1;
  }
  while (!remotePositionPending && section->isPartial() &&
         section->currentPage >= static_cast<int>(section->pageCount)) {
    // Start a build to extend a partial toward the requested page.
    if (!section->isBuilding()) {
      const bool started = section->startBuild(renderSpec);
      if (lock.isStale()) return;
      if (!started) {
        LOG_ERR("ERS", "Failed to start partial extension build");
        section.reset();
        showBuildError();
        return;
      }
    }
    // Extend until either the target page exists or the build completes.
    while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
      const bool built = section->buildSomeMore(BUILD_PAGES_PER_CHUNK);
      if (lock.isStale()) return;
      if (!built) {
        LOG_ERR("ERS", "Failed during incremental section build");
        section.reset();
        showBuildError();
        return;
      }
    }
  }
  // For an in-progress incremental build, make sure the page we're about to show has been laid out.
  if (!remotePositionPending && section->isBuilding()) {
    while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
      const bool built = section->buildSomeMore(BUILD_PAGES_PER_CHUNK);
      if (lock.isStale()) return;
      if (!built) {
        LOG_ERR("ERS", "Failed during incremental section build");
        section.reset();
        showBuildError();
        return;
      }
    }
  }

  // The requested page is now as built as it will get. If it still lands past the end,
  // clamp to the last real page: the UINT16_MAX "last page" sentinel from backward chapter
  // navigation, an explicit jump beyond a finished chapter, or a stale saved position.
  // Guarded on !isBuilding() because a still-building section's pageCount is only the current
  // watermark (not the final count) and has already been driven far enough by the loops above.
  if (!section->isBuilding() && section->pageCount > 0 &&
      section->currentPage >= static_cast<int>(section->pageCount)) {
    section->currentPage = section->pageCount - 1;
  }

  // Apply a deferred settings-change reposition now that the real page count is known (a no-op for
  // a plain resume / unchanged pagination). If still building, this defers to loop() on completion.
  applyDeferredReposition();

  if (remotePositionPending) {
    GUI.drawPopup(renderer, tr(STR_INDEXING));
    return;
  }

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    return;
  }

  updateBookmarkFlag();

  {
    // Unified page read: the in-progress build's in-RAM table if it has reached the page,
    // otherwise the on-disk file (finalized section, or a partial from a previous session).
    auto p = section->loadPage(section->currentPage);
    if (lock.isStale()) return;
    if (!p) {
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache");
      automaticPageTurnActive = false;
      // Retrying rebuilds a transiently corrupt section and usually recovers, but a page that keeps
      // failing would loop forever on a blank screen, so bound the retries before giving up.
      const bool giveUp = ++pageLoadRetryCount > MAX_PAGE_LOAD_RETRIES;
      // Abandon (not suspend) any active build BEFORE clearing: clearCache deletes the files,
      // and the destructor's suspend would otherwise commit tables into a deleted handle.
      section->abandonBuild();
      section->clearCache();
      section.reset();
      if (giveUp) {
        LOG_ERR("ERS", "Page load retry limit reached, aborting");
        pageLoadRetryCount = 0;  // Reset so a later user-initiated navigation can try afresh
        renderer.clearScreen();
        renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
        renderer.displayBuffer();
        return;
      }
      requestUpdate();  // Try again after clearing cache
      return;
    }
    pageLoadRetryCount = 0;  // Reset the retry counter once a page loads cleanly

    // Collect footnotes from the loaded page
    currentPageFootnotes = std::move(p->footnotes);
    currentPageLoaded = true;

    const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft,
                   lock);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
    if (lock.isStale()) return;
    lastRenderCompleteMs = millis();
  }
  if (lock.isStale()) return;
  // Only count a genuine position change. render() also runs on menu, bookmark and screenshot
  // re-renders; estimatedTotalPages() (not pageCount) is compared here to match what's actually
  // stored below -- comparing pageCount instead would spuriously see "changed" on nearly every
  // tick while a section is background-building (pageCount lags the estimate until it finalizes).
  const int estimatedPageCount = section->estimatedTotalPages();
  const bool logicalPositionChanged =
      currentSpineIndex != lastObservedSpineIndex || section->currentPage != lastObservedPage;
  if (logicalPositionChanged || estimatedPageCount != lastObservedPageCount) {
    lastObservedSpineIndex = currentSpineIndex;
    lastObservedPage = section->currentPage;
    lastObservedPageCount = estimatedPageCount;
    // Footnote navigation is not real reading progress -- see flushPendingProgress()'s
    // footnote-exit handling, which persists the pre-footnote origin instead.
    if (ReadingPositionPolicy::shouldStamp(logicalPositionChanged, readingNavigationPending, footnoteDepth != 0)) {
      markReadingPositionChanged();
    }
    if (footnoteDepth == 0) {
      progressDebounce.notePositionChanged(millis());
    }
  }
  if (footnoteDepth == 0) readingNavigationPending = false;
  if (footnoteDepth == 0 && progressDebounce.isFlushDue(millis())) {
    if (saveProgress(currentSpineIndex, section->currentPage, estimatedPageCount)) {
      progressDebounce.markFlushed();
    }
  }

  if (lock.isStale()) return;

  if (pendingScreenshot) {
    if (lock.isStale()) return;
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }

  if (showBookmarkMessage) {
    GUI.drawPopup(renderer, bookmarkRemoved ? tr(STR_BOOKMARK_REMOVED) : tr(STR_BOOKMARK_ADDED));
  }

  if (showDictionaryMessage) {
    GUI.drawPopup(renderer, tr(STR_DICT_NO_DICT_SET));
  }

  if (showKoSyncSuccess) {
    GUI.drawPopup(renderer, tr(STR_ALREADY_SYNCED));
  }
}

bool EpubReaderActivity::applyDeferredReposition() {
  if (cachedChapterTotalPageCount == 0 || !section || section->isBuilding()) {
    return false;
  }
  bool changed = false;
  // Only remap when the chapter actually re-paginated (e.g. after a settings change). A plain
  // resume has identical pagination, so section->pageCount == cachedChapterTotalPageCount and
  // nothing moves.
  if (currentSpineIndex == cachedSpineIndex && section->pageCount != cachedChapterTotalPageCount) {
    const float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
    int newPage = static_cast<int>(progress * static_cast<float>(section->pageCount));
    if (newPage < 0) newPage = 0;
    if (section->pageCount > 0 && newPage >= static_cast<int>(section->pageCount)) {
      newPage = section->pageCount - 1;
    }
    if (newPage != section->currentPage) {
      section->currentPage = newPage;
      changed = true;
    }
  }
  cachedChapterTotalPageCount = 0;  // consumed; don't read cached progress again
  return changed;
}

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  return EpubReaderUtils::saveProgress(*epub, spineIndex, currentPage, pageCount, progressEventTimestamp,
                                       progressIsLegacy);
}

void EpubReaderActivity::markReadingPositionChanged() {
  progressIsLegacy = false;
  uint64_t timestamp = 0;
  if (!halClock.getEpochSeconds(timestamp)) timestamp = 0;
  progressEventTimestamp = ReadingPositionPolicy::timestampAfterChange(progressEventTimestamp, timestamp);
  if (!ProgressTimestamp::valid(progressEventTimestamp)) {
    LOG_INF("ERS", "Reading position changed without a trustworthy RTC timestamp");
  }
}

void EpubReaderActivity::ensureCurrentPageLoaded() {
  if (currentPageLoaded) return;
  // Runs on the main task (menu open / footnote shortcut), shortly after a Quick Resume
  // skip-render leaves currentPageLoaded false. renderContents() (render task) can be mid-flight
  // setting the same currentPageFootnotes/currentPageLoaded concurrently -- take the same lock it
  // runs under so the two never touch the vector at once.
  // Safe only because both current call sites run directly in loop()'s own body (not from a render
  // task or from an ActivityResult callback dispatched under RenderLock, e.g. the PendingAction::Pop
  // handling in ActivityManager::loop()) -- ActivityManager::loop() never holds RenderLock when it
  // calls Activity::loop() (see its "do not hold a lock here" comment), so this wait=true take can't
  // re-enter a lock this same task already holds. A future caller reached from inside a RenderLock
  // scope would deadlock; keep this function called only from loop()'s own body.
  RenderLock lock(true, false);
  if (currentPageLoaded || !section) return;
  if (const auto p = section->loadPage(section->currentPage)) {
    currentPageFootnotes = std::move(p->footnotes);
  }
  currentPageLoaded = true;
}

bool EpubReaderActivity::tryQuickResumeWithoutRender() {
  if (!epub || currentSpineIndex < 0 || currentSpineIndex >= epub->getSpineItemsCount()) return false;

  // Match render()'s own viewport computation exactly, or the section-cache header check below
  // spuriously mismatches against a spec built with different margins.
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;
  orientedMarginBottom +=
      std::max(SETTINGS.screenMargin, static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight()));
  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
  const ReaderRenderSpec renderSpec = SETTINGS.readerRenderSpec(viewportWidth, viewportHeight);

  auto candidateSection = makeUniqueNoThrow<Section>(epub, currentSpineIndex, renderer);
  if (!candidateSection) {
    LOG_ERR("ERS", "OOM: Section (Quick Resume probe)");
    return false;
  }
  const bool cacheLoaded = candidateSection->loadSectionFile(renderSpec);
  const bool cacheIsPartial = cacheLoaded && candidateSection->isPartial();
  const int cachedPageCount = cacheLoaded ? static_cast<int>(candidateSection->pageCount) : 0;

  // cleanBaseViewAtSleep is hardcoded true here: the caller (onEnter()) only reaches this method
  // when skipInitialRender is already true, which itself required a clean base view at sleep --
  // see EpubReaderActivity::isQuickResumeEligibleBaseView() and main.cpp's enterDeepSleep().
  if (!isQuickResumeEligible(/*cleanBaseViewAtSleep=*/true, cacheLoaded, cacheIsPartial, nextPageNumber,
                             cachedPageCount)) {
    return false;
  }

  section = std::move(candidateSection);
  section->currentPage = nextPageNumber;
  partialRebuildStartFailed = false;
  // Matches render()'s cacheLoaded branch: matching render params means identical pagination, so
  // the saved page is valid as-is -- consume any pending settings-change reposition.
  cachedChapterTotalPageCount = 0;
  buildViewportWidth = viewportWidth;
  buildViewportHeight = viewportHeight;
  // Deferred: footnotes/page metadata load lazily on first real need (see ensureCurrentPageLoaded).
  currentPageLoaded = false;
  lastObservedSpineIndex = currentSpineIndex;
  lastObservedPage = section->currentPage;
  lastObservedPageCount = section->estimatedTotalPages();
  return true;
}

bool EpubReaderActivity::flushPendingProgress() {
  if (remotePositionPending) {
    APP_STATE.quickResumeSkipRenderEligible = false;
    APP_STATE.saveToFile();
    return true;
  }
  if (!epub || !section) return true;
  if (footnoteDepth > 0) {
    // Mid-footnote: currentSpineIndex/section->currentPage are the footnote's own location, not
    // real reading progress -- persist the pre-footnote origin so the book reopens at the link,
    // not inside the footnote. pageCount=0 (unknown) matches this path's pre-existing behavior:
    // section belongs to the footnote's chapter here, not the origin's, so its pageCount can't be
    // used for the origin without an extra cache lookup.
    const SavedPosition& origin = savedPositions[0];
    return saveProgress(origin.spineIndex, origin.pageNumber, 0);
  }
  if (!progressDebounce.hasPendingChange()) return true;
  if (saveProgress(currentSpineIndex, section->currentPage, section->estimatedTotalPages())) {
    progressDebounce.markFlushed();
    return true;
  }
  return false;
}
void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft, const RenderLock& lock) {
  const auto t0 = millis();
  const int fontId = SETTINGS.getReaderFontId();
  const PageRenderCancellation cancellation{renderWasSuperseded, &lock};

  // The image pixel-cache RAM slot lives for exactly one page render (it feeds
  // the BW double-refresh and every grayscale band pass); release it on every
  // exit so nothing stays resident across page turns.
  struct PxcSlotGuard {
    ~PxcSlotGuard() { ImageBlock::releaseRenderCache(); }
  } pxcSlotGuard;

  // Font prewarm: scan pass accumulates text, then prewarm, then real render.
  // renderStatusBar() is scanned here too, inside this SAME scope (never a
  // nested PrewarmScope -- the constructor unconditionally resets scan state,
  // so a second scope here would wipe the body text just accumulated). This
  // batches status-bar text (chapter/book title, which can resolve to the
  // same SD-card CJK font as the body via resolveTextFontId()) into the same
  // per-font-id prewarm pass as the body, instead of leaving it to load
  // on-demand mid-draw later. FontCacheManager::recordText batches by the
  // RESOLVED font id each drawText() call reports, so this is safe even when
  // the status bar's UI font and the body font differ. The call only draws
  // text/lines/rects/icons -- no image decode -- so it's cheap and orientation
  // is already handled inside GUI.drawStatusBar via the renderer's oriented
  // viewable-area helpers, same as the real (non-scan) call to this function
  // later in this method.
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  if (!page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop, &cancellation)) {
    scope.cancel();
    return;
  }
  renderStatusBar();
  if (!scope.endScanAndPrewarm(renderWasSuperseded, &lock) || lock.isStale()) return;
  const auto tPrewarm = millis();

  const bool pageHasImages = page->hasImages();
  const bool pageHasImagesNeedingDecode = pageHasImages && page->hasImagesNeedingDecode();
  const bool manualRefreshPending = forcedRefreshPending;
  forcedRefreshPending = false;
  // Read only -- NOT cleared here. Several lock.isStale() early-returns sit between this read and
  // where the flag is actually used below; clearing eagerly would drop the obligation if this
  // render gets superseded, letting a later retry wrongly qualify for FAST_REFRESH while residue
  // is still on the panel (regression of the #2190 fix). Cleared only once actually consumed (see
  // the non-image branch below); the image branch unconditionally re-arms it regardless.
  const bool imageResidueCleanupDue = imageResidueCleanupPending;
  // The reader starts with zero here, which means the normal refresh cycle
  // would use a HALF refresh for its first page. Keep that same clean base for
  // image pages: their double-FAST path otherwise runs directly over the
  // retained frame after a silent restart (for example, when returning from
  // KOReader sync), leaving the old UI mixed with the image.
  const bool cleanImageBasePending = manualRefreshPending || pagesUntilFullRefresh <= 1;
  const auto& fontMap = renderer.getFontMap();
  const auto readerFont = fontMap.find(fontId);
  const EpdFontData* readerFontData =
      readerFont != fontMap.end() ? readerFont->second.getData(EpdFontFamily::REGULAR) : nullptr;
  const bool readerFontSupportsGrayscale = readerFontData && readerFontData->is2Bit;
  // A 1-bit font has no grayscale coverage planes. Running the AA pipeline
  // would redraw the same black mask twice and submit an expensive gray
  // waveform without improving the glyphs.
  const bool needsTextGrayscale = SETTINGS.textAntiAliasing && readerFontSupportsGrayscale;
  const bool needsAnyGrayscale = needsTextGrayscale || pageHasImages;
  const bool tiledGrayscale = needsAnyGrayscale && renderer.supportsStripGrayscale();
  // Whole-plane buffering only pays when the BW refresh genuinely runs async
  // underneath it; on blocking panels (X3) it would just spend ~50 KB for the
  // identical serial timing. Image pages take the blocking double-FAST path
  // below (no async refresh is ever started), so they'd spend the buffers with
  // nothing in flight to overlap.
  const bool overlapRefresh = tiledGrayscale && renderer.supportsAsyncRefresh() && !pageHasImages;
  auto renderGrayscalePass = [&]() -> bool {
    if (needsTextGrayscale) {
      return page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop, &cancellation);
    }
    return page->renderImages(renderer, fontId, orientedMarginLeft, orientedMarginTop, &cancellation);
  };

  if (pageHasImagesNeedingDecode) {
    if (!page->renderWithImagePlaceholders(renderer, fontId, orientedMarginLeft, orientedMarginTop, &cancellation)) {
      return;
    }
    renderStatusBar();
    if (lock.isStale()) return;
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    if (lock.isStale()) return;
    renderer.clearScreen();
  }

  if (!page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop, &cancellation)) return;
  renderStatusBar();
  if (lock.isStale()) return;
  const auto tBwRender = millis();

  if (pageHasImages) {
    // Double FAST_REFRESH with selective image blanking (pablohc's technique):
    // HALF_REFRESH sets particles too firmly for the grayscale LUT to adjust.
    // Instead, blank only the image area and do two fast refreshes.
    // Step 1: Display page with image area blanked (text appears, image area white)
    // Step 2: Re-render with images and display again (images appear clean)
    int16_t imgX, imgY, imgW, imgH;
    if (page->getImageBoundingBox(imgX, imgY, imgW, imgH)) {
      // Image pages intentionally bypass the regular refresh cadence. Preserve
      // a pending clean base before their double-FAST grayscale pipeline.
      if (cleanImageBasePending) {
        if (lock.isStale()) return;
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      }
      if (lock.isStale()) return;
      renderer.fillRect(imgX + orientedMarginLeft, imgY + orientedMarginTop, imgW, imgH, false);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);

      // Re-render page content to restore images into the blanked area
      // Status bar is not re-rendered here to avoid reading stale dynamic values (e.g. battery %)
      if (!page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop, &cancellation)) return;
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    // The image's own page is handled above and doesn't count toward the full
    // refresh cadence. But the grayscale pass below leaves gray charge in the
    // image region that a plain fast diff on the *next* page can't clear, so
    // text there ghosts gray (#2190). Force the next ordinary page onto the
    // HALF ghost-cleanup path, which drives every pixel to its target
    // regardless of residue.
    pagesUntilFullRefresh = 1;
    imageResidueCleanupPending = true;
  } else {
    // Async form: start the waveform and return so the grayscale plane rendering
    // below overlaps the panel's refresh time instead of following it.
    if (lock.isStale()) return;
    const bool eligibleForNoFlashMaintenance = isEligibleForNoFlashMaintenance(
        pageHasImages, needsTextGrayscale, manualRefreshPending, imageResidueCleanupDue);
    imageResidueCleanupPending = false;  // consumed: the refresh mode below now reflects it
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh, consecutiveNoFlashMaintenance, overlapRefresh,
                                         eligibleForNoFlashMaintenance);
  }
  const auto tDisplay = millis();

  // Tiled grayscale: render each plane band-by-band, leaving the BW
  // framebuffer intact so no full-frame storeBwBuffer is needed; controller
  // RAM is re-synced from the live framebuffer afterward. The page is
  // re-rendered ceil(H/STRIP_ROWS) times per plane, but renderCharImpl culls
  // out-of-band glyphs before decode so the cost stays close to one render.
  // Both text (drawPixel) and images (DirectPixelWriter) honor the active
  // strip target. When the BW refresh above went out async, the plane
  // rendering below overlaps the panel's refresh time; only the controller
  // RAM writes wait for BUSY.
  if (tiledGrayscale) {
    constexpr int STRIP_ROWS = 80;
    const int gh = renderer.getDisplayHeight();
    const int gwBytes = renderer.getDisplayWidthBytes();
    const size_t planeBytes = static_cast<size_t>(gwBytes) * gh;

    // Render one plane band-by-band into a whole-plane buffer without touching
    // the controller, so it can run while the refresh is still in flight.
    auto renderPlaneToBuffer = [&](const bool lsbPlane, uint8_t* buf) -> bool {
      renderer.setRenderMode(lsbPlane ? GfxRenderer::GRAYSCALE_LSB : GfxRenderer::GRAYSCALE_MSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        if (lock.isStale()) return false;
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(buf + static_cast<size_t>(y) * gwBytes, y, rows);
        renderer.clearScreen(0x00);
        const bool rendered = renderGrayscalePass();
        renderer.endStripTarget();
        if (!rendered) return false;
      }
      return true;
    };

    // Tiered on heap pressure: two plane buffers hide both plane renders
    // inside the refresh wait; one hides the LSB render (its buffer is reused
    // for MSB after streaming); none falls back to the strip-scratch flow with
    // no overlap. Each buffer is only attempted when it leaves ~60 KB free so
    // the pass never starves concurrent allocations: the next page re-render
    // allocates through throwing std::string paths that abort() on OOM under
    // -fno-exceptions, so a plane buffer that "fits" but eats the render
    // headroom is worse than the strip fallback. Blocking panels skip the
    // buffers entirely (nothing to overlap).
    constexpr size_t PLANE_BUF_HEADROOM = 60000;
    // Free-heap alone ignores fragmentation: taking the largest block for a
    // plane can leave only slivers behind even when total headroom looks fine.
    // Require the block to fit the plane with 16 KB contiguous to spare, which
    // also keeps the advance-table batch scratch viable mid-render (same
    // rationale as BACKGROUND_BUILD_MIN_MAX_ALLOC).
    constexpr size_t PLANE_BUF_MAX_ALLOC_RESERVE = 16 * 1024;
    const auto planeBufFits = [planeBytes] {
      return ESP.getFreeHeap() >= planeBytes + PLANE_BUF_HEADROOM &&
             ESP.getMaxAllocHeap() >= planeBytes + PLANE_BUF_MAX_ALLOC_RESERVE;
    };
    auto lsbPlaneBuf = (overlapRefresh && planeBufFits()) ? makeUniqueNoThrow<uint8_t[]>(planeBytes) : nullptr;
    auto msbPlaneBuf = (lsbPlaneBuf && planeBufFits()) ? makeUniqueNoThrow<uint8_t[]>(planeBytes) : nullptr;

    if (lsbPlaneBuf) {
      if (!renderPlaneToBuffer(true, lsbPlaneBuf.get())) {
        renderer.setRenderMode(GfxRenderer::BW);
        return;
      }
      if (msbPlaneBuf && !renderPlaneToBuffer(false, msbPlaneBuf.get())) {
        renderer.setRenderMode(GfxRenderer::BW);
        return;
      }
      const auto tGrayRender = millis();

      renderer.waitRefreshComplete();
      if (lock.isStale()) {
        renderer.setRenderMode(GfxRenderer::BW);
        return;
      }
      const auto tWait = millis();

      renderer.writeGrayscalePlaneStrip(true, lsbPlaneBuf.get(), 0, gh);
      if (msbPlaneBuf) {
        renderer.writeGrayscalePlaneStrip(false, msbPlaneBuf.get(), 0, gh);
      } else {
        if (!renderPlaneToBuffer(false, lsbPlaneBuf.get())) {
          renderer.setRenderMode(GfxRenderer::BW);
          return;
        }
        renderer.writeGrayscalePlaneStrip(false, lsbPlaneBuf.get(), 0, gh);
      }
      const auto tGrayWrite = millis();

      renderer.setRenderMode(GfxRenderer::BW);
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();

      // BW framebuffer is intact; re-sync controller RAM for the next
      // differential page turn directly from it.
      renderer.cleanupGrayscaleWithFrameBuffer();
      const auto tEnd = millis();

      READER_PERF_LOG("ERS",
                      "Page render (tiled async): prewarm=%lums bw_render=%lums display=%lums gray_render=%lums "
                      "wait=%lums gray_write=%lums gray_display=%lums cleanup=%lums total=%lums (planes buffered: %d)",
                      tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayRender - tDisplay,
                      tWait - tGrayRender, tGrayWrite - tWait, tGrayDisplay - tGrayWrite, tEnd - tGrayDisplay,
                      tEnd - t0, msbPlaneBuf ? 2 : 1);
    } else {
      // Per-strip scratch tier: blocking panels (X3) and the OOM fallback.
      // The strip writes below need the panel idle, so wait out any pending
      // async refresh first (no-op on blocking panels).
      auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS);
      renderer.waitRefreshComplete();
      if (!scratch) {
        LOG_ERR("ERS", "OOM: grayscale strip scratch (%d bytes); skipping AA this page", gwBytes * STRIP_ROWS);
        if (overlapRefresh) {
          // The BW refresh ran the shadow-free async path, so controller RAM's
          // differential baseline was never rebuilt. Even with AA skipped it must
          // be re-synced from the intact BW framebuffer, or the next differential
          // update diffs against stale contents.
          renderer.cleanupGrayscaleWithFrameBuffer();
        }
      } else {
        // Bands may be streamed in any order: X4 windows each via setRamArea,
        // X3 via PTL.
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
        for (int y = 0; y < gh; y += STRIP_ROWS) {
          if (lock.isStale()) {
            renderer.setRenderMode(GfxRenderer::BW);
            return;
          }
          const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
          renderer.beginStripTarget(scratch.get(), y, rows);
          renderer.clearScreen(0x00);
          const bool rendered = renderGrayscalePass();
          renderer.endStripTarget();
          if (!rendered) {
            renderer.setRenderMode(GfxRenderer::BW);
            return;
          }
          renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
        }
        const auto tGrayLsb = millis();

        // MSB plane.
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
        for (int y = 0; y < gh; y += STRIP_ROWS) {
          if (lock.isStale()) {
            renderer.setRenderMode(GfxRenderer::BW);
            return;
          }
          const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
          renderer.beginStripTarget(scratch.get(), y, rows);
          renderer.clearScreen(0x00);
          const bool rendered = renderGrayscalePass();
          renderer.endStripTarget();
          if (!rendered) {
            renderer.setRenderMode(GfxRenderer::BW);
            return;
          }
          renderer.writeGrayscalePlaneStrip(false, scratch.get(), y, rows);
        }
        const auto tGrayMsb = millis();

        renderer.setRenderMode(GfxRenderer::BW);
        renderer.displayGrayBuffer();
        const auto tGrayDisplay = millis();

        // BW framebuffer is intact; re-sync controller RAM for the next
        // differential page turn directly from it.
        renderer.cleanupGrayscaleWithFrameBuffer();
        const auto tCleanup = millis();

        const auto tEnd = millis();
        READER_PERF_LOG("ERS",
                        "Page render (tiled): prewarm=%lums bw_render=%lums display=%lums gray_lsb=%lums "
                        "gray_msb=%lums gray_display=%lums cleanup=%lums total=%lums",
                        tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayLsb - tDisplay,
                        tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tCleanup - tGrayDisplay, tEnd - t0);
      }
    }
  } else {
    // Fallback path for a controller without strip support. grayscale rendering
    // TODO: Only do this if font supports it
    if (needsAnyGrayscale) {
      // Save the BW frame before the grayscale passes overwrite it, restore
      // after. Only needed when grayscale actually renders.
      if (!renderer.storeBwBuffer()) {
        LOG_ERR("ERS", "Failed to store BW buffer for grayscale render; skipping grayscale this page");
        const auto tEnd = millis();
        READER_PERF_LOG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
                        tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
        return;
      }
      const auto tBwStore = millis();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      if (!renderGrayscalePass()) {
        renderer.setRenderMode(GfxRenderer::BW);
        renderer.restoreBwBuffer();
        return;
      }
      renderer.copyGrayscaleLsbBuffers();
      const auto tGrayLsb = millis();

      // Render and copy to MSB buffer
      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      if (!renderGrayscalePass()) {
        renderer.setRenderMode(GfxRenderer::BW);
        renderer.restoreBwBuffer();
        return;
      }
      renderer.copyGrayscaleMsbBuffers();
      const auto tGrayMsb = millis();

      // display grayscale part
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();
      renderer.setRenderMode(GfxRenderer::BW);
      renderer.restoreBwBuffer();
      const auto tBwRestore = millis();

      const auto tEnd = millis();
      READER_PERF_LOG("ERS",
                      "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
                      "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
                      tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay,
                      tGrayLsb - tBwStore, tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay,
                      tEnd - t0);
    } else {
      // No text AA and no images: BW frame already displayed above, no grayscale
      // to render, so no save/restore.
      const auto tEnd = millis();
      READER_PERF_LOG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
                      tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
    }
  }
}

void EpubReaderActivity::renderStatusBar() const {
  // Calculate progress in book. Use the estimated total while a giant spine is still building so
  // "page X of Y" and the progress bar don't read off the small build watermark.
  const int currentPage = section->currentPage + 1;
  const float pageCount = section->estimatedTotalPages();
  const float sectionChapterProg = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) : 0;
  const float bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100;

  std::string title;

  int textYOffset = 0;
  const auto sb = SETTINGS.statusBarSpec();

  if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);

    // calculates textYOffset when rendering title in status bar
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

    // offsets text if no status bar or progress bar only
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }

  } else if (sb.titleMode == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_UNNAMED);
    const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    if (tocIndex != -1) {
      const auto tocItem = epub->getTocItem(tocIndex);
      title = tocItem.title;
    }

  } else if (sb.titleMode == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub->getTitle();
  }

  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset, true, currentPageBookmarked,
                    section->isBuilding());
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  leaveCurrentPageForStats();
  if (!epub) return;

  // Push current position onto saved stack
  if (savePosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, section->currentPage);
  }

  // Extract fragment anchor (e.g. "#note1" or "chapter2.xhtml#note1")
  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  // Check for same-file anchor reference (#anchor only)
  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';

  int targetSpineIndex;
  if (sameFile) {
    targetSpineIndex = currentSpineIndex;
  } else {
    targetSpineIndex = epub->resolveHrefToSpineIndex(hrefStr);
  }

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth > 0) footnoteDepth--;  // undo push
    return;
  }

  {
    RenderLock lock(*this);
    if (!savePosition) readingNavigationPending = true;
    pendingAnchor = std::move(anchor);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    section.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  leaveCurrentPageForStats();
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock(*this);
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::loadCachedBookmarks() {
  cachedBookmarks.clear();
  if (cachedBookmarks.capacity() < initialBookmarkCacheCapacity) {
    cachedBookmarks.reserve(initialBookmarkCacheCapacity);
  }
  if (!epub) {
    currentPageBookmarked = false;
    return;
  }

  BookmarkFile::load(epub->getPath(), cachedBookmarks);
  updateBookmarkFlag();
}

void EpubReaderActivity::addBookmark() {
  if (!section || !epub) {
    return;
  }
  LOG_DBG("ERS", "Toggle bookmark at spine %d, page %d", currentSpineIndex, section ? section->currentPage : -1);
  int currentPage;
  int pageCount;
  {
    RenderLock lock(*this);
    pageCount = section->estimatedTotalPages();
    currentPage = section->currentPage;
  }

  SavedProgressPosition progress = ProgressMapper::toSavedProgress(epub, getCurrentPosition());
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, currentPage, pageCount);

  const size_t bookmarkCountBeforeToggle = cachedBookmarks.size();
  cachedBookmarks.erase(std::remove_if(cachedBookmarks.begin(), cachedBookmarks.end(),
                                       [&](const BookmarkEntry& b) {
                                         return bookmarkMatchesProgress(b, currentSpineIndex, currentPage, pageCount,
                                                                        pageRange);
                                       }),
                        cachedBookmarks.end());
  if (cachedBookmarks.size() != bookmarkCountBeforeToggle) {
    bookmarkRemoved = true;
    currentPageBookmarked = false;
  } else {
    std::string pageText;
    if (currentPage >= 0 && currentPage < pageCount) {
      pageText = section->getTextFromSectionFile();
    }
    BookmarkEntry entry;
    entry.percentage = progress.percentage;
    entry.xpath = progress.xpath;
    entry.summary = BookmarkUtil::sanitizeBookmarkSummary(pageText);
    entry.computedSpineIndex = currentSpineIndex;
    entry.computedChapterPageCount = pageCount;
    entry.computedChapterProgress = currentPage;
    cachedBookmarks.insert(cachedBookmarks.begin(), entry);
    bookmarkRemoved = false;
    currentPageBookmarked = true;
  }

  if (!BookmarkFile::save(epub->getPath(), cachedBookmarks)) {
    LOG_ERR("ERS", "Failed to save bookmarks");
  }
  requestUpdate();
}

void EpubReaderActivity::updateBookmarkFlag() {
  if (!section || !epub || cachedBookmarks.empty()) {
    currentPageBookmarked = false;
    return;
  }
  const int pageCount = section->estimatedTotalPages();
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, section->currentPage, pageCount);
  currentPageBookmarked = std::any_of(cachedBookmarks.begin(), cachedBookmarks.end(), [&](const BookmarkEntry& b) {
    return bookmarkMatchesProgress(b, currentSpineIndex, section->currentPage, pageCount, pageRange);
  });
}

ScreenshotInfo EpubReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Epub;
  if (epub) {
    snprintf(info.title, sizeof(info.title), "%s", epub->getTitle().c_str());
    info.spineIndex = currentSpineIndex;
  }
  if (section) {
    info.currentPage = section->currentPage + 1;
    info.totalPages = section->estimatedTotalPages();
    if (epub && epub->getBookSize() > 0 && info.totalPages > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(info.totalPages);
      int pct = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      info.progressPercent = pct;
    }
  }
  return info;
}

CrossPointPosition EpubReaderActivity::getCurrentPosition() const {
  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPages};
  if (paragraphIndex.has_value()) {
    localPos.paragraphIndex = *paragraphIndex;
    localPos.hasParagraphIndex = true;
  }
  return localPos;
}
