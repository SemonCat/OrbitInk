/**
 * XtcReaderActivity.h
 *
 * XTC ebook reader activity for CrossPoint Reader
 * Displays pre-rendered XTC pages on e-ink display
 */

#pragma once

#include <Xtc.h>

#include <string>
#include <utility>

#include "EndOfBookOptions.h"
#include "ProgressDebounce.h"
#include "activities/Activity.h"

class XtcReaderActivity final : public Activity {
  std::shared_ptr<Xtc> xtc;

  uint32_t currentPage = 0;
  int pagesUntilFullRefresh = 0;
  // XTC pages never claim no-flash-maintenance eligibility (see displayWithRefreshCycle's call
  // site below), so this never advances -- kept only because the API requires an lvalue to bind.
  int consecutiveNoFlashMaintenance = 0;
  // Debounces progress.bin writes (see ProgressDebounce.h). lastObservedPage detects a genuine
  // page change so a no-op re-render doesn't feed the debounce counter.
  uint32_t lastObservedPage = UINT32_MAX;
  ProgressDebounce progressDebounce;
  // Next-book suggestion menu for the End-of-Book screen
  EndOfBookOptions endOfBookOptions;

  enum class StatusBarOverlayPosition { Bottom, Top };
  struct StatusBarInfo {
    int currentPage;
    int pageCount;
    std::string title;
  };

  void renderPage();
  // Opens chapter selection when the book has chapters (short-press Confirm); no-op otherwise
  void openChapterSelection();
  void renderStatusBarOverlay(StatusBarOverlayPosition position) const;
  StatusBarInfo getStatusBarInfo() const;
  bool saveProgress() const;
  void loadProgress();
  // Unconditional flush bypassing the debounce cadence. Returns false only if a write was
  // attempted and failed. Call before exit/sleep/book-transition.
  bool flushPendingProgress();

 public:
  explicit XtcReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Xtc> xtc,
                             int initialRefreshCountdown)
      : Activity("XtcReader", renderer, mappedInput),
        xtc(std::move(xtc)),
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
    }
    requestUpdate();
    return true;
  }
  ScreenshotInfo getScreenshotInfo() const override;
};
