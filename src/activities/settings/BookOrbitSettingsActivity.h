#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Submenu for BookOrbit Sync settings: username, password, server URL, and a test-connection
 * action -- BookOrbit's own credentials and server, entirely separate from KOReaderSettingsActivity
 * (KOReaderCredentialStore). No document-matching toggle: BookOrbit always identifies documents by
 * the binary partial-MD5 hash (see BookOrbitCredentialStore.h).
 *
 * Adapted, with attribution, from the MIT-licensed CrossInk-BookOrbit fork's
 * BookOrbitSettingsActivity (https://github.com/agosez/CrossInk-Bookorbit, commit 8c64d44) -- the
 * field list and the "separate from KOReaderSettingsActivity" framing come from there.
 */
class BookOrbitSettingsActivity final : public Activity {
 public:
  explicit BookOrbitSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BookOrbitSettings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;

  size_t selectedIndex = 0;
  bool showSaveError = false;

  void handleSelection();
};
