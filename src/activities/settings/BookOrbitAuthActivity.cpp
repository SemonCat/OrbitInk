#include "BookOrbitAuthActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include "BookOrbitSyncClient.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookOrbitStatsSync.h"
#include "util/KOSyncWifiPowerSaveGuard.h"

void BookOrbitAuthActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    {
      RenderLock lock(*this);
      state = FAILED;
      errorMessage = tr(STR_WIFI_CONN_FAILED);
    }
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    state = AUTHENTICATING;
    statusMessage = tr(STR_AUTHENTICATING);
  }
  requestUpdate();

  performTestConnection();
}

void BookOrbitAuthActivity::performTestConnection() {
  // Bounded to these two requests on one session: modem sleep's latency has been observed to
  // make the wolfSSL TLS handshake fail with http=-1 (same reasoning as KOReaderAuthActivity).
  const KOSyncWifiPowerSaveGuard wifiPowerSaveGuard;
  BookOrbitSyncClient::Session session;

  const auto authResult = session.authenticate();
  BookOrbitSyncClient::Error result = authResult;
  if (authResult == BookOrbitSyncClient::OK) {
    // Valid credentials alone are not "connected to BookOrbit": the same headers authenticate
    // against any KOSync-dialect server. Confirming the capability probe here is what this
    // explicit provider exists to guarantee before the user walks away thinking sync will work.
    BookOrbitSyncClient::Capabilities caps;
    result = session.probeCapabilities(caps);
    if (result == BookOrbitSyncClient::OK && !caps.isBookOrbit) result = BookOrbitSyncClient::NOT_BOOKORBIT;
    // Persist this explicit, user-triggered probe the same way the automatic sync path's own
    // ensureCapabilitiesConfirmed() does -- otherwise a successful Test Connection here is
    // discarded and the very next sync re-probes from scratch, needing its own confirmation
    // round-trip before it can drain stats/bookmarks/progress under the newly-fixed gates.
    if (result == BookOrbitSyncClient::OK && caps.isBookOrbit) BookOrbitStats::recordCapabilities(caps);
  }

  {
    RenderLock lock(*this);
    if (result == BookOrbitSyncClient::OK) {
      state = SUCCESS;
      statusMessage = tr(STR_AUTH_SUCCESS);
    } else {
      state = FAILED;
      errorMessage = result == BookOrbitSyncClient::NOT_BOOKORBIT ? tr(STR_NOT_BOOKORBIT)
                                                                  : BookOrbitSyncClient::errorString(result);
    }
  }
  requestUpdate();
}

void BookOrbitAuthActivity::onEnter() {
  Activity::onEnter();

  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void BookOrbitAuthActivity::onExit() {
  Activity::onExit();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void BookOrbitAuthActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BOOKORBIT_AUTH));
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  if (state == AUTHENTICATING) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, statusMessage.c_str());
  } else if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_AUTH_SUCCESS), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height + 10, tr(STR_BOOKORBIT_SYNC_READY));
  } else if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_AUTH_FAILED), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height + 10, errorMessage.c_str());
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void BookOrbitAuthActivity::loop() {
  if (state == SUCCESS || state == FAILED) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
      finish();
    }
  }
}
