#pragma once

#include <OtaDiagnostics.h>

#include <cstddef>
#include <cstdint>

// Bounded HTTPS GET for the custom OTA channel only (manifest + firmware), device build only.
//
// Uses freeink::SecureClient (wolfSSL TLS) directly with lib/FirmwareUpdate's fixed-size
// ResponseHeadParser instead of freeink::SecureHttpClient, whose per-hop std::string storage of
// every response header is what the X3 heap-abort decoded to (see OtaHttp.h). Memory use per hop
// is constant: one SecureClient, one parser (~3 KB), one 4 KB read buffer.
//
// Every hop must be https to an allowlisted GitHub host (ota_http::isAllowedHost), at most
// MAX_REDIRECTS redirects, and the final 200 must carry a Content-Length within `maxBytes`
// (and equal to `expectedLength` when non-zero). Chunked bodies are refused.
//
// TLS peer verification is OFF, as for every other request this firmware makes over wolfSSL
// (no CA bundle is wired up; see SecureHttpClient.h). Authenticity therefore never comes from the
// transport: the manifest carries an ECDSA P-256 signature checked against the key compiled into
// the firmware, and the firmware bytes must match the signed SHA256 and size before anything is
// flashed. A network attacker can block or stall an update, not substitute one.
namespace ota_fetch {

enum class Result : uint8_t {
  OK,
  BAD_URL,
  HOST_NOT_ALLOWED,
  OOM,
  CONNECT_FAILED,
  WRITE_FAILED,
  TIMEOUT,
  BAD_RESPONSE,  // unparsable / oversized head
  HTTP_STATUS,   // non-200 final status (see lastStatus)
  TOO_MANY_REDIRECTS,
  NO_LENGTH,     // missing Content-Length or chunked encoding
  TOO_LARGE,     // Content-Length over maxBytes / != expectedLength
  TRUNCATED,     // connection closed before Content-Length bytes
  SINK_ABORTED,  // onData returned false
};

const char* resultName(Result r);

constexpr int MAX_REDIRECTS = 4;

// Called with each body chunk; return false to abort.
using DataFn = bool (*)(const uint8_t* data, size_t len, void* ctx);

// `userAgent` must be a stable literal (ota_channel::IDENTITY). Returns the final HTTP status in
// `lastStatus` when the request reached a server.
// On failure, `diag` (when given) receives the failing hop, its host and the heap state; the
// caller sets phase/error.
Result get(const char* url, const char* userAgent, size_t maxBytes, size_t expectedLength, DataFn onData, void* ctx,
           int* lastStatus = nullptr, ota_diagnostics::Record* diag = nullptr);

// Firmware download of exactly `expectedLength` bytes (the signed size). When the body stops part-way
// (TRUNCATED or TIMEOUT) after delivering new bytes, the request is re-issued from `url` -- fresh
// redirect walk, since signed asset Locations expire -- with "Range: bytes=<delivered>-", and only a
// 206 continuing at exactly that offset of the same object is accepted (ota_http::checkContinuation).
// At most ota_http::MAX_RESUMES resumes. `onData` therefore sees one contiguous byte stream, and the
// caller's own size/SHA-256/image validation covers every byte; nothing partial is ever accepted.
// `diag->detail` names why the last attempt's body stopped (PEER_CLOSED, TLS_READ_ERROR, BODY_IDLE,
// RESUME_BAD_*), `diag->resumes` how many resumes ran.
Result getResumable(const char* url, const char* userAgent, size_t expectedLength, DataFn onData, void* ctx,
                    int* lastStatus = nullptr, ota_diagnostics::Record* diag = nullptr);

}  // namespace ota_fetch
