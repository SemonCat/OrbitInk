#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * First-run BookOrbit setup guide, pushed by BookOrbitHomeActivity when BookOrbitSetupPolicy::shouldShowGuide()
 * holds. Explains what BookOrbit needs, lists what a partially saved account still lacks, and offers:
 * - "Set up in a web browser": ActivityManager::goToBookOrbitWebSetup() -- the existing web server (Join
 *   Network / Create Hotspot) with the existing Settings page and /api/bookorbit. WiFi starts only here.
 * - "Set up on this device": the existing BookOrbitSettingsActivity; returns here while still incomplete.
 * - "Later" (or Back): persists CrossPointSettings::bookOrbitSetupGuideDismissed and returns to Home.
 * Never writes BookOrbit credentials itself.
 *
 * recordHandledIfConfigured() persists the same marker the first time BookOrbit is complete. It is
 * called at boot (already-configured upgrades, and web setup, whose exit reboots) and on Home/guide
 * entry (on-device completion in the same session), so a later credential clear never reopens the guide.
 */
class BookOrbitSetupGuideActivity final : public Activity {
 public:
  static void recordHandledIfConfigured();

  explicit BookOrbitSetupGuideActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BookOrbitSetupGuide", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;

  // Layout computed in onEnter() so render() and touch handling agree.
  std::vector<std::string> introLines;
  std::string missingLine;  // empty unless a partial account is saved
  int textLineHeight = 0;   // text-aware: the zh-TW UI fallback font is taller than UI_10's own
  int listTop = 0;
  int listHeight = 0;

  void activate();
  void dismiss();
};
