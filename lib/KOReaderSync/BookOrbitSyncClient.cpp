#include "BookOrbitSyncClient.h"

#include <ArduinoJson.h>
#include <BookOrbitCredentialStore.h>
#include <Logging.h>
#include <Utf8.h>

#include <cmath>
#include <string>

#include "DefaultKOSyncTransport.h"
#include "KOSyncRequestHelpers.h"
#include "ProgressTimestamp.h"

// BookOrbitCredentialStore.h above is included via angle brackets, matching
// KOReaderSyncClient.cpp's own convention, so a host test's stub store can win the -I search
// order ahead of the real header sitting next to this .cpp (see test/bookorbit_sync/host_stubs/).
//
// This file never includes the real transport (freeink::SecureHttpClient) directly -- see
// DefaultKOSyncTransport.h -- so the protocol logic here is host-testable against a fake
// KOSyncTransport with no device/network/TLS stack at all, exactly like KOReaderSyncClient.cpp.

namespace {

// Bounds an HTTP status onto this client's Error categories. Identical rule set to
// KOReaderSyncClient.cpp's categorizeStatsStatus (deliberately -- both providers speak the same
// KOSync-derived wire dialect), duplicated rather than shared because the two now have distinct
// Error enums by design (see BookOrbitSyncClient.h's class comment).
BookOrbitSyncClient::Error categorizeStatus(const int httpCode) {
  if (httpCode <= 0) return BookOrbitSyncClient::NETWORK_ERROR;
  if (KOSyncRequest::isHttpSuccess(httpCode)) return BookOrbitSyncClient::OK;
  if (httpCode == 404 || httpCode == 405 || httpCode == 501) return BookOrbitSyncClient::UNSUPPORTED;
  if (httpCode == 400 || httpCode == 413 || httpCode == 422) return BookOrbitSyncClient::BAD_REQUEST;
  if (httpCode == 401) return BookOrbitSyncClient::AUTH_FAILED;
  return BookOrbitSyncClient::SERVER_ERROR;
}

bool sameHash(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++) {
    const char ca = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
    const char cb = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] - 'A' + 'a') : b[i];
    if (ca != cb) return false;
  }
  return true;
}

void applyBookOrbitAuthHeaders(KOSyncTransport& http) {
  // Explicit-credential overload (KOSyncRequestHelpers.h) so this client depends on
  // BookOrbitCredentialStore alone, never on KOReaderCredentialStore.
  KOSyncRequest::applyAuthHeaders(http, BOOKORBIT_STORE.getUsername(), BOOKORBIT_STORE.getMd5Password(),
                                  BOOKORBIT_STORE.getPassword());
}

void logOutcome(const char* method, int httpCode, BookOrbitSyncClient::Error result) {
  LOG_INF("BookOrbit", "%s: http=%d (%s)", method, httpCode, BookOrbitSyncClient::errorString(result));
}

// Fallback only: every caller (KOReaderAutoSync's push/pull) sets progress.device explicitly
// from CrossPointSettings' user-configurable device name, mirroring KOReaderSyncClient.cpp's own
// DEVICE_NAME fallback.
constexpr char PROGRESS_DEVICE_NAME_FALLBACK[] = "CrossPoint";

// UTF-8-safe truncation for server-supplied text (title): a raw byte cut can sever a multi-byte
// codepoint, which matters on this zh-TW branch. Mirrors BookOrbitCatalogClient.cpp's own
// boundedString() helper (not shared between the two .cpp files -- each is a small, self-
// contained anonymous-namespace utility, matching this codebase's existing duplication-over-
// coupling precedent for the two BookOrbit clients).
std::string boundedString(const char* value, size_t maxChars) {
  if (!value) return "";
  std::string s(value);
  if (s.size() > maxChars) {
    s.resize(static_cast<size_t>(utf8SafeTruncateBuffer(s.data(), static_cast<int>(maxChars))));
  }
  return s;
}

}  // namespace

const char* BookOrbitSyncClient::errorString(Error error) {
  switch (error) {
    case PROGRESS_NOT_CONFIRMED:
      return "Push not confirmed by readback";
    case OK:
      return "Success";
    case NO_CREDENTIALS:
      return "No credentials configured";
    case NETWORK_ERROR:
      return "Network error";
    case AUTH_FAILED:
      return "Authentication failed";
    case SERVER_ERROR:
      return "Server error (try again later)";
    case JSON_ERROR:
      return "JSON parse error";
    case LOW_MEMORY:
      return "Not enough memory for sync — please retry";
    case NOT_BOOKORBIT:
      return "Server did not identify itself as BookOrbit";
    case UNSUPPORTED:
      return "Not supported by this server";
    case BAD_REQUEST:
      return "Request rejected by server";
    case NOT_FOUND:
      return "No progress recorded on server";
  }
  return "Unknown error";
}

