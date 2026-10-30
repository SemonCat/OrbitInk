#pragma once

#include <functional>

#include "NetworkModeList.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * NetworkModeSelectionActivity presents the user with a choice:
 * - "Join a Network" - Connect to an existing WiFi network (STA mode)
 * - "Connect to Calibre" - Use Calibre wireless device transfers
 * - "Create Hotspot" - Create an Access Point that others can connect to (AP mode)
 *
 * includeCalibre = false (BookOrbit web setup) omits "Connect to Calibre"; see NetworkModeList.h.
 *
 * The onModeSelected callback is called with the user's choice.
 * The onCancel callback is called if the user presses back.
 */
class NetworkModeSelectionActivity final : public Activity {
  ButtonNavigator buttonNavigator;

  int selectedIndex = 0;
  const bool includeCalibre;

 public:
  explicit NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        const bool includeCalibre = true)
      : Activity("NetworkModeSelection", renderer, mappedInput), includeCalibre(includeCalibre) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  void onModeSelected(NetworkMode mode);
  void onCancel();
};
