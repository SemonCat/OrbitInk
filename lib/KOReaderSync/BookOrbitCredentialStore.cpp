#include "BookOrbitCredentialStore.h"

#include <Logging.h>
#include <MD5Builder.h>
#include <ObfuscationUtils.h>

namespace {
// BookOrbit's kosync-compatible endpoints (users/auth, syncs/progress, plugin/*) live under this
// path prefix, unlike a plain koreader-sync server which serves them at the root. Adapted from
// CrossInk-Bookorbit's BookOrbitCredentialStore.cpp (MIT, see BookOrbitCredentialStore.h).
constexpr char API_PREFIX[] = "/api/v1/koreader";
constexpr char API_V1_SUFFIX[] = "/api/v1";
}  // namespace

void BookOrbitCredentialStore::toJson(JsonDocument& doc) const {
  doc["username"] = getUsername();
  doc["password_obf"] = obfuscation::obfuscateToBase64(getPassword());
  doc["serverUrl"] = getServerUrl();
}

bool BookOrbitCredentialStore::fromJson(JsonVariantConst doc) {
  std::string user = doc["username"] | "";

  bool needsResave = false;
  std::string pass = extractPassword(doc, needsResave);

  setCredentials(user, pass);
  setServerUrl(doc["serverUrl"] | "");

  if (needsResave) {
    LOG_DBG("BOS", "Resaving BookOrbit credentials to update format");
    requestResave();
  }

  return true;
}

void BookOrbitCredentialStore::setCredentials(const std::string& user, const std::string& pass) {
  username = user;
  password = pass;
  LOG_DBG("BOS", "Set credentials for user: %s", user.c_str());
}

std::string BookOrbitCredentialStore::getMd5Password() const {
  if (password.empty()) {
    return "";
  }

  MD5Builder md5;
  md5.begin();
  md5.add(password.c_str());
  md5.calculate();

  return md5.toString().c_str();
}

bool BookOrbitCredentialStore::hasCredentials() const {
  return !username.empty() && !password.empty() && !serverUrl.empty();
}

void BookOrbitCredentialStore::clearCredentials() {
  username.clear();
  password.clear();
  saveToFile();
  LOG_DBG("BOS", "Cleared BookOrbit credentials");
}

void BookOrbitCredentialStore::setServerUrl(const std::string& url) {
  serverUrl = url;
  LOG_DBG("BOS", "Set server URL: %s", url.empty() ? "(none)" : url.c_str());
}

std::string BookOrbitCredentialStore::getBaseUrl() const {
  if (serverUrl.empty()) {
    return "";
  }

  std::string url;
  if (serverUrl.find("://") == std::string::npos) {
    // BookOrbit is self-hosted and typically deployed behind TLS; unlike
    // KOReaderCredentialStore there is no well-known plaintext-LAN convention to default to.
    url = "https://" + serverUrl;
  } else {
    url = serverUrl;
  }

  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }

  // Strip a trailing "/api/v1" or "/api/v1/koreader" the user may have pasted from BookOrbit's
  // own settings page, then append our own canonical prefix -- so re-pasting the same value
  // twice, or pasting either form, normalizes to the same base URL (and the same owner
  // fingerprint; see BookOrbitOwnerId).
  constexpr size_t prefixLen = sizeof(API_PREFIX) - 1;
  constexpr size_t v1Len = sizeof(API_V1_SUFFIX) - 1;
  if (url.size() >= prefixLen && url.compare(url.size() - prefixLen, prefixLen, API_PREFIX) == 0) {
    url.resize(url.size() - prefixLen);
  } else if (url.size() >= v1Len && url.compare(url.size() - v1Len, v1Len, API_V1_SUFFIX) == 0) {
    url.resize(url.size() - v1Len);
  }

  return url + API_PREFIX;
}
