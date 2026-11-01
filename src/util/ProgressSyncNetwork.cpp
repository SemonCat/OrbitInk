#include "ProgressSyncNetwork.h"

#include <HalGPIO.h>
#include <BookOrbitCredentialStore.h>
#include <KOReaderCredentialStore.h>
#include <KOReaderDocumentId.h>
#include <WiFi.h>

#include <optional>
#include <utility>

#include "ProgressSyncDeferred.h"
#include "ProgressSyncReceipt.h"
#include "WifiCredentialStore.h"

namespace ProgressSyncNetwork {
namespace {
unsigned depth = 0;
bool attempted = false;
unsigned long budgetMs = CONNECT_BUDGET_MS;
std::string identityPath;
std::string identity;

void stop() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}
}  // namespace

Batch::Batch(std::string bookPath, GfxRenderer* gfx, unsigned long connectBudgetMs)
    : path(std::move(bookPath)), renderer(gfx) {
  if (depth++ == 0) {
    attempted = false;
    budgetMs = connectBudgetMs;
    identityPath = path;
    identity.clear();
  }
}

Batch::~Batch() {
  if (depth > 1) {
    --depth;
    return;
  }
  if (attempted) stop();
  attempted = false;
  // No provider transport survives its call, and the radio is off before mapping.
  if (renderer) ProgressSyncDeferred::applyForBook(path, *renderer);
  depth = 0;
  budgetMs = CONNECT_BUDGET_MS;
  identityPath.clear();
  identity.clear();
}

bool active() { return depth != 0; }

bool available() {
  return WIFI_STORE.getCredentialCount() > 0 &&
         (BOOKORBIT_STORE.hasCredentials() ||
          (KOREADER_STORE.getAutoSyncEnabled() && KOREADER_STORE.hasCredentials()));
}

std::string binaryDocument(const std::string& path) {
  if (!active() || path != identityPath) return KOReaderDocumentId::calculateForStats(path);
  if (identity.empty()) identity = KOReaderDocumentId::calculateForStats(path);
  return identity;
}

bool connect() {
  if (active() && attempted) return WiFi.status() == WL_CONNECTED;
  if (active()) attempted = true;
  if (WIFI_STORE.getCredentialCount() == 0) return false;
  std::optional<WifiCredential> credential;
  const auto ssid = WIFI_STORE.getLastConnectedSsid();
  if (!ssid.empty()) credential = WIFI_STORE.findCredential(ssid);
  if (!credential) credential = WIFI_STORE.getCredentialAt(0);
  if (!credential) return false;

  WiFi.mode(WIFI_STA);
  if (credential->password.empty()) {
    WiFi.begin(credential->ssid.c_str());
  } else {
    WiFi.begin(credential->ssid.c_str(), credential->password.c_str());
  }
  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    gpio.update();
    if (millis() - start >= budgetMs || gpio.wasAnyPressed()) {
      ProgressSyncReceipt::noteWifi(false, millis() - start);
      return false;
    }
    delay(50);
  }
  ProgressSyncReceipt::noteWifi(true, millis() - start);
  return true;
}

void disconnect() {
  if (!active()) stop();
}
}  // namespace ProgressSyncNetwork
