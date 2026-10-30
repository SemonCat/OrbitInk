#pragma once
#include <BookOrbitSessionClock.h>
#include <BookOrbitStatsQueue.h>
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>

#include <optional>

#include "BookReadingStats.h"
#include "BookmarkEntry.h"
#include "BuildWindow.h"
#include "EndOfBookOptions.h"
#include "EpubReaderMenuActivity.h"
#include "GlobalReadingStats.h"
#include "PageDwellTracker.h"
#include "ProgressDebounce.h"
#include "ProgressMapper.h"
#include "ReadingStatsSession.h"
#include "activities/Activity.h"

class EpubReaderActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  // Set once from the constructor; consumed (and cleared) by the first onEnter().
  bool skipInitialRender = false;
  std::optional<uint16_t> pendingPageJump;
  // Set when navigating to a footnote href with a fragment (e.g. #note1).
  // Cleared on the next render after the new section loads and resolves it to a page.
  std::string pendingAnchor;
  int pagesUntilFullRefresh = 0;
  // How many consecutive maintenance boundaries in a row were satisfied by the no-flash substitute
  // instead of a real flash -- see ReaderUtils::displayWithRefreshCycle / mustForceRealMaintenance.
  int consecutiveNoFlashMaintenance = 0;
  // Image pages use a dedicated double-FAST refresh path, so retain a manual
  // refresh request until renderContents can issue its clean base pass.
  bool forcedRefreshPending = false;
  // Set alongside pagesUntilFullRefresh=1 after an image page: the grayscale pass leaves gray
  // charge in the image region that a plain fast diff on the very next page can't clear (#2190),
  // so that next page must flash even though it otherwise looks eligible for no-flash
  // maintenance. Consumed (cleared) the first time it's read.
  bool imageResidueCleanupPending = false;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  // Reading-stats timing: entirely no-op when !SETTINGS.shouldTrackReadingStats() -- see
  // recordPageDwellTime()'s guard. The tracker is still re-baselined unconditionally (a bare
  // millis() call costs nothing), but nothing reads it, mutates stats/globalStats, or touches SD
  // when the setting is off.
  PageDwellTracker pageDwell;
  uint32_t sessionReadingSeconds = 0;
  uint32_t sessionPaceSampleSeconds = 0;
  uint16_t sessionPaceSampleCount = 0;
  BookReadingStats stats;
  GlobalReadingStats globalStats;
  // One-way latch guaranteeing the session is committed once; see finalizeReadingStatsSession().
  ReadingStatsSession statsSession;
  // ---- Optional BookOrbit page-stats capture -------------------------------------------
  // Entirely inert unless BookOrbitStats::captureEnabled() -- which additionally requires the
  // Reading Stats setting above to be on AND the configured sync server to have already been
  // confirmed as BookOrbit. Resolved once in onEnter() so the per-page path is a bool test.
  bool bookOrbitCaptureEnabled = false;
  // Fixed-size session buffer: 32 x 12 = 384 bytes, allocated with the activity and never
  // grown. Deliberately not a std::vector -- a growing buffer in a long reading session is
  // exactly the repeated-realloc fragmentation the resource protocol forbids. Overflow simply
  // stops capturing for the rest of the session rather than dropping older events, since these
  // are flushed as one batch at exit anyway.
  BookOrbitStatEvent bookOrbitEvents[BookOrbitStatsQueue::MAX_EVENTS_PER_BATCH];
  uint8_t bookOrbitEventCount = 0;
  // Identity of the server/account this session's events belong to, stamped into the queue so a
  // later reconfiguration can never upload them to a different account.
  BookOrbitOwnerId bookOrbitOwner;
  // The book's KOSync document hash, which is both the stats queue's key and the identity the
  // events upload under. Resolved once in onEnter() (only when capture is enabled, so a device
  // that is not using BookOrbit never pays for it), and empty when capture is off.
  std::string bookOrbitDocumentHash;
  // Session wall clock: the X3 RTC gives whole minutes only, so this anchors on one reading and
  // projects forward with millis(). Unanchored (and capture skipped) when the RTC is invalid.
  BookOrbitSessionClock bookOrbitClock;
  // Folds the current page's dwell time (since pageShownAtMs) into sessionReadingSeconds, and --
  // for a forward turn -- into stats' running pace average. Discards dwell longer than
  // READING_STATS_IDLE_THRESHOLD_SECONDS (the reader was left open, not actively read) or shorter
  // than READING_STATS_MIN_DWELL_SECONDS (an accidental/instant double-turn). Always resets
  // pageShownAtMs to now for the page that's about to show. No-op when reading-stats tracking is
  // off.
  void recordPageDwellTime(bool isForwardTurn);
  // Commits the reading-stats session exactly once: final page dwell, aggregate session totals,
  // per-book + global saves, and the BookOrbit event flush.
  //
  // Called from onExit() AND immediately before manual KOSync's early epub/section teardown --
  // that path releases both objects to free heap for TLS before the activity is replaced, so
  // onExit()'s own guard (`epub` non-null) would otherwise skip the whole commit and lose the
  // session. ReadingStatsSession's latch makes the second call a no-op, so neither site needs
  // to know about the other.
  void finalizeReadingStatsSession();
  // Appends one BookOrbit page-stat event for the page just finished, using `dwellSeconds` as
  // measured by recordPageDwellTime() -- which is the only caller, so the existing dwell/idle
  // qualification rules apply unchanged and the final page of a session (recorded from
  // onExit(), including the deep-sleep path) is captured like any other. No-op unless
  // bookOrbitCaptureEnabled and the session clock is anchored.
  // Closes out the dwell on the page currently displayed, because something OTHER than a page
  // turn is about to replace it: a percentage jump, a chapter or bookmark jump, a followed link
  // or footnote, a position restore, or an orientation reflow.
  //
  // Without this those paths carry the outgoing page's dwell across the jump, so the time is
  // credited to whatever page lands next and its BookOrbit event is stamped with the WRONG
  // position -- a chapter jump after five minutes on one page would file five minutes against
  // the chapter it jumped to. recordPageDwellTime() is idempotent in the sense that matters
  // here: it re-baselines pageShownAtMs, so a redundant call simply measures a zero dwell, which
  // the minimum-dwell rule discards.
  void leaveCurrentPageForStats() { recordPageDwellTime(/*isForwardTurn=*/false); }
  void captureBookOrbitPageEvent(uint32_t dwellSeconds);
  // Appends whatever the session buffer holds to the book's durable queue and empties it.
  // Called when the buffer fills mid-session and again from onExit(); a storage failure leaves
  // the buffer populated so the next attempt can retry rather than losing the events.
  void flushBookOrbitEvents();
  // Recomputes stats.estimatedTimeLeftSeconds from the current pace average and remaining pages
  // (current chapter's real pagination + the rest of the book estimated at this chapter's own
  // bytes-per-page density). Zeroes it out when there's no pace data yet, or the book is already
  // finished (FINISHED_BOOKS is the source of truth for that -- this doesn't duplicate it).
  void refreshEstimatedTimeLeft();
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  bool pendingScreenshot = false;
  bool pendingSyncSaveError = false;
  // Consecutive page-load failures. Each failure drops the section and rebuilds on the next render,
  // which recovers a transiently corrupt cache; capped so a persistently bad page can't spin forever.
  uint8_t pageLoadRetryCount = 0;
  static constexpr uint8_t MAX_PAGE_LOAD_RETRIES = 3;
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  bool automaticPageTurnActive = false;
  bool showBookmarkMessage = false;
  bool showKoSyncSuccess = false;
  unsigned long koSyncSuccessTime = 0UL;
  // "No dictionary set" popup, shown when a lookup is triggered without a configured dictionary.
  bool showDictionaryMessage = false;
  unsigned long dictionaryMessageTime = 0UL;
  bool ignoreNextConfirmRelease = false;
  bool currentPageBookmarked = false;
  // Idle-time glyph prewarm: after a page settles, scan the LIKELY next page
  // (scan mode draws nothing) and load its missing glyphs from SD during idle,
  // so the next turn's in-render prewarm is a cache hit instead of ~100 ms of
  // SD reads on the page-turn critical path. One attempt per position.
  int idlePrewarmSpine = -1;
  int idlePrewarmPage = -1;
  unsigned long lastRenderCompleteMs = 0;
  bool bookmarkRemoved = false;  // true when last toggle removed (controls popup text)
  std::vector<BookmarkEntry> cachedBookmarks;
  // Tracks whether this book is currently removed from Recent Books by the
  // removeReadBooksFromRecents feature (set at End-of-Book, cleared if paged back in).
  bool recentsEntryRemoved = false;
  unsigned long bookmarkMessageTime = 0UL;
  // Set when the reader is left at end-of-book and SETTINGS.moveFinishedToReadFolder is on.
  // Consumed in onExit() to relocate the finished book into /Read/.
  bool pendingReadFolderMove = false;
  // Next-book suggestion menu for the End-of-Book screen
  EndOfBookOptions endOfBookOptions;

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  // Viewport of the last render(), captured so loop()'s lazy partial-extension start
  // builds with IDENTICAL layout parameters to the pages already rendered (a mismatch
  // would paginate differently than the partial being extended). 0 = no render yet.
  uint16_t buildViewportWidth = 0;
  uint16_t buildViewportHeight = 0;
  // Set when the lazy extension start failed, so loop() doesn't retry (and log) every
  // tick; the blocking extension in render() remains the fallback past the watermark.
  bool partialRebuildStartFailed = false;
  bool remotePositionPending = false;
  bool remoteApplyAttempted = false;
  bool remoteBuildFailed = false;
  std::optional<uint32_t> remoteBuildHeapPausedSince;

  // Position observed by the last render() tick, regardless of whether it was actually written --
  // detects a genuine page/spine change so a no-op re-render (menu/bookmark/screenshot) doesn't
  // feed the debounce counter below. Distinct from "last persisted": progressDebounce decides
  // WHEN an observed change actually reaches disk.
  int lastObservedSpineIndex = -1;
  int lastObservedPage = -1;
  int lastObservedPageCount = -1;
  // Debounces the progress.bin write itself (see ProgressDebounce.h for the flush policy).
  // Callers must still force an unconditional flush before exit/sleep/book-transition/KOSync --
  // see onExit() and launchKOReaderSync().
  ProgressDebounce progressDebounce;
  // Origin time of the last genuine logical navigation. It is loaded from progress.bin and
  // reused by every open/close/retry/layout save. A navigation with unavailable/backwards RTC
  // time deliberately sets it to unknown (0) rather than making the old position look newest.
  uint64_t progressEventTimestamp = 0;
  bool progressIsLegacy = false;
  bool readingNavigationPending = false;

  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft, const RenderLock& lock);
  void renderStatusBar() const;
  // Pages laid out per incremental-build pump: on the render path (catching up to the page
  // being shown) and per loop() tick (background build of a large chapter). Kept small so a
  // background build chunk never noticeably delays input or a pending render.
  static constexpr int BUILD_PAGES_PER_CHUNK = 8;
  static constexpr int BACKGROUND_BUILD_PAGES_PER_TICK = 2;
  // Wall-clock ceiling for one background build tick, on top of the page cap above (see
  // Section::buildSomeMore). Two pages is normally ~60ms, but "two pages" is not a bound on
  // anything: a page dominated by a large image, or a stretch of markup that lays out no page
  // at all, can run far longer -- and the tick holds the RenderLock throughout, so a pending
  // render and the input it belongs to wait behind it. 120ms keeps a tick under the ~150ms
  // where a page-turn press starts to feel dropped, while still being many parse steps of real
  // progress per tick. Overshoot is bounded by one parse step (an image decode cannot be
  // interrupted mid-way), not by this number.
  static constexpr uint32_t BACKGROUND_BUILD_BUDGET_MS = 120;

  // MEMFIX-PORT: background-build heap floor; portable
  // Skip background build ticks below this free-heap floor. The parse path grows
  // word vectors of heap strings — throwing allocations that abort() on OOM under
  // -fno-exceptions (field crash: bad_alloc in ParsedText::addWord during a
  // background tick under heap pressure). The tick is deferrable work:
  // page-turn transients free up between turns and the build resumes; the render
  // path still builds the page it actually needs regardless of this floor.
  static constexpr size_t BACKGROUND_BUILD_MIN_FREE_HEAP = 32 * 1024;
  // Fragmentation floor for the same gate: a tick passed the free-heap floor at
  // 34.7 KB free but the largest block was ~11 KB, and a parse allocation inside the
  // tick aborted anyway. Free heap says how much memory exists; maxAlloc says whether
  // any single allocation can actually have it. 16 KB also keeps the advance-table
  // batch path (16 KB scratch) viable during builds.
  static constexpr size_t BACKGROUND_BUILD_MIN_MAX_ALLOC = 16 * 1024;
  // Gate for a background build tick: true when the heap can take parse allocations.
  // Updates buildHeapPaused as a side effect.
  bool buildTickHeapGate();
  // True when the background build still has pages the reader will actually need soon -- the
  // single source of truth for both the build tick in loop() and skipLoopDelay(). Without the
  // shared predicate, a build that had run BUILD_WINDOW_AHEAD pages ahead kept skipLoopDelay()
  // true (isBuilding() is still true) while the tick itself correctly did nothing, so the loop
  // spun at full CPU speed doing no work for as long as the reader stayed in that chapter.
  // Side-effect free: deliberately does NOT consult buildTickHeapGate() (see BuildWindow.h).
  bool hasBuildWorkInWindow() const {
    return section && ((remotePositionPending && section->isBuilding()) ||
                      readerbuild::hasBuildWorkInWindow(section->isBuilding(), section->isPartial(),
                                                        static_cast<int>(section->pageCount), section->currentPage,
                                                        BUILD_WINDOW_AHEAD));
  }
  // True while the background build is gated on the heap floors. Lets skipLoopDelay()
  // return the loop to normal delay/power-saving during the pause: isBuilding() stays
  // true the whole time, and without this the loop would spin at full CPU speed doing
  // no build work — indefinitely, if the build context itself keeps the heap low.
  bool buildHeapPaused = false;
  // Heap floor for optional render-adjacent work (idle prewarm). Page
  // deserialization (TextBlock word vectors/strings) and glyph caching allocate
  // through throwing paths that abort() on OOM; skip deferrable work below it.
  static constexpr size_t RENDER_MIN_FREE_HEAP = 24 * 1024;
  // How many pages to keep laid out ahead of the reader for a still-building section. A page
  // turn is ~1s on e-ink and a page builds in ~30ms, so the reader can't out-click the builder
  // -- a tiny buffer is enough. The background build stops once the watermark is this far
  // ahead and resumes as the reader advances; building unbounded instead locked up input by
  // monopolizing the RenderLock. A giant single-spine book therefore never finalizes its .bin
  // in one sitting -- instant reopen comes from Section::suspendBuild() persisting the pages
  // already laid out as a partial file on exit/sleep.
  static constexpr int BUILD_WINDOW_AHEAD = 5;
  // Reopening a partial does NOT immediately restart its extension build (a whole-chapter
  // re-layout from page 0 -- minutes of background CPU + SD writes on a giant spine, wasted
  // when the reader never crosses the watermark that session). Instead loop() starts it once
  // the reader is within this many pages of the watermark: at ~30s per page read and ~100-300ms
  // per page rebuilt, this margin gives the rebuild ample runway to catch up (and finalize)
  // before the reader arrives.
  static constexpr int PARTIAL_REBUILD_START_MARGIN = 15;
  // Show the indexing popup when an initial build must lay out more than this many pages up front
  // (a deep resume/jump into a not-yet-built section), so it isn't a silent wait. Kept independent
  // of the small look-ahead window so ordinary landings stay popup-free.
  static constexpr int BUILD_POPUP_PAGE_THRESHOLD = 20;
  // Also show the popup when first building a spine larger than this (uncompressed bytes): its
  // whole HTML must be inflated before page 1 can lay out (the giant single-spine case), which is
  // a multi-second wait. Normal chapters are well under this and stay popup-free.
  static constexpr size_t BUILD_POPUP_BYTE_THRESHOLD = 96 * 1024;
  // Deadline backstop for the predictive gates above: if the blocking build-to-target still
  // hasn't produced the landing page this long after the build started, surface the popup
  // mid-build. Builds that finish under the deadline stay popup-free.
  static constexpr unsigned long BUILD_POPUP_DEADLINE_MS = 1000;
  // True only during onEnter's blocking build-to-target phase, until the popup has been
  // drawn. Gates showBuildPopup() so the parser's popup callback (which persists into
  // background buildSomeMore chunks) can never draw over a displayed page.
  bool buildPopupPending = false;
  // Draw the indexing popup mid-build (parser image-probe callback and deadline backstop).
  void showBuildPopup();
  // Remap the cached relative reading position once the section's real page count is known
  // (used after a settings change re-paginates a chapter). Returns true if currentPage moved.
  // No-op while the section is still building or when the pagination is unchanged (plain resume).
  bool applyDeferredReposition();
  bool saveProgress(int spineIndex, int currentPage, int pageCount);
  void markReadingPositionChanged();
  // Unconditional flush of any debounced progress change, bypassing ProgressDebounce's cadence.
  // While mid-footnote, persists the pre-footnote origin instead of the footnote's own position
  // (see navigateToHref/restoreSavedPosition) -- footnote navigation is never real reading
  // progress. Returns false only if a write was attempted and failed; a no-op (nothing pending)
  // returns true. Call before exit, sleep, book transitions, cache invalidation, and any KOSync
  // request so none of them can observe or upload a stale position.
  bool flushPendingProgress();
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  // Opens the reader menu for the current position (short-press Confirm)
  void openReaderMenu();
  void openDictionaryWordSelect();
  // Returns true if sync acted (launched, or surfaced a save error); false if it was a no-op
  // because no KOReader credentials are stored.
  bool launchKOReaderSync();
  void applyOrientation(uint8_t orientation);
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void pageTurn(bool isForwardTurn);
  void loadCachedBookmarks();
  void addBookmark();
  void updateBookmarkFlag();

  // Footnote navigation
  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

  // X3 Quick Resume only: attempts to skip the initial page render, trusting the already-restored
  // sleep-frame pixels. Prepares section state for interaction (loadSectionFile only, not
  // loadPage -- see currentPageLoaded) without touching the framebuffer/display. Returns true on
  // success (caller must not call requestUpdate()); false means the caller must fall back to the
  // normal requestUpdate() path -- every failure path here is a safe fallback, never a crash.
  bool tryQuickResumeWithoutRender();
  // Lazily loads the current page's metadata (footnotes) if a Quick Resume skip-render left it
  // unpopulated. No-op (and cheap) once currentPageLoaded is true, which is the common case.
  void ensureCurrentPageLoaded();
  // True once the current page's full metadata (currentPageFootnotes) reflects section->currentPage.
  // Only ever false immediately after a successful tryQuickResumeWithoutRender(), until the next
  // real render() or ensureCurrentPageLoaded() call.
  bool currentPageLoaded = true;

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub,
                              int initialRefreshCountdown, bool skipInitialRender = false)
      : Activity("EpubReader", renderer, mappedInput),
        epub(std::move(epub)),
        pagesUntilFullRefresh(initialRefreshCountdown),
        skipInitialRender(skipInitialRender) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  // Full CPU speed + fast loop ticks while a section build has work to do: at the
  // low-power frequency a giant chapter's background rebuild stretches from ~40s to
  // many minutes, so the reader exits before it can finalize and the next open
  // restarts it from page 0. Reverts to normal power behavior the moment the build
  // finishes, while the build is heap-paused (no work is happening, so spinning at
  // full speed would only burn battery; the paused gate still retries every loop
  // pass), and -- via hasBuildWorkInWindow() -- once the build has run
  // BUILD_WINDOW_AHEAD pages ahead of the reader and the tick has correspondingly
  // stopped doing work. Partials and heap catch-up keep their old behavior.
  bool skipLoopDelay() override { return section && !buildHeapPaused && hasBuildWorkInWindow(); }
  bool isReaderActivity() const override { return true; }
  bool allowReaderIdlePowerSaving() const override { return true; }
  bool isQuickResumeEligibleBaseView() const override {
    return footnoteDepth == 0 && !showBookmarkMessage && !showDictionaryMessage && !showKoSyncSuccess &&
           !buildPopupPending;
  }
  bool handleForcedRefresh() override {
    {
      RenderLock lock(*this);
      pagesUntilFullRefresh = 1;
      forcedRefreshPending = true;
    }
    requestUpdate();
    return true;
  }
  ScreenshotInfo getScreenshotInfo() const override;
  CrossPointPosition getCurrentPosition() const;
};
