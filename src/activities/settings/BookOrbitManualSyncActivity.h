#pragma once

#include "activities/Activity.h"
#include "util/BookOrbitAutoSync.h"

/**
 * "Sync Now" manual trigger for BookOrbit progress sync, reachable from BookOrbitSettingsActivity.
 *
 * Resolves direction automatically from origin timestamps. Invalid protocol/clock chronology is
 * reported without changing either side.
 *
 * Also unlike BookOrbitAuthActivity, this never launches an interactive WifiSelectionActivity:
 * syncNow() connects using the last-saved WiFi network the same way the silent auto-sync triggers
 * do, so "no saved network" reports as a plain NO_WIFI result rather than prompting to pick one.
 */
class BookOrbitManualSyncActivity final : public Activity {
 public:
  explicit BookOrbitManualSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BookOrbitManualSync", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == SYNCING; }

 private:
  enum State { SYNCING, DONE };

  State state = SYNCING;
  BookOrbitAutoSync::SyncNowResult result = BookOrbitAutoSync::SyncNowResult::ALREADY_SYNCED;
  std::string errorDetail;
  void performSync();
};
