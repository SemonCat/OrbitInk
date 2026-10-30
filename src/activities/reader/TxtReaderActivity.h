#pragma once

#include <Txt.h>

#include <vector>

#include "CrossPointSettings.h"
#include "ProgressDebounce.h"
#include "activities/Activity.h"

class TxtReaderActivity final : public Activity {
  std::unique_ptr<Txt> txt;

  int currentPage = 0;
  int totalPages = 1;
  int pagesUntilFullRefresh = 0;
  // How many consecutive maintenance boundaries in a row were satisfied by the no-flash substitute
  // instead of a real flash -- see ReaderUtils::displayWithRefreshCycle / mustForceRealMaintenance.
  int consecutiveNoFlashMaintenance = 0;
  // Set by handleForcedRefresh(); gates the next maintenance-boundary refresh onto the full
  // HALF_REFRESH cleanup path regardless of otherwise-eligible page content (a manual forced
  // refresh must never be silently downgraded to no-flash).
  bool forcedRefreshPending = false;
  // Debounces progress.bin writes (see ProgressDebounce.h). lastObservedPage detects a genuine
  // page change so a no-op re-render doesn't feed the debounce counter.
  int lastObservedPage = -1;
  ProgressDebounce progressDebounce;

  // Streaming text reader - stores file offsets for each page
  std::vector<size_t> pageOffsets;  // File offset for start of each page
  std::vector<std::string> currentPageLines;
  int linesPerPage = 0;
  int viewportWidth = 0;
  bool initialized = false;

  // Cached settings for cache validation (different fonts/margins require re-indexing)
  int cachedFontId = 0;
  uint8_t cachedScreenMargin = 0;
  uint8_t cachedParagraphAlignment = CrossPointSettings::LEFT_ALIGN;
  int cachedOrientedMarginTop = 0;
  int cachedOrientedMarginRight = 0;
  int cachedOrientedMarginBottom = 0;
  int cachedOrientedMarginLeft = 0;

  void renderPage();
  void renderStatusBar() const;

  void initializeReader();
  bool loadPageAtOffset(size_t offset, std::vector<std::string>& outLines, size_t& nextOffset);
  void buildPageIndex();
  bool loadPageIndexCache();
  void savePageIndexCache() const;
  bool saveProgress() const;
  void loadProgress();
  // Unconditional flush bypassing the debounce cadence. Returns false only if a write was
  // attempted and failed. Call before exit/sleep/book-transition.
  bool flushPendingProgress();

 public:
  explicit TxtReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Txt> txt,
                             int initialRefreshCountdown)
      : Activity("TxtReader", renderer, mappedInput),
        txt(std::move(txt)),
        pagesUntilFullRefresh(initialRefreshCountdown) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool allowReaderIdlePowerSaving() const override { return true; }
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
};
