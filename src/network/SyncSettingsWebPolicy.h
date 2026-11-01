#pragma once

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <string>

// Web setup validation for the BookOrbit account (/api/bookorbit). The independent KOSync provider
// and its /api/kosync and legacy ko* settings were removed from this BookOrbit-only product; a
// stale ko* field in a /api/settings POST matches no setting and is ignored.
namespace SyncSettingsWebPolicy {

constexpr size_t MAX_REQUEST_BYTES = 1024;
constexpr size_t MAX_USERNAME_LENGTH = 64;
constexpr size_t MAX_PASSWORD_LENGTH = 64;
constexpr size_t MAX_SERVER_URL_LENGTH = 128;

bool isRequestBodySizeAllowed(size_t bytes);

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

ValidationResult parseBookOrbitUpdate(JsonVariantConst input, const BookOrbitState& current, BookOrbitState& next);

void writeBookOrbitResponse(JsonDocument& output, const BookOrbitState& state);

BookOrbitState withoutCredentials(const BookOrbitState& current);

template <typename State, typename Apply, typename Save>
bool persistWithRollback(const State& before, const State& after, Apply apply, Save save) {
  apply(after);
  if (save()) return true;
  apply(before);
  return false;
}

}  // namespace SyncSettingsWebPolicy
