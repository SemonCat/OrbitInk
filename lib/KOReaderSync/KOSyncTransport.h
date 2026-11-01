#pragma once

#include <cstdint>
#include <functional>
#include <string>

// Minimal HTTP transport abstraction covering exactly what KOReaderSyncClient needs from
// freeink::SecureHttpClient (begin/addHeader/sendRequest/end). Exists so the KOReader sync
// PROTOCOL logic (JSON building/parsing, HTTP-status-to-Error categorization, primary/alternate
// smart-sync selection) can be tested with a fake, in-memory transport -- no real network, WiFi,
// or TLS/wolfSSL stack -- while production code runs over the real freeink-sdk SecureHttpClient
// (see SecureHttpClientTransport.h) unmodified. freeink-sdk itself is never patched; this
// interface is purely first-party, sitting one layer above it.
class KOSyncTransport {
 public:
  using DataCallback = std::function<bool(const uint8_t* data, size_t len)>;
  using AbortCallback = std::function<bool()>;

  virtual ~KOSyncTransport() = default;

  // Parse the URL and reset per-request state. Returns false on a malformed URL. Matches
  // freeink::SecureHttpClient::begin()'s contract exactly.
  virtual bool begin(const std::string& url) = 0;

  virtual void addHeader(const std::string& name, const std::string& value) = 0;

  // Performs one HTTP request. payload/payloadLen may be null/0 for a body-less request (GET).
  // onData, if set, receives response body chunks as they arrive (return false to abort the
  // transfer); if unset, the implementation may buffer the whole body internally instead
  // (matching SecureHttpClient's plain sendRequest() convenience overload). Returns the HTTP
  // status code only for a complete response, or a negative value on transport failure,
  // including a truncated/cancelled response after its status line was received.
  virtual int sendRequest(const char* method, const uint8_t* payload, size_t payloadLen,
                          const DataCallback& onData = nullptr, const AbortCallback& shouldAbort = nullptr) = 0;

  // The full response body accumulated by the last sendRequest() call for which onData was NOT
  // provided (empty otherwise, since the caller consumed the body itself via the callback).
  virtual const std::string& getString() const = 0;

  // Response header from the most recent request, case-insensitive in the production adapter.
  // Empty means absent or unavailable. Exposing this narrow read-only seam lets protocol clients
  // reject a web frontend's HTML before attempting to parse it as KOSync JSON.
  virtual std::string getResponseHeader(const std::string& /*name*/) const { return ""; }

  // Closes the connection (if any) held open for reuse. Safe to call even if nothing is open.
  virtual void end() = 0;
  virtual void setTimeout(unsigned long /*milliseconds*/) {}

  // True if this transport keeps the underlying connection open across begin()/sendRequest()
  // pairs for identical scheme://host:port, reusing the TLS session instead of paying a fresh
  // handshake each time. Purely informational (e.g. for logging); callers don't need to branch
  // on it -- both real and fake transports behave correctly regardless of the answer.
  virtual bool reuses() const { return true; }
};
