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
  // "Content-Range: bytes first-last/total", strictly parsed (no "*", no unknown total).
  bool hasContentRange() const { return haveRange; }
  uint64_t rangeFirst() const { return rangeStart; }
  uint64_t rangeLast() const { return rangeEnd; }
  uint64_t rangeTotal() const { return rangeComplete; }

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
  bool haveRange = false;
  uint64_t rangeStart = 0;
  uint64_t rangeEnd = 0;
  uint64_t rangeComplete = 0;
};

// Firmware download resume (OtaFetch). After the body stopped early, the fetch is re-issued from the
// signed URL with "Range: bytes=<offset>-"; only a reply that continues EXACTLY at `offset` of the
// same `total`-byte object may feed the stream. The caller's size/SHA-256/image checks still run over
// every byte, so a wrong continuation can only fail the update, never install it.
enum class Continuation : uint8_t {
  OK,             // 200 for a fresh request, or a 206 continuing at offset of the same object
  BAD_STATUS,     // anything else (incl. 200 to a resume: the server ignored Range)
  BAD_RANGE,      // 206 without, or with a different, Content-Range
  BAD_LENGTH,     // Content-Length missing or not the remaining span
};
Continuation checkContinuation(const ResponseHeadParser& head, uint64_t offset, uint64_t total);
const char* continuationName(Continuation c);

// At most three extra requests, including retries before any body bytes arrive.
// One no-progress failure may recover; a second consecutive one stops early.
constexpr int MAX_RESUMES = 3;
constexpr int MAX_NO_PROGRESS_ATTEMPTS = 2;
inline bool shouldResume(const int resumesSoFar, const int noProgressAttempts, const uint64_t deliveredNow,
                         const uint64_t total) {
  return resumesSoFar < MAX_RESUMES && noProgressAttempts < MAX_NO_PROGRESS_ATTEMPTS && deliveredNow < total;
}

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
