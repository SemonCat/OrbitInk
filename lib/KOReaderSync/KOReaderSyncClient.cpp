#include "KOReaderSyncClient.h"

#include <ArduinoJson.h>
#include <KOReaderCredentialStore.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

#include "DefaultKOSyncTransport.h"
#include "KOSyncRequestHelpers.h"
#include "ProgressTimestamp.h"

// KOReaderCredentialStore.h above is deliberately included via angle brackets, not quotes: a
// quoted #include always searches the including file's OWN directory first, which would make it
// impossible for test/kosync_protocol/'s host stub (a fake, test-settable credential store, with
// no PersistableStore/SD/MD5Builder dependency) to take priority over the real header sitting
// right next to this .cpp. Angle brackets let the -I search order decide, matching how every
// other cross-lib include in this codebase already works. This resolves to the exact same real
// header in the firmware build (PlatformIO's LDF adds every lib/*/ directory to the global
// include path), so production behavior is unchanged.
//
// This file never includes the real transport (freeink::SecureHttpClient, and transitively
// Arduino.h/WiFiClient.h/wolfSSL) directly -- see DefaultKOSyncTransport.h -- specifically so its
// PROTOCOL logic (JSON building/parsing, HTTP-status categorization) can be host-tested against
// a fake KOSyncTransport with no device/network/TLS stack at all.

int KOReaderSyncClient::lastHttpCode = 0;

