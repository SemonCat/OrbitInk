#pragma once

#include <cstddef>
#include <cstdint>

// Bounded HTTP/1.1 response-head parsing and URL handling for the custom OTA fetch
// (src/network/OtaFetch.cpp). Pure and host-tested (test/ota_manifest).
//
// Why not freeink::SecureHttpClient: it stores every response header as a std::string pair
// (SecureHttpClient.h, _responseHeaders) and each line up to 4 KB. GitHub's release redirect hop
// sends a ~4 KB Content-Security-Policy line plus a ~0.9 KB signed Location on every hop, and the
// X3 zh-TW crash that disabled online updates decoded to exactly that redirect/header string
// allocation. This parser keeps a fixed-size line buffer and only four facts (status, Content-Length,
// Transfer-Encoding presence, Location); every other header is discarded byte by byte, so the
// memory used is constant regardless of what the server sends.
namespace ota_http {

constexpr size_t LOCATION_MAX = 1536;  // observed GitHub signed asset URL: ~930 chars
constexpr size_t LINE_KEEP = LOCATION_MAX + 16;
constexpr size_t MAX_HEAD_BYTES = 32 * 1024;
constexpr size_t MAX_HEADER_LINES = 128;
constexpr size_t HOST_MAX = 64;
constexpr size_t PATH_MAX_LEN = LOCATION_MAX;

class ResponseHeadParser {
 public:
  enum class Status : uint8_t { NEED_MORE, DONE, ERROR };

  // Consume bytes of the response; stops right after the blank line ending the head and returns
  // how many bytes of `data` it used (the rest is body).
  size_t feed(const uint8_t* data, size_t len);

  Status status() const { return st; }
  int statusCode() const { return code; }
  bool hasContentLength() const { return haveLength; }
  uint64_t contentLength() const { return length; }
  bool hasTransferEncoding() const { return transferEncoding; }
  const char* location() const { return haveLocation ? loc : nullptr; }

 private:
  void finishLine();
  void handleHeader(const char* name, size_t nameLen, const char* value, size_t valueLen);

  Status st = Status::NEED_MORE;
  char line[LINE_KEEP + 1] = {};
  size_t lineLen = 0;
  bool lineOverflow = false;
  bool sawStatusLine = false;
  size_t headBytes = 0;
  size_t lines = 0;
  int code = 0;
  bool haveLength = false;
  uint64_t length = 0;
  bool transferEncoding = false;
  bool haveLocation = false;
  char loc[LOCATION_MAX + 1] = {};
};

struct Url {
  char host[HOST_MAX + 1] = {};
  uint16_t port = 443;
  char path[PATH_MAX_LEN + 1] = {};
};

// Parses "https://host[:port]/path". Rejects other schemes, userinfo, empty host, and anything
// that does not fit the fixed buffers.
bool parseHttpsUrl(const char* url, Url& out);

// Resolves a redirect Location against the current URL: absolute https URLs, or absolute paths
// ("/...") on the same host. Protocol-relative, relative-path and non-https forms are refused.
bool resolveRedirect(const Url& current, const char* location, Url& out);

// Only these hosts may be contacted by the OTA fetch (initial URL and every redirect hop).
bool isAllowedHost(const char* host);

}  // namespace ota_http
