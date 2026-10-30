#pragma once

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace SyncSettingsWebPolicy {

constexpr size_t MAX_REQUEST_BYTES = 1024;
constexpr size_t MAX_USERNAME_LENGTH = 64;
constexpr size_t MAX_PASSWORD_LENGTH = 64;
constexpr size_t MAX_SERVER_URL_LENGTH = 128;

bool isRequestBodySizeAllowed(size_t bytes);

struct KOSyncState {
  std::string serverUrl;
  std::string username;
  std::string password;
  uint8_t matchMethod = 0;
  bool sendMetadata = false;
  uint8_t syncBehavior = 0;
  bool autoSync = true;
};

struct BookOrbitState {
  std::string serverUrl;
  std::string username;
  std::string password;
};

struct ValidationResult {
  bool valid = false;
  bool hasChanges = false;
  const char* error = "Invalid request";
};

ValidationResult parseKOSyncUpdate(JsonVariantConst input, const KOSyncState& current, KOSyncState& next);
ValidationResult parseLegacyKOSyncUpdate(JsonVariantConst input, const KOSyncState& current, KOSyncState& next);
ValidationResult parseBookOrbitUpdate(JsonVariantConst input, const BookOrbitState& current, BookOrbitState& next);

void writeKOSyncResponse(JsonDocument& output, const KOSyncState& state);
void writeBookOrbitResponse(JsonDocument& output, const BookOrbitState& state);

KOSyncState withoutCredentials(const KOSyncState& current);
BookOrbitState withoutCredentials(const BookOrbitState& current);

bool isLegacyKOSyncSetting(const char* key);
bool isLegacySecretSetting(const char* key);

template <typename State, typename Apply, typename Save>
bool persistWithRollback(const State& before, const State& after, Apply apply, Save save) {
  apply(after);
  if (save()) return true;
  apply(before);
  return false;
}

}  // namespace SyncSettingsWebPolicy