BookOrbitSyncClient::Session::Session() : transport_(makeDefaultKOSyncTransport()) {}

BookOrbitSyncClient::Session::Session(std::unique_ptr<KOSyncTransport> transport) : transport_(std::move(transport)) {}

BookOrbitSyncClient::Session::~Session() {
  if (transport_) transport_->end();
}

BookOrbitSyncClient::Error BookOrbitSyncClient::Session::authenticate(const AbortCallback& shouldAbort) {
  lastHttpCode_ = 0;
  if (!BOOKORBIT_STORE.hasCredentials()) {
    LOG_DBG("BookOrbit", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url = BOOKORBIT_STORE.getBaseUrl() + "/users/auth";
  LOG_DBG("BookOrbit", "Authenticating: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (KOSyncRequest::insufficientHeap()) return LOW_MEMORY;

  if (!transport_->begin(url)) {
    LOG_ERR("BookOrbit", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  applyBookOrbitAuthHeaders(*transport_);

  std::string responseBody;
  const int httpCode = transport_->sendRequest(
      "GET", nullptr, 0,
      [&responseBody](const uint8_t* data, size_t len) {
        constexpr size_t MAX_AUTH_BODY = 512;
        if (responseBody.size() < MAX_AUTH_BODY) {
          responseBody.append(reinterpret_cast<const char*>(data), std::min(len, MAX_AUTH_BODY - responseBody.size()));
        }
        return true;
      },
      shouldAbort);
  lastHttpCode_ = httpCode;

  Error result = categorizeStatus(httpCode);
  if (result == OK) {
    // BookOrbit's users/auth echoes {"authorized":"OK"} on success (koreader-auth response
    // shape). A 2xx alone is not proof -- a reverse proxy or SPA fallback can answer anything --
    // so an explicit denial or an unparseable body must not read as a successful login.
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, responseBody.c_str());
    if (error) {
      LOG_DBG("BookOrbit", "Auth response unparseable (%s)", error.c_str());
      result = JSON_ERROR;
    } else {
      // NOT `doc["authorized"] | nullptr`: ArduinoJson's operator| has a dedicated const-char*
      // overload, but a bare `nullptr` argument is deduced as std::nullptr_t by the more generic
      // templated overload first (an exact-match template beats a converting non-template
      // candidate), which checks variant.is<std::nullptr_t>() -- never true for a JSON string --
      // so the field's actual value is discarded and this always evaluates to null. An explicit
      // cast forces the intended const-char* overload to be selected instead.
      const char* authorized = doc["authorized"] | static_cast<const char*>(nullptr);
      if (authorized && std::string(authorized) != "OK") {
        LOG_DBG("BookOrbit", "Auth response explicitly denied authorization");
        result = AUTH_FAILED;
      }
    }
  }

  logOutcome("GET /users/auth", httpCode, result);
  return result;
}

BookOrbitSyncClient::Error BookOrbitSyncClient::Session::probeCapabilities(Capabilities& out,
                                                                           const AbortCallback& shouldAbort) {
  out = Capabilities{};
  lastHttpCode_ = 0;
  if (!BOOKORBIT_STORE.hasCredentials()) {
    LOG_DBG("BookOrbit", "No credentials configured");
    return NO_CREDENTIALS;
  }

  // Identity probe against the configured server -- never a hostname match, since BookOrbit is
  // self-hosted on any domain. GET .../plugin/version exists only on BookOrbit
  // (koreader-plugin.controller.ts @Get('version')).
  const std::string url = BOOKORBIT_STORE.getBaseUrl() + "/plugin/version";
  LOG_DBG("BookOrbit", "Probing capabilities: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (KOSyncRequest::insufficientHeap()) return LOW_MEMORY;

  if (!transport_->begin(url)) {
    LOG_ERR("BookOrbit", "Bad URL: %s", url.c_str());
    return BAD_REQUEST;
  }
  applyBookOrbitAuthHeaders(*transport_);

  std::string responseBody;
  const int httpCode = transport_->sendRequest(
      "GET", nullptr, 0,
      [&responseBody](const uint8_t* data, size_t len) {
        // Bound the probe response: a misconfigured URL could stream a whole HTML page at us, and
        // the fields needed sit in the first few hundred bytes of a small JSON object.
        constexpr size_t MAX_PROBE_BODY = 2048;
        if (responseBody.size() < MAX_PROBE_BODY) {
          responseBody.append(reinterpret_cast<const char*>(data), std::min(len, MAX_PROBE_BODY - responseBody.size()));
        }
        return true;
      },
      shouldAbort);
  lastHttpCode_ = httpCode;

  Error result = categorizeStatus(httpCode);
  if (result == OK) {
    // Require the typed version fingerprint -- KoreaderPluginVersionInfo:
    // { pluginVersion: string, serverVersion: string, capabilities: array } -- with all three
    // fields present and of the right type. A generic 2xx body that merely mentions
    // "serverVersion" is not proof; NOT_BOOKORBIT (not a cacheable negative) lets the caller
    // treat this as a configuration problem rather than "confirmed plain KOSync server", which
    // would be true only for a probe that ran against a server the user did NOT explicitly
    // configure as BookOrbit -- see BookOrbitStatsSync.cpp's use of this result.
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, responseBody.c_str());
    if (error) {
      LOG_DBG("BookOrbit", "Capability probe: 2xx but unparseable body (%s)", error.c_str());
      result = JSON_ERROR;
    } else if (!doc["pluginVersion"].is<const char*>() || !doc["serverVersion"].is<const char*>() ||
               !doc["capabilities"].is<JsonArrayConst>()) {
      result = NOT_BOOKORBIT;
    } else {
      out.isBookOrbit = true;
      for (JsonVariantConst cap : doc["capabilities"].as<JsonArrayConst>()) {
        const char* name = cap.as<const char*>();
        if (!name) continue;
        const std::string capName = name;
        if (capName == "bookmarkSync") {
          out.hasBookmarkSync = true;
        } else if (capName == "catalogBulkManifest") {
          out.hasCatalogBulkManifest = true;
        } else if (capName == "catalogDashboardSections") {
          out.hasCatalogDashboardSections = true;
        }
        // Unrecognized capability strings are ignored, not stored -- see Capabilities' doc
        // comment: this device only ever needs to ask about a small, known set.
      }
    }
  }

  logOutcome("GET /plugin/version", httpCode, result);
  return result;
}

BookOrbitSyncClient::Error BookOrbitSyncClient::Session::getProgress(const std::string& documentHash,
                                                                     BookOrbitProgress& outProgress,
                                                                     const AbortCallback& shouldAbort) {
  outProgress = BookOrbitProgress{};
  lastHttpCode_ = 0;
  if (!BOOKORBIT_STORE.hasCredentials()) {
    LOG_DBG("BookOrbit", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url = BOOKORBIT_STORE.getBaseUrl() + "/syncs/progress/" + documentHash;
  LOG_DBG("BookOrbit", "Getting progress: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (KOSyncRequest::insufficientHeap()) return LOW_MEMORY;

  if (!transport_->begin(url)) {
    LOG_ERR("BookOrbit", "Bad URL: %s", url.c_str());
    return BAD_REQUEST;
  }
  applyBookOrbitAuthHeaders(*transport_);

  std::string responseBody;
  const int httpCode = transport_->sendRequest(
      "GET", nullptr, 0,
      [&responseBody](const uint8_t* data, size_t len) {
        // Bounded, unlike KOReaderSyncClient::Session::getProgress's own unbounded accumulator: a
        // progress document is a handful of short fields, never a reason to let this grow.
        constexpr size_t MAX_PROGRESS_BODY = 1024;
        if (responseBody.size() < MAX_PROGRESS_BODY) {
          responseBody.append(reinterpret_cast<const char*>(data),
                              std::min(len, MAX_PROGRESS_BODY - responseBody.size()));
        }
        return true;
      },
      shouldAbort);
  lastHttpCode_ = httpCode;

  Error result;
  if (httpCode <= 0) {
    result = NETWORK_ERROR;
  } else if (httpCode == 204) {
    // No Content: mirrors KOReaderSyncClient::Session::getProgress's own handling of servers that
    // use 204 rather than 404 for "no progress recorded yet". Never hand responseBody to the JSON
    // parser below in this case.
    result = NOT_FOUND;
  } else if (KOSyncRequest::isHttpSuccess(httpCode)) {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, responseBody.c_str());
    const JsonVariantConst echoedDocument = doc["document"];
    if (error || !doc.is<JsonObjectConst>()) {
      LOG_ERR("BookOrbit", "JSON parse failed: %s", error ? error.c_str() : "not an object");
      result = JSON_ERROR;
    } else if (doc.as<JsonObjectConst>().size() == 0) {
      // The official server's own no-progress contract (koreader.controller.ts:
      // `return progress ?? {}`) is an HTTP 200 carrying an EMPTY object body, not a 404/204 --
      // treat it exactly like those, not as a malformed response. Verified against
      // koreader.service.ts's getProgress() returning null when nothing is recorded.
      result = NOT_FOUND;
    } else if (!doc["percentage"].is<float>() || !std::isfinite(doc["percentage"].as<float>()) ||
               doc["percentage"].as<float>() < 0.0f || doc["percentage"].as<float>() > 1.0f) {
      // Rejects a MISSING, non-numeric, non-finite, or out-of-[0,1] percentage -- but never a
      // present ZERO, which is a legitimate reset-to-start position (the server's own
      // reset-convergence mechanism resets a device's progress to 0 with a real xpath; see
      // koreader.service.ts's resolveResetHold()), not "no progress".
      LOG_ERR("BookOrbit", "Progress response missing/invalid percentage field");
      result = JSON_ERROR;
    } else if (!doc["progress"].is<const char*>()) {
      LOG_ERR("BookOrbit", "Progress response missing/invalid progress field");
      result = JSON_ERROR;
    } else if (!doc["timestamp"].is<int64_t>() || doc["timestamp"].as<int64_t>() < 0 ||
               !ProgressTimestamp::valid(static_cast<uint64_t>(doc["timestamp"].as<int64_t>()))) {
      LOG_ERR("BookOrbit", "Progress response missing/invalid Unix-seconds timestamp");
      result = JSON_ERROR;
    } else if (!echoedDocument.isUnbound() &&
               (!echoedDocument.is<const char*>() || !sameHash(documentHash, echoedDocument.as<const char*>()))) {
      // The official server echoes the requested document hash. Keep the field optional for
      // KOSync-compatible older deployments, but when a peer supplies it, reject a wrong type or
      // identity rather than applying a valid-looking position returned for another book.
      LOG_ERR("BookOrbit", "Progress response echoed a different/invalid document identity");
      result = JSON_ERROR;
    } else {
      outProgress.document = documentHash;
      outProgress.progress = doc["progress"].as<std::string>();
      outProgress.percentage = doc["percentage"].as<float>();
      outProgress.device = doc["device"].as<std::string>();
      outProgress.deviceId = doc["device_id"].as<std::string>();
      outProgress.timestamp = doc["timestamp"].as<int64_t>();
      LOG_DBG("BookOrbit", "Got progress: %.2f%% at %s", outProgress.percentage * 100, outProgress.progress.c_str());
      result = OK;
    }
  } else if (httpCode == 401) {
    result = AUTH_FAILED;
  } else if (httpCode == 404) {
    result = NOT_FOUND;
  } else {
    result = SERVER_ERROR;
  }

  logOutcome("GET /syncs/progress", httpCode, result);
  return result;
}

BookOrbitSyncClient::Error BookOrbitSyncClient::Session::updateProgress(const BookOrbitProgress& progress,
                                                                        const AbortCallback& shouldAbort) {
  lastHttpCode_ = 0;
  if (!BOOKORBIT_STORE.hasCredentials()) {
    LOG_DBG("BookOrbit", "No credentials configured");
    return NO_CREDENTIALS;
  }
  if (progress.document.size() != 32) {
    LOG_ERR("BookOrbit", "Refusing progress update: document hash is %u chars, expected 32",
            (unsigned)progress.document.size());
    return BAD_REQUEST;
  }
  if (progress.deviceId.empty()) {
    LOG_ERR("BookOrbit", "Refusing progress update without a persisted installation id");
    return BAD_REQUEST;
  }
  if (progress.timestamp < 0 || !ProgressTimestamp::valid(static_cast<uint64_t>(progress.timestamp))) {
    LOG_ERR("BookOrbit", "Refusing progress update without a valid origin timestamp");
    return BAD_REQUEST;
  }

  const std::string url = BOOKORBIT_STORE.getBaseUrl() + "/syncs/progress";
  LOG_DBG("BookOrbit", "Updating progress: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (KOSyncRequest::insufficientHeap()) return LOW_MEMORY;

  JsonDocument doc;
  doc["document"] = progress.document;
  doc["progress"] = progress.progress;
  doc["percentage"] = progress.percentage;
  doc["device"] = progress.device.empty() ? PROGRESS_DEVICE_NAME_FALLBACK : progress.device;
  // Per-installation id, not BookOrbitLimits::DEVICE_ID's shared literal -- see
  // BookOrbitProgress::deviceId's doc comment for why the old shared constant was wrong here (it
  // is NOT purely informational server-side: koreader.service.ts keys per-device progress rows
  // and reset-convergence tracking on it). Threaded in by the caller, like uploadPageStats() and
  // exchangeBookmarks() already do, rather than read from BookOrbitStats here -- this lib/ target
  // never depends on src/util/, where BookOrbitStats::statsDeviceId() lives.
  doc["device_id"] = progress.deviceId;
  // Official BookOrbit accepts Unix seconds here and preserves the supplied origin time. This is
  // intentionally absent from generic KOReaderSyncClient PUTs, whose protocol assigns receipt
  // time server-side instead.
  doc["timestamp"] = progress.timestamp;

  std::string body;
  serializeJson(doc, body);

  if (!transport_->begin(url)) {
    LOG_ERR("BookOrbit", "Bad URL: %s", url.c_str());
    return BAD_REQUEST;
  }
  applyBookOrbitAuthHeaders(*transport_);
  transport_->addHeader("Content-Type", "application/json");
  // Response body is unused here, unlike getProgress() -- discard it, still threading the abort
  // callback through so a PUT can be cancelled mid-flight the same way a GET can.
  const int httpCode = transport_->sendRequest(
      "PUT", reinterpret_cast<const uint8_t*>(body.data()), body.size(), [](const uint8_t*, size_t) { return true; },
      shouldAbort);
  lastHttpCode_ = httpCode;

  body.clear();
  body.shrink_to_fit();

  Error result;
  if (httpCode <= 0)
    result = NETWORK_ERROR;
  else if (KOSyncRequest::isHttpSuccess(httpCode))
    result = OK;
  else if (httpCode == 401)
    result = AUTH_FAILED;
  else
    result = SERVER_ERROR;

  logOutcome("PUT /syncs/progress", httpCode, result);
  return result;
}

BookOrbitSyncClient::Error BookOrbitSyncClient::Session::updateProgressAndVerify(
    const BookOrbitProgress& progress, const AbortCallback& shouldAbort) {
  const Error put = updateProgress(progress, shouldAbort);
  if (put != OK) return put;

  BookOrbitProgress selected;
  const Error get = getProgress(progress.document, selected, shouldAbort);
  if (get != OK && get != NOT_FOUND) return get;
  // The deployed PUT returns a receipt, not the selected reading position. A reset hold
  // or a concurrent reader can leave GET different despite 2xx. Never write again here.
  const bool matches = get == OK && selected.progress == progress.progress &&
                       std::fabs(selected.percentage - progress.percentage) <= 0.000001f &&
                       selected.timestamp == progress.timestamp;
  LOG_INF("BookOrbit", "Push readback: match=%u sent=%.6f@%lld selected=%.6f@%lld http=%d",
          static_cast<unsigned>(matches), progress.percentage, static_cast<long long>(progress.timestamp),
          selected.percentage, static_cast<long long>(selected.timestamp), lastHttpCode_);
  return matches ? OK : PROGRESS_NOT_CONFIRMED;
}

BookOrbitSyncClient::Error BookOrbitSyncClient::Session::uploadPageStats(
    const std::string& documentHash, const std::string& deviceId, const std::string& deviceModel,
    const BookOrbitStatEvent* events, const size_t count, BookOrbitStatsAck& outAck, const AbortCallback& shouldAbort) {
  outAck = BookOrbitStatsAck{};
  lastHttpCode_ = 0;
  if (!BOOKORBIT_STORE.hasCredentials()) {
    LOG_DBG("BookOrbit", "No credentials configured");
    return NO_CREDENTIALS;
  }
  if (!events || count == 0) return OK;
  if (count > BookOrbitLimits::MAX_EVENTS_PER_REQUEST) {
    LOG_ERR("BookOrbit", "Refusing page-stats batch of %u events (server cap %u)", (unsigned)count,
            (unsigned)BookOrbitLimits::MAX_EVENTS_PER_REQUEST);
    return BAD_REQUEST;
  }
  if (documentHash.size() != 32) {
    LOG_ERR("BookOrbit", "Refusing page-stats upload: document hash is %u chars, expected 32",
            (unsigned)documentHash.size());
    return BAD_REQUEST;
  }
  if (deviceId.empty()) {
    LOG_ERR("BookOrbit", "Refusing page-stats upload without a persisted installation id");
    return BAD_REQUEST;
  }

  const std::string url = BOOKORBIT_STORE.getBaseUrl() + "/plugin/page-stats";
  LOG_DBG("BookOrbit", "Uploading %u page-stat event(s): %s (heap: %u)", (unsigned)count, url.c_str(),
          (unsigned)ESP.getFreeHeap());
  if (KOSyncRequest::insufficientHeap()) return LOW_MEMORY;

  std::string body;
  {
    JsonDocument doc;
    doc["deviceId"] = deviceId;
    doc["deviceModel"] = deviceModel.size() > BookOrbitLimits::MAX_DEVICE_MODEL_CHARS
                             ? deviceModel.substr(0, BookOrbitLimits::MAX_DEVICE_MODEL_CHARS)
                             : deviceModel;
    doc["pluginVersion"] = BookOrbitLimits::PLUGIN_VERSION;

    const JsonArray books = doc["books"].to<JsonArray>();
    const JsonObject book = books.add<JsonObject>();
    book["hash"] = documentHash;
    const JsonArray jsonEvents = book["events"].to<JsonArray>();
    for (size_t i = 0; i < count; i++) {
      const JsonObject event = jsonEvents.add<JsonObject>();
      event["page"] = events[i].page;
      event["startTime"] = events[i].startTime;
      event["durationSeconds"] = events[i].durationSeconds;
      event["totalPages"] = events[i].totalPages;
    }
    serializeJson(doc, body);
  }

  if (!transport_->begin(url)) {
    LOG_ERR("BookOrbit", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  applyBookOrbitAuthHeaders(*transport_);
  transport_->addHeader("Content-Type", "application/json");

  std::string responseBody;
  const int httpCode = transport_->sendRequest(
      "POST", reinterpret_cast<const uint8_t*>(body.data()), body.size(),
      [&responseBody](const uint8_t* data, size_t len) {
        constexpr size_t MAX_ACK_BODY = 1024;
        if (responseBody.size() < MAX_ACK_BODY) {
          responseBody.append(reinterpret_cast<const char*>(data), std::min(len, MAX_ACK_BODY - responseBody.size()));
        }
        return true;
      },
      shouldAbort);
  lastHttpCode_ = httpCode;

  body.clear();
  body.shrink_to_fit();

  const Error result = categorizeStatus(httpCode);
  if (result == OK) {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, responseBody.c_str());
    if (error) {
      LOG_INF("BookOrbit", "Page-stats 2xx with unparseable body (%s); retaining events for retry", error.c_str());
    } else {
      for (JsonObjectConst entry : doc["results"].as<JsonArrayConst>()) {
        const char* hash = entry["hash"];
        if (!hash || !sameHash(documentHash, hash)) continue;
        outAck.accepted = entry["accepted"].as<uint32_t>();
        outAck.duplicates = entry["duplicates"].as<uint32_t>();
        outAck.matched = outAck.accepted + outAck.duplicates >= count;
        if (!outAck.matched) {
          LOG_INF("BookOrbit", "Page-stats acknowledged only %u of %u event(s); retaining the batch",
                  static_cast<unsigned>(outAck.accepted + outAck.duplicates), static_cast<unsigned>(count));
        }
      }
      for (JsonVariantConst entry : doc["unmatched"].as<JsonArrayConst>()) {
        const char* hash = entry.as<const char*>();
        if (hash && sameHash(documentHash, hash)) outAck.unmatched = true;
      }
      if (!outAck.matched && !outAck.unmatched) {
        LOG_INF("BookOrbit", "Page-stats 2xx did not mention our hash; retaining events for retry");
      }
    }
    if (outAck.unmatched) {
      LOG_INF("BookOrbit", "Page-stats accepted but no library book matches this file yet");
    }
  }

  logOutcome("POST /plugin/page-stats", httpCode, result);
  return result;
}

BookOrbitSyncClient::Error BookOrbitSyncClient::Session::exchangeBookmarks(
    const std::string& documentHash, const std::string& deviceId, const std::string& deviceModel,
    const BookOrbitBookmarkKeyRow* keys, const size_t keyCount, const bool keysComplete,
    const BookOrbitBookmark* changes, const size_t changeCount, bool& outUnmatched,
    std::vector<BookOrbitIncomingBookmark>& outIncoming, bool& outMorePending, const AbortCallback& shouldAbort) {
  outUnmatched = false;
  outIncoming.clear();
  outMorePending = false;
  lastHttpCode_ = 0;
  if (!BOOKORBIT_STORE.hasCredentials()) {
    LOG_DBG("BookOrbit", "No credentials configured");
    return NO_CREDENTIALS;
  }
  if (documentHash.size() != 32) {
    LOG_ERR("BookOrbit", "Refusing bookmark exchange: document hash is %u chars, expected 32",
            (unsigned)documentHash.size());
    return BAD_REQUEST;
  }
  if (deviceId.empty()) {
    LOG_ERR("BookOrbit", "Refusing bookmark exchange without a persisted installation id");
    return BAD_REQUEST;
  }
  // No early return for an empty payload: this request is the only thing that brings the
  // server's own bookmark changes down, so skipping it when the device has nothing new to say is
  // exactly how a device stops hearing about web-created bookmarks and deletions.

  const std::string url = BOOKORBIT_STORE.getBaseUrl() + "/plugin/bookmarks/exchange";
  LOG_DBG("BookOrbit", "Exchanging %u bookmark(s), %u key(s) (heap: %u)", (unsigned)changeCount, (unsigned)keyCount,
          (unsigned)ESP.getFreeHeap());
  if (KOSyncRequest::insufficientHeap()) return LOW_MEMORY;

  std::string body;
  {
    JsonDocument doc;
    doc["deviceId"] = deviceId;
    doc["deviceModel"] = deviceModel;
    doc["pluginVersion"] = BookOrbitLimits::PLUGIN_VERSION;

    const JsonArray books = doc["books"].to<JsonArray>();
    const JsonObject book = books.add<JsonObject>();
    book["hash"] = documentHash;
    book["keysComplete"] = keysComplete;
    const JsonArray jsonKeys = book["keys"].to<JsonArray>();
    for (size_t i = 0; keys && i < keyCount; i++) {
      const JsonObject key = jsonKeys.add<JsonObject>();
      key["k"] = keys[i].k;
      key["dt"] = keys[i].dt;
    }
    const JsonArray jsonChanges = book["changes"].to<JsonArray>();
    for (size_t i = 0; changes && i < changeCount; i++) {
      const BookOrbitBookmark& bookmark = changes[i];
      const JsonObject entry = jsonChanges.add<JsonObject>();
      entry["datetime"] = bookmark.datetime;
      entry["pos"] = bookmark.pos;
      if (bookmark.pageno > 0) entry["pageno"] = bookmark.pageno;
    }
    serializeJson(doc, body);
  }

  if (!transport_->begin(url)) {
    LOG_ERR("BookOrbit", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  applyBookOrbitAuthHeaders(*transport_);
  transport_->addHeader("Content-Type", "application/json");

  std::string responseBody;
  const int httpCode = transport_->sendRequest(
      "POST", reinterpret_cast<const uint8_t*>(body.data()), body.size(),
      [&responseBody](const uint8_t* data, size_t len) {
        // Bounded well above a realistic BOOKORBIT_BOOKMARK_BATCH-sized response: each entry
        // carries at most a position and a title, never arbitrary text.
        constexpr size_t MAX_EXCHANGE_BODY = 8192;
        if (responseBody.size() < MAX_EXCHANGE_BODY) {
          responseBody.append(reinterpret_cast<const char*>(data),
                              std::min(len, MAX_EXCHANGE_BODY - responseBody.size()));
        }
        return true;
      },
      shouldAbort);
  lastHttpCode_ = httpCode;
  body.clear();
  body.shrink_to_fit();

  const Error result = categorizeStatus(httpCode);
  if (result == OK) {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, responseBody.c_str());
    if (error) {
      LOG_ERR("BookOrbit", "Bookmark exchange 2xx with unparseable body (%s)", error.c_str());
      logOutcome("POST /plugin/bookmarks/exchange", httpCode, JSON_ERROR);
      return JSON_ERROR;
    }

    for (JsonVariantConst hash : doc["unmatched"].as<JsonArrayConst>()) {
      const char* h = hash.as<const char*>();
      if (h && sameHash(documentHash, h)) {
        outUnmatched = true;
        logOutcome("POST /plugin/bookmarks/exchange", httpCode, result);
        return OK;
      }
    }

    // Never trust `results[0]` positionally: the server's own BookmarkExchangeBookResult always
    // carries a `hash`, and requires matching it (exactly like the `unmatched` scan just above)
    // rather than assuming index 0 answers for the one book this request named. An empty
    // `results` array, or one whose only entry answers for a DIFFERENT hash, is otherwise
    // indistinguishable from a genuine confirmation once toApply/more/skippedNoPosition all
    // default emptily -- and the caller (BookOrbitBookmarkSync::sync()) would then mark this
    // round's outgoing bookmarks permanently synced without the server ever having confirmed
    // them.
    JsonObjectConst matchedResult;
    bool foundResult = false;
    for (JsonObjectConst entry : doc["results"].as<JsonArrayConst>()) {
      const char* hash = entry["hash"] | static_cast<const char*>(nullptr);
      if (hash && sameHash(documentHash, hash)) {
        matchedResult = entry;
        foundResult = true;
        break;
      }
    }
    if (!foundResult) {
      LOG_ERR("BookOrbit", "Bookmark exchange 2xx with no result for the requested book");
      logOutcome("POST /plugin/bookmarks/exchange", httpCode, JSON_ERROR);
      return JSON_ERROR;
    }

    outMorePending = (matchedResult["more"] | false) || (matchedResult["skippedNoPosition"] | 0U) > 0;

    const JsonObjectConst toApply = matchedResult["toApply"].as<JsonObjectConst>();
    const auto collect = [&](const char* field, const bool deleted) {
      for (JsonObjectConst entry : toApply[field].as<JsonArrayConst>()) {
        if (outIncoming.size() >= BOOKORBIT_BOOKMARK_BATCH) {
          LOG_INF("BookOrbit", "More server-side bookmark changes remain; they arrive next sync");
          return;
        }
        const uint32_t serverId = entry["serverId"] | 0U;
        // The server re-offers everything not yet acknowledged, so a later pull round repeats
        // earlier rounds' entries; appending them again would apply and ack each one twice.
        bool alreadyCollected = false;
        for (const auto& seen : outIncoming) {
          if (seen.serverId == serverId) {
            alreadyCollected = true;
            break;
          }
        }
        if (alreadyCollected) continue;
        const char* pos = entry["pos"] | static_cast<const char*>(nullptr);
        const char* key = entry["key"] | static_cast<const char*>(nullptr);
        // Each kind carries what it needs and nothing more: a deletion has a key, an add has a
        // position. Demanding one of both is what would drop every deletion on the floor.
        if (serverId == 0 || (deleted ? !key || !*key : !pos || !*pos)) continue;

        BookOrbitIncomingBookmark incoming;
        incoming.serverId = serverId;
        incoming.key = boundedString(key, BookOrbitBookmarkKeyRow::DIGEST_SIZE - 1);
        incoming.pos = boundedString(pos, BOOKORBIT_BOOKMARK_POS_MAX_CHARS);
        incoming.title =
            boundedString(entry["title"] | static_cast<const char*>(nullptr), BOOKORBIT_BOOKMARK_TITLE_MAX_CHARS);
        incoming.pageno = entry["pageno"] | 0;
        incoming.deleted = deleted;
        outIncoming.push_back(std::move(incoming));
      }
    };
    collect("add", false);
    collect("delete", true);
    if (!outIncoming.empty()) {
      LOG_INF("BookOrbit", "Server sent %u bookmark change(s)", (unsigned)outIncoming.size());
    }
  }

  logOutcome("POST /plugin/bookmarks/exchange", httpCode, result);
  return result;
}

BookOrbitSyncClient::Error BookOrbitSyncClient::Session::ackBookmarks(const std::string& documentHash,
                                                                      const std::string& deviceId,
                                                                      const std::string& deviceModel,
                                                                      const std::vector<BookOrbitBookmarkAck>& applied,
                                                                      const std::vector<uint32_t>& deletedServerIds,
                                                                      const AbortCallback& shouldAbort) {
  lastHttpCode_ = 0;
  if (!BOOKORBIT_STORE.hasCredentials()) {
    LOG_DBG("BookOrbit", "No credentials configured");
    return NO_CREDENTIALS;
  }
  if (documentHash.size() != 32) return BAD_REQUEST;
  if (applied.empty() && deletedServerIds.empty()) return OK;
  if (deviceId.empty()) {
    LOG_ERR("BookOrbit", "Refusing bookmark acknowledgement without a persisted installation id");
    return BAD_REQUEST;
  }

  const std::string url = BOOKORBIT_STORE.getBaseUrl() + "/plugin/bookmarks/exchange-ack";
  LOG_DBG("BookOrbit", "Acking %u applied, %u deleted bookmark(s) (heap: %u)", (unsigned)applied.size(),
          (unsigned)deletedServerIds.size(), (unsigned)ESP.getFreeHeap());
  if (KOSyncRequest::insufficientHeap()) return LOW_MEMORY;

  std::string body;
  {
    JsonDocument doc;
    doc["deviceId"] = deviceId;
    doc["deviceModel"] = deviceModel;
    doc["pluginVersion"] = BookOrbitLimits::PLUGIN_VERSION;
    const JsonArray books = doc["books"].to<JsonArray>();
    const JsonObject book = books.add<JsonObject>();
    book["hash"] = documentHash;
    const JsonArray appliedArray = book["applied"].to<JsonArray>();
    for (const BookOrbitBookmarkAck& ack : applied) {
      const JsonObject entry = appliedArray.add<JsonObject>();
      entry["serverId"] = ack.serverId;
      entry["status"] = ack.failed ? "failed" : "applied";
      // The local identity this device minted; it is what links the server's bookmark here.
      if (!ack.failed && !ack.key.empty()) {
        entry["key"] = ack.key;
        entry["datetime"] = ack.datetime;
        entry["pos"] = ack.pos;
      }
    }
    const JsonArray deletedArray = book["deleted"].to<JsonArray>();
    for (const uint32_t serverId : deletedServerIds) {
      const JsonObject entry = deletedArray.add<JsonObject>();
      entry["serverId"] = serverId;
      entry["status"] = "applied";
    }
    serializeJson(doc, body);
  }

  if (!transport_->begin(url)) {
    LOG_ERR("BookOrbit", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  applyBookOrbitAuthHeaders(*transport_);
  transport_->addHeader("Content-Type", "application/json");

  const int httpCode = transport_->sendRequest(
      "POST", reinterpret_cast<const uint8_t*>(body.data()), body.size(), [](const uint8_t*, size_t) { return true; },
      shouldAbort);
  lastHttpCode_ = httpCode;
  body.clear();
  body.shrink_to_fit();

  const Error result = categorizeStatus(httpCode);
  logOutcome("POST /plugin/bookmarks/exchange-ack", httpCode, result);
  return result;
}
