#pragma once

#include "activities/Activity.h"

/**
 * Activity for testing BookOrbit credentials. Connects to WiFi, authenticates against
 * .../users/auth, then confirms the server is actually BookOrbit via the capability probe
 * (.../plugin/version) -- valid KOSync-dialect credentials against a server that is NOT BookOrbit
 * is reported as a distinct failure, not a success, since that is exactly the misconfiguration
 * this explicit provider exists to catch rather than silently accept.
 *
 * No sign-up mode, unlike KOReaderAuthActivity: BookOrbit's own account registration is a JWT-
 * gated web flow, never done from the koreader-plugin wire protocol this firmware speaks.
 *
 * Adapted, with attribution, from the MIT-licensed CrossInk-BookOrbit fork's
 * BookOrbitAuthActivity (https://github.com/agosez/CrossInk-Bookorbit, commit 8c64d44) -- the
 * WIFI_SELECTION -> CONNECTING -> AUTHENTICATING -> SUCCESS/FAILED state shape and the "test
 * connection" framing come from there; this implementation additionally requires the capability
 * probe to confirm BookOrbit (that fork's own client used a differently-shaped auth call).
 */
class BookOrbitAuthActivity final : public Activity {
 public:
  explicit BookOrbitAuthActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BookOrbitAuth", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == AUTHENTICATING; }

 private:
  enum State { WIFI_SELECTION, CONNECTING, AUTHENTICATING, SUCCESS, FAILED };

  State state = WIFI_SELECTION;
  std::string statusMessage;
  std::string errorMessage;

  void onWifiSelectionComplete(bool success);
  void performTestConnection();
};
