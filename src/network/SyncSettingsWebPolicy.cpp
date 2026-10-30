#include "SyncSettingsWebPolicy.h"

#include <cstring>

namespace SyncSettingsWebPolicy {
namespace {

ValidationResult invalid(const char* error) { return {.valid = false, .hasChanges = false, .error = error}; }

ValidationResult valid(bool hasChanges) { return {.valid = true, .hasChanges = hasChanges, .error = nullptr}; }

bool readString(JsonObjectConst input, const char* key, size_t maxLength, std::string& output, bool& found,
                const char*& error) {
  JsonVariantConst value = input[key];
  if (value.isUnbound()) return true;

  found = true;
  if (!value.is<const char*>()) {
    error = "Expected a string";
    return false;
  }

  const char* text = value.as<const char*>();
  if (text == nullptr || std::strlen(text) > maxLength) {
    error = "String is too long";
    return false;
  }

  output = text;
  return true;
}

bool readPassword(JsonObjectConst input, const char* key, std::string& output, bool& found, const char*& error) {
  std::string candidate;
  bool present = false;
  if (!readString(input, key, MAX_PASSWORD_LENGTH, candidate, present, error)) return false;
  if (!present) return true;

  found = true;
  if (!candidate.empty()) output = candidate;
  return true;
}

bool readEnum(JsonObjectConst input, const char* key, uint8_t& output, bool& found, const char*& error) {
  JsonVariantConst value = input[key];
  if (value.isUnbound()) return true;

  found = true;
  if (value.is<bool>() || !value.is<int>()) {
    error = "Expected an integer enum";
    return false;
  }

  const int candidate = value.as<int>();
  if (candidate < 0 || candidate > 1) {
    error = "Enum is out of range";
    return false;
  }

  output = static_cast<uint8_t>(candidate);
  return true;
}

bool readBool(JsonObjectConst input, const char* key, bool& output, bool& found, const char*& error) {
  JsonVariantConst value = input[key];
  if (value.isUnbound()) return true;

  found = true;
  if (!value.is<bool>() || value.is<int>()) {
    error = "Expected a boolean";
    return false;
  }

  output = value.as<bool>();
  return true;
}

bool readLegacyBool(JsonObjectConst input, const char* key, bool& output, bool& found, const char*& error) {
  uint8_t candidate = output ? 1 : 0;
  if (!readEnum(input, key, candidate, found, error)) return false;
  output = candidate != 0;
  return true;
}

void normalizeEmptyScheme(std::string& url) {
  if (url == "https://" || url == "http://") url.clear();
}

bool isObject(JsonVariantConst input) { return input.is<JsonObjectConst>(); }

template <size_t N>
bool hasOnlyKeys(JsonObjectConst input, const char* const (&allowed)[N]) {
  for (JsonPairConst entry : input) {
    bool recognized = false;
    for (const char* key : allowed) {
      if (std::strcmp(entry.key().c_str(), key) == 0) {
        recognized = true;
        break;
      }
    }
    if (!recognized) return false;
  }
  return true;
}

}  // namespace

bool isRequestBodySizeAllowed(const size_t bytes) { return bytes > 0 && bytes <= MAX_REQUEST_BYTES; }

ValidationResult parseKOSyncUpdate(JsonVariantConst input, const KOSyncState& current, KOSyncState& next) {
  next = current;
  if (!isObject(input)) return invalid("Expected a JSON object");

  const JsonObjectConst object = input.as<JsonObjectConst>();
  constexpr const char* allowed[] = {"serverUrl",    "username",     "password", "matchMethod",
                                     "sendMetadata", "syncBehavior", "autoSync"};
  if (!hasOnlyKeys(object, allowed)) return invalid("Unknown setting");
  bool found = false;
  const char* error = nullptr;
  if (!readString(object, "serverUrl", MAX_SERVER_URL_LENGTH, next.serverUrl, found, error) ||
      !readString(object, "username", MAX_USERNAME_LENGTH, next.username, found, error) ||
      !readPassword(object, "password", next.password, found, error) ||
      !readEnum(object, "matchMethod", next.matchMethod, found, error) ||
      !readBool(object, "sendMetadata", next.sendMetadata, found, error) ||
      !readEnum(object, "syncBehavior", next.syncBehavior, found, error) ||
      !readBool(object, "autoSync", next.autoSync, found, error)) {
    next = current;
    return invalid(error);
  }

  normalizeEmptyScheme(next.serverUrl);
  if (!found) {
    next = current;
    return invalid("No recognized settings");
  }
  return valid(true);
}

ValidationResult parseLegacyKOSyncUpdate(JsonVariantConst input, const KOSyncState& current, KOSyncState& next) {
  next = current;
  if (!isObject(input)) return invalid("Expected a JSON object");

  const JsonObjectConst object = input.as<JsonObjectConst>();
  bool found = false;
  const char* error = nullptr;
  if (!readString(object, "koServerUrl", MAX_SERVER_URL_LENGTH, next.serverUrl, found, error) ||
      !readString(object, "koUsername", MAX_USERNAME_LENGTH, next.username, found, error) ||
      !readPassword(object, "koPassword", next.password, found, error) ||
      !readEnum(object, "koMatchMethod", next.matchMethod, found, error) ||
      !readLegacyBool(object, "koSendMetadata", next.sendMetadata, found, error) ||
      !readEnum(object, "koSyncBehavior", next.syncBehavior, found, error) ||
      !readLegacyBool(object, "koAutoSync", next.autoSync, found, error)) {
    next = current;
    return invalid(error);
  }

  normalizeEmptyScheme(next.serverUrl);
  return valid(found);
}

ValidationResult parseBookOrbitUpdate(JsonVariantConst input, const BookOrbitState& current, BookOrbitState& next) {
  next = current;
  if (!isObject(input)) return invalid("Expected a JSON object");

  const JsonObjectConst object = input.as<JsonObjectConst>();
  constexpr const char* allowed[] = {"serverUrl", "username", "password"};
  if (!hasOnlyKeys(object, allowed)) return invalid("Unknown setting");
  bool found = false;
  const char* error = nullptr;
  if (!readString(object, "serverUrl", MAX_SERVER_URL_LENGTH, next.serverUrl, found, error) ||
      !readString(object, "username", MAX_USERNAME_LENGTH, next.username, found, error) ||
      !readPassword(object, "password", next.password, found, error)) {
    next = current;
    return invalid(error);
  }

  normalizeEmptyScheme(next.serverUrl);
  if (!found) {
    next = current;
    return invalid("No recognized settings");
  }
  return valid(true);
}

void writeKOSyncResponse(JsonDocument& output, const KOSyncState& state) {
  output.clear();
  output["serverUrl"] = state.serverUrl;
  output["username"] = state.username;
  output["hasPassword"] = !state.password.empty();
  output["configured"] = !state.username.empty() && !state.password.empty();
  output["matchMethod"] = state.matchMethod;
  output["sendMetadata"] = state.sendMetadata;
  output["syncBehavior"] = state.syncBehavior;
  output["autoSync"] = state.autoSync;
}

void writeBookOrbitResponse(JsonDocument& output, const BookOrbitState& state) {
  output.clear();
  output["serverUrl"] = state.serverUrl;
  output["username"] = state.username;
  output["hasPassword"] = !state.password.empty();
  output["configured"] = !state.serverUrl.empty() && !state.username.empty() && !state.password.empty();
}

KOSyncState withoutCredentials(const KOSyncState& current) {
  KOSyncState next = current;
  next.username.clear();
  next.password.clear();
  return next;
}

BookOrbitState withoutCredentials(const BookOrbitState& current) {
  BookOrbitState next = current;
  next.username.clear();
  next.password.clear();
  return next;
}

bool isLegacyKOSyncSetting(const char* key) {
  if (key == nullptr) return false;
  return std::strcmp(key, "koServerUrl") == 0 || std::strcmp(key, "koUsername") == 0 ||
         std::strcmp(key, "koPassword") == 0 || std::strcmp(key, "koMatchMethod") == 0 ||
         std::strcmp(key, "koSendMetadata") == 0 || std::strcmp(key, "koSyncBehavior") == 0 ||
         std::strcmp(key, "koAutoSync") == 0;
}

bool isLegacySecretSetting(const char* key) { return key != nullptr && std::strcmp(key, "koPassword") == 0; }

}  // namespace SyncSettingsWebPolicy