namespace {
// Fallback device name if a caller doesn't populate KOReaderProgress::device -- see its use below.
constexpr char DEVICE_NAME[] = "CrossPoint";
// Sourced from BookOrbitLimits: this constant predates BookOrbit becoming a standalone provider
// with its own device id (see BookOrbitStats::statsDeviceId()) and stays here unchanged, since
// changing it would orphan every device's server-side KOSync progress record.
constexpr const char* DEVICE_ID = BookOrbitLimits::DEVICE_ID;
// A valid KOSync progress object is normally a few hundred bytes, including the optional rich
// position. Four KiB leaves ample room for compatible server extensions while ensuring a web
// frontend or other incompatible endpoint can never grow the response accumulator without bound.
constexpr size_t MAX_PROGRESS_RESPONSE_BODY = 4096;

bool isJsonContentType(std::string contentType) {
  const size_t parameters = contentType.find(';');
  if (parameters != std::string::npos) contentType.resize(parameters);
  const size_t first = contentType.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return false;
  const size_t last = contentType.find_last_not_of(" \t\r\n");
  contentType = contentType.substr(first, last - first + 1);
  std::transform(contentType.begin(), contentType.end(), contentType.begin(),
                 [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (contentType == "application/json") return true;
  return contentType.rfind("application/", 0) == 0 && contentType.size() > 5 &&
         contentType.compare(contentType.size() - 5, 5, "+json") == 0;
}

// Request-shaping helpers live in KOSyncRequestHelpers.h so this client and the standalone
// BookOrbitSyncClient provably apply the SAME auth-header shape, the same TLS heap gate, and the
// same "any 2xx is success" rule.
using KOSyncRequest::insufficientHeap;
using KOSyncRequest::isHttpSuccess;

// Trivial KOREADER_STORE-reading wrapper around the shared, credential-agnostic helper -- kept
// local rather than in KOSyncRequestHelpers.h so that shared header never references
// KOReaderCredentialStore.h (see its own doc comment). Named identically to the pre-refactor
// helper so every call site below is unchanged from ee4490f.
void applyAuthHeaders(KOSyncTransport& http) {
  KOSyncRequest::applyAuthHeaders(http, KOREADER_STORE.getUsername(), KOREADER_STORE.getMd5Password(),
                                  KOREADER_STORE.getPassword());
}
using KOSyncRequest::logOutcome;

// KOSync's TLS-1.3 servers can't be reached through the precompiled system
// mbedTLS (TLS 1.3 is stubbed out), so requests run over wolfSSL via
// SecureHttpClient. The handshake still needs working heap; gate on it. wolfSSL's
// footprint is smaller than mbedTLS's old ~48KB peak, but keep a conservative
// floor. Check both total free heap and largest contiguous block so fragmented
// heap does not fall through into a failed TLS allocation path.
// MEMFIX-PORT: TLS heap gate; portable
// Field data (July 2026): launching sync from a reader session lands at
// 51.9-58.2 KB free / 42-53 KB maxAlloc after WiFi comes up. wolfSSL handles
// allocation failure by returning MEMORY_E (no abort under -fno-exceptions),
// so an optimistic attempt degrades to the same clean "sync failed" as the
// gate — the gate only needs to keep out states where a doomed handshake
// would waste tens of seconds, not guarantee success.
//
// Free and largest-block have separate requirements: with SP ECC
// (WOLFSSL_HAVE_SP_ECC) the handshake's crypto uses fixed 256-bit arrays, so
// the largest single TLS allocation is the ~17 KB wolfSSL record buffer, not
// a run of fast-math bignums. A handshake was measured succeeding inside a
// 43 KB largest block; requiring 50 KB contiguous refused syncs that fit.
//
// The 35 KB free floor covers the measured peak of what remains after the SP
// ECC + X25519 work: session object plus record buffer plus RSA cert-verify
// temps (2 KB apiece at FP_MAX_BITS 8192) totals ~30-40 KB transient. The old
// 50 KB floor was calibrated against the fast-math bignum failure mode that
// SP ECC removed, and sat inside the 51.9-58.2 KB band a reading session
// normally leaves, refusing syncs that would have succeeded. A wrong guess
// here fails soft: MEMORY_E aborts the handshake within its 15 s deadline.
//
// UNCHANGED by connection reuse (Session, below): reuse means this gate is
// evaluated once per transaction's first request rather than once per
// request, since a reused connection's later requests skip the handshake
// this floor protects -- it still runs before every request regardless, so a
// heap drop mid-transaction (e.g. from a page render between GET and PUT) is
// still caught before the next request, not just the first.
// The two figures themselves now live in KOSyncRequestHelpers.h (MIN_FREE_FOR_TLS /
// MIN_BLOCK_FOR_TLS) alongside insufficientHeap(), unchanged in value and behavior; the
// calibration record above stays here, with the code it was measured against.
}  // namespace

KOReaderSyncClient::Session::Session() : transport_(makeDefaultKOSyncTransport()) {}

KOReaderSyncClient::Session::Session(std::unique_ptr<KOSyncTransport> transport) : transport_(std::move(transport)) {}

KOReaderSyncClient::Session::~Session() {
  if (transport_) transport_->end();
}

KOReaderSyncClient::Error KOReaderSyncClient::Session::authenticate() {
  lastHttpCode_ = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/users/auth";
  LOG_DBG("KOSync", "Authenticating: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

  if (!transport_->begin(url)) {
    LOG_ERR("KOSync", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  applyAuthHeaders(*transport_);
  const int httpCode = transport_->sendRequest("GET", nullptr, 0);
  lastHttpCode_ = httpCode;
  KOReaderSyncClient::lastHttpCode = httpCode;

  Error result;
  if (httpCode <= 0)
    result = NETWORK_ERROR;
  else if (isHttpSuccess(httpCode))
    result = OK;
  else if (httpCode == 401)
    result = AUTH_FAILED;
  else
    result = SERVER_ERROR;

  logOutcome("GET /users/auth", httpCode, result, nullptr);
  return result;
}

KOReaderSyncClient::Error KOReaderSyncClient::Session::createUser() {
  lastHttpCode_ = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/users/create";
  LOG_DBG("KOSync", "Creating account: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

  JsonDocument doc;
  doc["username"] = KOREADER_STORE.getUsername();
  doc["password"] = KOREADER_STORE.getMd5Password();
  std::string body;
  serializeJson(doc, body);

  if (!transport_->begin(url)) {
    LOG_ERR("KOSync", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  transport_->addHeader("Accept", "application/vnd.koreader.v1+json");
  transport_->addHeader("Content-Type", "application/json");
  const int httpCode = transport_->sendRequest("POST", reinterpret_cast<const uint8_t*>(body.data()), body.size());
  lastHttpCode_ = httpCode;
  KOReaderSyncClient::lastHttpCode = httpCode;

  Error result;
  if (httpCode <= 0)
    result = NETWORK_ERROR;
  else if (isHttpSuccess(httpCode))
    result = OK;
  else if (httpCode == 402)
    result = USER_EXISTS;
  else
    result = SERVER_ERROR;

  logOutcome("POST /users/create", httpCode, result, nullptr);
  return result;
}

KOReaderSyncClient::Error KOReaderSyncClient::Session::getProgress(const std::string& documentHash,
                                                                   KOReaderProgress& outProgress,
                                                                   const AbortCallback& shouldAbort,
                                                                   const char* hashKind) {
  lastHttpCode_ = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress/" + documentHash;
  LOG_DBG("KOSync", "Getting progress: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

  if (!transport_->begin(url)) {
    LOG_ERR("KOSync", "Bad URL: %s", url.c_str());
    return BAD_REQUEST;
  }
  applyAuthHeaders(*transport_);
  // Own strictly bounded accumulator: never use the transport's unbounded getString() path here.
  // Headers have already been read when SecureHttpClient invokes onData, so an incompatible
  // Content-Type can stop the transfer before even its first body chunk is retained.
  std::string responseBody;
  bool responseTypeChecked = false;
  bool incompatibleResponse = false;
  bool responseTooLarge = false;
  bool sawFirstNonWhitespace = false;
  const int httpCode = transport_->sendRequest(
      "GET", nullptr, 0,
      [this, &responseBody, &responseTypeChecked, &incompatibleResponse, &responseTooLarge, &sawFirstNonWhitespace](
          const uint8_t* data, size_t len) {
        if (!responseTypeChecked) {
          responseTypeChecked = true;
          const std::string contentType = transport_->getResponseHeader("Content-Type");
          if (!contentType.empty() && !isJsonContentType(contentType)) {
            incompatibleResponse = true;
            return false;
          }
        }
        for (size_t i = 0; i < len && !sawFirstNonWhitespace; ++i) {
          if (std::isspace(static_cast<unsigned char>(data[i]))) continue;
          sawFirstNonWhitespace = true;
          if (data[i] == '<') {
            incompatibleResponse = true;
            return false;
          }
        }
        if (len > MAX_PROGRESS_RESPONSE_BODY - responseBody.size()) {
          responseTooLarge = true;
          return false;
        }
        responseBody.append(reinterpret_cast<const char*>(data), len);
        return true;
      },
      shouldAbort);
  lastHttpCode_ = httpCode;
  KOReaderSyncClient::lastHttpCode = httpCode;

  LOG_DBG("KOSync", "Get progress response: %d", httpCode);

  // Empty bodies never invoke onData, so consult an available Content-Type here as well. The
  // result is applied only to successful responses below; HTTP error categorization remains
  // authoritative for 401/404/5xx responses even if their body happens to be HTML.
  if (!responseTypeChecked) {
    const std::string contentType = transport_->getResponseHeader("Content-Type");
    incompatibleResponse = !contentType.empty() && !isJsonContentType(contentType);
  }

  Error result;
  if (httpCode <= 0) {
    result = NETWORK_ERROR;
  } else if (httpCode == 204) {
    // No Content: some servers (e.g. BookLore/grimmory) use this to mean "no progress recorded
    // for this document" instead of 404. Map it the same way -- and never hand responseBody (per
    // HTTP semantics there isn't one, but a misbehaving server could still send bytes) to the JSON
    // parser below, which would either fail confusingly or, worse, parse into real-looking data.
    result = NOT_FOUND;
  } else if (isHttpSuccess(httpCode)) {
    if (responseTooLarge) {
      LOG_ERR("KOSync", "Progress response exceeds %u-byte limit", (unsigned)MAX_PROGRESS_RESPONSE_BODY);
      result = JSON_ERROR;
    } else if (incompatibleResponse) {
      LOG_ERR("KOSync", "Progress response is not compatible JSON");
      result = JSON_ERROR;
    } else {
      JsonDocument doc;
      const DeserializationError error = deserializeJson(doc, responseBody.c_str());

      if (error || !doc.is<JsonObjectConst>()) {
        LOG_ERR("KOSync", "JSON parse failed: %s", error.c_str());
        result = JSON_ERROR;
      } else if (!doc["percentage"].is<float>() || !std::isfinite(doc["percentage"].as<float>()) ||
                 doc["percentage"].as<float>() < 0.0f || doc["percentage"].as<float>() > 1.0f ||
                 !doc["progress"].is<const char*>()) {
        LOG_ERR("KOSync", "Progress response missing/invalid position fields");
        result = JSON_ERROR;
      } else if (!doc["timestamp"].is<int64_t>() || doc["timestamp"].as<int64_t>() < 0 ||
                 !ProgressTimestamp::valid(static_cast<uint64_t>(doc["timestamp"].as<int64_t>()))) {
        LOG_ERR("KOSync", "Progress response missing/invalid Unix-seconds timestamp");
        result = JSON_ERROR;
      } else {
        outProgress.document = documentHash;
        outProgress.progress = doc["progress"].as<std::string>();
        outProgress.percentage = doc["percentage"].as<float>();
        outProgress.device = doc["device"].as<std::string>();
        outProgress.deviceId = doc["device_id"].as<std::string>();
        outProgress.timestamp = doc["timestamp"].as<int64_t>();

        outProgress.position.reset();
        if (KOREADER_STORE.usesCrossPointSyncServer()) {
          const JsonObjectConst pos = doc["position"].as<JsonObjectConst>();
          if (!pos.isNull()) {
            KOReaderRichPosition rich;
            rich.pctQ = pos["pctQ"].as<uint32_t>();
            rich.spineIndex = pos["spine"].as<uint16_t>();
            rich.pageNumber = pos["page"].as<uint16_t>();
            const uint16_t pages = pos["pages"].as<uint16_t>();
            rich.totalPages = pages > 0 ? pages : 1;
            const uint16_t para = pos["para"].as<uint16_t>();
            if (para > 0) rich.paragraphIndex = para;
            rich.xpath = pos["xpath"].as<const char*>() ? pos["xpath"].as<const char*>() : "";
            LOG_DBG("KOSync", "Got rich position: spine=%u page=%u/%u para=%u", rich.spineIndex, rich.pageNumber,
                    rich.totalPages, para);
            outProgress.position = std::move(rich);
          }
        }

        LOG_DBG("KOSync", "Got progress: %.2f%% at %s", outProgress.percentage * 100, outProgress.progress.c_str());
        result = OK;
      }
    }
  } else if (httpCode == 401) {
    result = AUTH_FAILED;
  } else if (httpCode == 404) {
    result = NOT_FOUND;
  } else {
    result = SERVER_ERROR;
  }

  logOutcome("GET /syncs/progress", httpCode, result, hashKind);
  return result;
}

KOReaderSyncClient::Error KOReaderSyncClient::Session::updateProgress(const KOReaderProgress& progress,
                                                                      const AbortCallback& shouldAbort,
                                                                      const char* hashKind) {
  lastHttpCode_ = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress";
  LOG_DBG("KOSync", "Updating progress: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

  // Build JSON body
  JsonDocument doc;
  doc["document"] = progress.document;
  if (progress.metadata.has_value()) {
    auto meta = doc["metadata"].to<JsonObject>();
    meta["filename"] = progress.metadata->filename;
    meta["title"] = progress.metadata->title;
    meta["authors"] = progress.metadata->authors;
  }
  doc["progress"] = progress.progress;
  doc["percentage"] = progress.percentage;
  // Caller (KOReaderSyncActivity) sets progress.device from CrossPointSettings' user-configurable
  // device name; DEVICE_NAME is only a fallback for the (currently theoretical) case of a caller
  // that doesn't set it, so a sync payload never goes out with a blank device field.
  doc["device"] = progress.device.empty() ? DEVICE_NAME : progress.device;
  doc["device_id"] = DEVICE_ID;
  if (progress.position.has_value() && KOREADER_STORE.usesCrossPointSyncServer()) {
    // CrossPoint-specific extension: do not send it to third-party KOSync servers.
    const auto& p = *progress.position;
    auto pos = doc["position"].to<JsonObject>();
    pos["pctQ"] = p.pctQ;
    pos["spine"] = p.spineIndex;
    pos["page"] = p.pageNumber;
    pos["pages"] = p.totalPages;
    if (p.paragraphIndex.has_value()) pos["para"] = *p.paragraphIndex;
    // Server rejects the whole position object if xpath exceeds 120 bytes.
    if (!p.xpath.empty() && p.xpath.size() <= 120) pos["xpath"] = p.xpath;
  }

  std::string body;
  serializeJson(doc, body);

  LOG_DBG("KOSync", "Request body: %s", body.c_str());

  if (!transport_->begin(url)) {
    LOG_ERR("KOSync", "Bad URL: %s", url.c_str());
    return BAD_REQUEST;
  }
  applyAuthHeaders(*transport_);
  transport_->addHeader("Content-Type", "application/json");
  // Response body is unused here, unlike getProgress() -- discard it, still threading the abort
  // callback through so a PUT can be cancelled mid-flight the same way a GET can.
  const int httpCode = transport_->sendRequest(
      "PUT", reinterpret_cast<const uint8_t*>(body.data()), body.size(), [](const uint8_t*, size_t) { return true; },
      shouldAbort);
  lastHttpCode_ = httpCode;
  KOReaderSyncClient::lastHttpCode = httpCode;

  LOG_DBG("KOSync", "Update progress response: %d", httpCode);

  Error result;
  if (httpCode <= 0)
    result = NETWORK_ERROR;
  else if (isHttpSuccess(httpCode))
    result = OK;
  else if (httpCode == 401)
    result = AUTH_FAILED;
  else
    result = SERVER_ERROR;

  logOutcome("PUT /syncs/progress", httpCode, result, hashKind);
  return result;
}

KOReaderSyncClient::Error KOReaderSyncClient::authenticate() {
  Session session;
  const Error result = session.authenticate();
  lastHttpCode = session.lastHttpCode();
  return result;
}

KOReaderSyncClient::Error KOReaderSyncClient::createUser() {
  Session session;
  const Error result = session.createUser();
  lastHttpCode = session.lastHttpCode();
  return result;
}

KOReaderSyncClient::Error KOReaderSyncClient::getProgress(const std::string& documentHash,
                                                          KOReaderProgress& outProgress,
                                                          const AbortCallback& shouldAbort) {
  Session session;
  const Error result = session.getProgress(documentHash, outProgress, shouldAbort, nullptr);
  lastHttpCode = session.lastHttpCode();
  return result;
}

KOReaderSyncClient::Error KOReaderSyncClient::updateProgress(const KOReaderProgress& progress,
                                                             const AbortCallback& shouldAbort) {
  Session session;
  const Error result = session.updateProgress(progress, shouldAbort, nullptr);
  lastHttpCode = session.lastHttpCode();
  return result;
}

const char* KOReaderSyncClient::errorString(Error error) {
  switch (error) {
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
    case NOT_FOUND:
      return "No progress found";
    case LOW_MEMORY:
      return "Not enough memory for sync — please retry";
    case UNSUPPORTED:
      return "Not supported by this server";
    case BAD_REQUEST:
      return "Server rejected the request payload";
    default:
      return "Unknown error";
  }
}
