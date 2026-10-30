#pragma once
#include <Logging.h>
#include <base64.h>

#include <cstdint>
#include <string>

#include "KOReaderSyncClient.h"
#include "KOSyncTransport.h"

// Request-shaping helpers shared by every KOSync-dialect request: core progress sync
// (KOReaderSyncClient.cpp) and the standalone BookOrbit client (BookOrbitSyncClient.cpp), which
// speaks the same wire dialect over its own credentials (see applyAuthHeaders' explicit-credential
// overload below). Header-inline rather than duplicated per translation unit so both provably
// send the same auth-header shape, apply the same TLS heap gate, and categorise HTTP status
// identically -- the ee4490f hardening (any 2xx is success; a bounded heap gate before every
// request) must not fork between them.
//
// Like KOReaderSyncClient.cpp itself, this file pulls in no Arduino/ESP-IDF/wolfSSL header
// directly, so both callers stay host-testable against a fake KOSyncTransport.

namespace KOSyncRequest {

// KOSync's TLS-1.3 servers run over wolfSSL via SecureHttpClient; the handshake still needs
// working heap. Both figures, and the reasoning behind them, are unchanged from ee4490f --
// see the long calibration note that accompanied them in KOReaderSyncClient.cpp.
constexpr uint32_t MIN_FREE_FOR_TLS = 35000;
constexpr uint32_t MIN_BLOCK_FOR_TLS = 20000;

// Apply the shared KOSync-dialect auth headers after begin(), for an explicit credential triple.
// x-auth-* is the native KOSync scheme; Basic auth is added for Calibre-Web-Automated
// compatibility. BookOrbit's own plugin guard (koreader-auth.guard.ts) accepts the same headers
// over its own credentials, which is what lets BookOrbitSyncClient share this helper without
// depending on KOReaderCredentialStore -- see BookOrbitSyncClient.cpp's call site.
//
// Deliberately the ONLY overload here: a KOReaderCredentialStore-reading convenience wrapper used
// to live here too, but this header is `inline`-parsed (and therefore type-checked) by every TU
// that includes it, including BookOrbitSyncClient.cpp, which never includes
// KOReaderCredentialStore.h -- exactly the coupling this header exists to avoid. Each caller
// instead keeps its own trivial credential-store-reading wrapper local to its own .cpp (see
// KOReaderSyncClient.cpp's applyAuthHeaders() and BookOrbitSyncClient.cpp's
// applyBookOrbitAuthHeaders()).
inline void applyAuthHeaders(KOSyncTransport& http, const std::string& username, const std::string& md5Password,
                             const std::string& password) {
  http.addHeader("Accept", "application/vnd.koreader.v1+json");
  http.addHeader("x-auth-user", username);
  http.addHeader("x-auth-key", md5Password);
  const std::string credentials = username + ":" + password;
  const String encoded = base64::encode(credentials.c_str());
  http.addHeader("Authorization", std::string("Basic ") + encoded.c_str());
}

// True when free heap is too low to risk a TLS handshake.
inline bool insufficientHeap() {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
  if (freeHeap < MIN_FREE_FOR_TLS || maxAllocHeap < MIN_BLOCK_FOR_TLS) {
    LOG_ERR("KOSync", "Insufficient heap for TLS handshake: %u bytes free (need %u), %u max alloc (need %u)", freeHeap,
            MIN_FREE_FOR_TLS, maxAllocHeap, MIN_BLOCK_FOR_TLS);
    return true;
  }
  return false;
}

// Release-visible (INFO) summary of one request's outcome, for a device serial capture to
// confirm exactly what happened without needing DEBUG-level logs enabled. Deliberately
// excludes the URL, document hash, and all auth material.
inline void logOutcome(const char* method, int httpCode, KOReaderSyncClient::Error result, const char* hashKind) {
  LOG_INF("KOSync", "%s %s: http=%d (%s)", method, hashKind ? hashKind : "-", httpCode,
          KOReaderSyncClient::errorString(result));
}

// Every 2xx status is a success, matching the semantics of the underlying HTTP protocol rather
// than special-casing individual codes different servers happen to use (e.g. 201/202/226).
inline bool isHttpSuccess(int httpCode) { return httpCode >= 200 && httpCode < 300; }

}  // namespace KOSyncRequest
