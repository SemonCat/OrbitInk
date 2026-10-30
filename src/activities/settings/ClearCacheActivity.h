#pragma once

#include <functional>

#include "activities/Activity.h"
#include "components/OptionPopup.h"

class ClearCacheActivity final : public Activity {
 public:
  explicit ClearCacheActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ClearCache", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  // Only while clearing. WARNING (a confirm dialog) and the SUCCESS/FAILED screens just wait for
  // a keypress; spinning the main loop at full CPU there only burned battery. clearCache() runs
  // synchronously inside beginClear(), so in practice this is polled with the state already
  // terminal -- the intent is stated here rather than left as an unconditional `true`.
  bool skipLoopDelay() override { return state == CLEARING; }
  void render(RenderLock&&) override;

 private:
  enum State { WARNING, CLEARING, SUCCESS, FAILED };

  State state = WARNING;

  void goBack() { finish(); }

  int clearedCount = 0;
  int failedCount = 0;
  OptionPopup confirmPopup;
  void beginClear();
  void clearCache();
};
