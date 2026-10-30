#pragma once

// Owned simulator adapter: offline stand-in for freeink::SecureHttpClient.
//
// WHY THIS FILE EXISTS
// The device build gets freeink::SecureHttpClient from the FreeInk SDK (see
// freeink-sdk/libs/network/SecureNet/include/SecureHttpClient.h): a self-contained HTTP/1.1 client
// over wolfSSL's SecureClient. That cannot build natively, and the pinned simulator's own
// SecureHttpClient.h is a thin Arduino-HTTPClient wrapper with an incompatible, String-based
// shape -- it has no getHeader(), a non-const getString() returning String, and no
// payload+callback sendRequest() overload, all of which lib/KOReaderSync/SecureHttpClientTransport.h
// and src/network/HttpDownloader.cpp require. Since this adapter library is searched ahead of the
// simulator's src/, this file replaces that header outright for the simulator build. No simulator
// translation unit includes SecureHttpClient.h, so there is exactly one definition in the program
// and nothing to keep in sync with the dependency.
//
// Offline by default. An explicit CROSSFADE_SIM_HTTP_BRIDGE absolute executable path
// enables the owned subprocess transport in SecureHttpClient.cpp. Requests retain the
// production caller's URL, method and body. The supplied BookOrbit helper restricts the
// origin, document and device, sources auth from a supported plugin provision package,
// and requires a matching pre-write progress snapshot. No dependency source is patched.
// Host transport/clock recovery is not a test of device TLS, NTP reachability or memory.
//
// See sim/README.md.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace freeink {

class SecureHttpClient {
 public:
  // Signatures mirror the SDK header so call sites compile unchanged.
  using DataCallback = std::function<bool(const uint8_t* data, size_t len)>;
  using AbortCallback = std::function<bool()>;
  using ProgressCallback = std::function<bool(size_t downloaded, size_t total)>;

  SecureHttpClient() = default;
  ~SecureHttpClient() = default;
  SecureHttpClient(const SecureHttpClient&) = delete;
  SecureHttpClient& operator=(const SecureHttpClient&) = delete;

  // Configuration is accepted and discarded: with no transport there is nothing to configure.
  void setInsecure() {}
  void setCACert(const char*) {}
  void setTimeout(uint32_t) {}
  void setProgressCallback(const ProgressCallback&) {}
  void setFollowRedirects(int) {}
  void setAllowRedirectDowngrade(bool) {}
  void setBasicAuth(const std::string&, const std::string&) {}
  void clearBasicAuth() {}
  void setUserAgent(const std::string&) {}
  void setReuse(bool) {}

  // Matches the SDK: false only for a URL this client could never dial. Accepting a well-formed
  // URL and then failing at request time reproduces the device's offline shape (the firmware sees
  // a connect failure, not a bad-URL rejection) and so exercises the same error handling.
  bool begin(const std::string& url) {
    _body.clear();
    _headers.clear();
    _responseHeaders.clear();
    _url = url;
    _complete = false;
    _aborted = false;
    _status = 0;
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
  }
  void end() {}

  void addHeader(const std::string& name, const std::string& value) { _headers.emplace_back(name, value); }

  int GET() { return sendRequest("GET", nullptr, 0); }
  int GET(const DataCallback& callback, const AbortCallback& abort = nullptr) {
    return sendRequest("GET", nullptr, 0, callback, abort);
  }
  int POST(const std::string& body) { return sendRequest("POST", body); }
  int sendRequest(const char* method, const std::string& body) {
    return sendRequest(method, reinterpret_cast<const uint8_t*>(body.data()), body.size());
  }
  int sendRequest(const char* method, const uint8_t* payload, size_t size) {
    return sendRequest(method, payload, size, nullptr, nullptr);
  }
  int sendRequest(const char*, const uint8_t*, size_t, const DataCallback&, const AbortCallback& = nullptr);

  const std::string& getString() const { return _body; }
  std::string getHeader(const std::string&) const;
  int getStatus() const { return _status; }
  int getSize() const { return static_cast<int>(_body.size()); }
  // The opt-in bridge reports completion and callback cancellation separately.
  bool responseComplete() const { return _complete; }
  bool callbackAborted() const { return _aborted; }
  bool aborted() const { return _aborted; }
  bool hasContentLength() const { return false; }
  size_t getContentLength() const { return 0; }

  // Only ever called to follow a redirect (src/network/HttpDownloader.cpp:115), which is
  // unreachable when no request can return 3xx. Reporting "cannot resolve" keeps that path
  // failing closed rather than inventing a URL the offline client could not fetch anyway.
  static bool resolveUrl(const std::string&, const std::string&, std::string&) { return false; }

  // wolfSSL is not linked into the host build; nothing here negotiates TLS at all.
  static bool tls13Available() { return false; }

 private:
  // The SDK returns -1 for "connection or status line could not be read".
  static constexpr int kTransportFailure = -1;

  std::string _body;
  std::string _url;
  std::vector<std::pair<std::string, std::string>> _headers;
  std::vector<std::pair<std::string, std::string>> _responseHeaders;
  bool _complete = false;
  bool _aborted = false;
  int _status = 0;
};

}  // namespace freeink
