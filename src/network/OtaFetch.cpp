#include "OtaFetch.h"

// clang-format off
// SecureClient.h pulls Arduino/WiFi, whose macros must be seen before lwip (same pin as OtaUpdater.cpp).
#include <SecureClient.h>
#include <Logging.h>
#include <Memory.h>
#include <OtaHttp.h>
// clang-format on

#include <cstdio>
#include <cstring>

#include "util/TaskWatchdog.h"

namespace ota_fetch {

namespace {
constexpr size_t CHUNK = 4096;
constexpr unsigned long IDLE_TIMEOUT_MS = 20000;  // no bytes for this long -> give up

bool writeAll(freeink::SecureClient& c, const char* s) {
  const size_t n = strlen(s);
  return c.write(reinterpret_cast<const uint8_t*>(s), n) == n;
}

bool sendRequest(freeink::SecureClient& c, const ota_http::Url& url, const char* userAgent, const uint64_t offset) {
  char port[16] = "";
  if (url.port != 443) snprintf(port, sizeof(port), ":%u", static_cast<unsigned>(url.port));
  char range[48] = "";
  if (offset > 0) snprintf(range, sizeof(range), "\r\nRange: bytes=%llu-", static_cast<unsigned long long>(offset));
  // Written piecewise: the path can be ~1.5 KB, so no request string is assembled on the heap.
  return writeAll(c, "GET ") && writeAll(c, url.path) && writeAll(c, " HTTP/1.1\r\nHost: ") && writeAll(c, url.host) &&
         writeAll(c, port) && writeAll(c, "\r\nUser-Agent: ") && writeAll(c, userAgent) && writeAll(c, range) &&
         writeAll(c, "\r\nAccept: */*\r\nConnection: close\r\n\r\n");
}

// Reads whatever is available into buf. Returns >0 bytes, 0 for "nothing yet", -1 for closed.
int readSome(freeink::SecureClient& c, uint8_t* buf, const size_t cap) {
  const int n = c.read(buf, cap);
  if (n > 0) return n;
  if (n < 0 || (!c.connected() && c.available() == 0)) return -1;
  return 0;
}
}  // namespace

const char* resultName(const Result r) {
  switch (r) {
    case Result::OK:
      return "OK";
    case Result::BAD_URL:
      return "BAD_URL";
    case Result::HOST_NOT_ALLOWED:
      return "HOST_NOT_ALLOWED";
    case Result::OOM:
      return "OOM";
    case Result::CONNECT_FAILED:
      return "CONNECT_FAILED";
    case Result::WRITE_FAILED:
      return "WRITE_FAILED";
    case Result::TIMEOUT:
      return "TIMEOUT";
    case Result::BAD_RESPONSE:
      return "BAD_RESPONSE";
    case Result::HTTP_STATUS:
      return "HTTP_STATUS";
    case Result::TOO_MANY_REDIRECTS:
      return "TOO_MANY_REDIRECTS";
    case Result::NO_LENGTH:
      return "NO_LENGTH";
    case Result::TOO_LARGE:
      return "TOO_LARGE";
    case Result::TRUNCATED:
      return "TRUNCATED";
    case Result::SINK_ABORTED:
      return "SINK_ABORTED";
  }
  return "?";
}

namespace {
struct HopState {
  int hop = -1;
  char host[sizeof(ota_diagnostics::Record::host)] = {};  // copied: the Url is freed on return
  uint64_t delivered = 0;  // body bytes handed to onData by this attempt
  const char* stop = "";   // why a body ended early: the cases readSome() folds into "closed"
};

// `offset` > 0 resumes a download: the request carries "Range: bytes=offset-" and the final reply must
// continue the same `expectedLength`-byte object exactly there (ota_http::checkContinuation).
Result getImpl(const char* urlText, const char* userAgent, const size_t maxBytes, const size_t expectedLength,
               const uint64_t offset, const DataFn onData, void* ctx, int* lastStatus, HopState& at) {
  if (lastStatus) *lastStatus = 0;
  auto url = makeUniqueNoThrow<ota_http::Url>();
  auto next = makeUniqueNoThrow<ota_http::Url>();
  auto buf = makeUniqueNoThrow<uint8_t[]>(CHUNK);
  if (!url || !next || !buf) {
    LOG_ERR("OTAF", "OOM before fetch");
    return Result::OOM;
  }
  if (!ota_http::parseHttpsUrl(urlText, *url)) return Result::BAD_URL;

  for (int hop = 0; hop <= MAX_REDIRECTS; ++hop) {
    at.hop = hop;
    if (lastStatus) *lastStatus = 0;  // a later hop's connect failure must not report an earlier 302
    snprintf(at.host, sizeof(at.host), "%s", url->host);
    if (!ota_http::isAllowedHost(url->host)) {
      LOG_ERR("OTAF", "host not allowed: %s", url->host);
      return Result::HOST_NOT_ALLOWED;
    }
    LOG_INF("OTAF", "GET https://%s (hop %d, free heap %u, largest block %u)", url->host, hop,
            static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));

    auto client = makeUniqueNoThrow<freeink::SecureClient>();
    auto head = makeUniqueNoThrow<ota_http::ResponseHeadParser>();
    if (!client || !head) {
      LOG_ERR("OTAF", "OOM for hop %d", hop);
      return Result::OOM;
    }
    client->setInsecure();  // authenticity is the signed manifest, see OtaFetch.h
    if (!client->connect(url->host, url->port)) {
      LOG_ERR("OTAF", "connect/handshake failed: %s", url->host);
      return Result::CONNECT_FAILED;
    }
    if (!sendRequest(*client, *url, userAgent, offset)) {
      client->stop();
      return Result::WRITE_FAILED;
    }

    // Response head, read in small slices so leftover body bytes are few and handled below.
    size_t bodyStart = 0;
    size_t bodyLen = 0;
    unsigned long idleSince = millis();
    while (head->status() == ota_http::ResponseHeadParser::Status::NEED_MORE) {
      resetTaskWatchdogIfSubscribed();
      const int n = readSome(*client, buf.get(), 512);
      if (n < 0) {
        client->stop();
        return Result::BAD_RESPONSE;
      }
      if (n == 0) {
        if (millis() - idleSince > IDLE_TIMEOUT_MS) {
          client->stop();
          return Result::TIMEOUT;
        }
        delay(2);
        continue;
      }
      idleSince = millis();
      const size_t used = head->feed(buf.get(), static_cast<size_t>(n));
      if (head->status() == ota_http::ResponseHeadParser::Status::DONE) {
        bodyStart = used;
        bodyLen = static_cast<size_t>(n) - used;
      }
    }
    if (head->status() != ota_http::ResponseHeadParser::Status::DONE) {
      client->stop();
      return Result::BAD_RESPONSE;
    }
    const int code = head->statusCode();
    if (lastStatus) *lastStatus = code;

    if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
      const bool ok = ota_http::resolveRedirect(*url, head->location(), *next);
      client->stop();
      if (!ok) {
        LOG_ERR("OTAF", "unusable redirect (%d)", code);
        return Result::BAD_RESPONSE;
      }
      *url = *next;
      continue;
    }
    if (code != 200 && !(offset > 0 && code == 206)) {
      client->stop();
      LOG_ERR("OTAF", "HTTP %d", code);
      return Result::HTTP_STATUS;
    }
    if (head->hasTransferEncoding() || !head->hasContentLength()) {
      client->stop();
      return Result::NO_LENGTH;
    }
    if (offset > 0) {
      const auto continuation = ota_http::checkContinuation(*head, offset, expectedLength);
      if (continuation != ota_http::Continuation::OK) {
        client->stop();
        at.stop = ota_http::continuationName(continuation);
        LOG_ERR("OTAF", "resume at %llu rejected: %s (HTTP %d)", static_cast<unsigned long long>(offset), at.stop, code);
        return Result::BAD_RESPONSE;
      }
    }
    const uint64_t length = head->contentLength();
    const uint64_t expectedBody = expectedLength > 0 ? expectedLength - offset : 0;
    if (length > maxBytes || (expectedLength > 0 && length != expectedBody)) {
      client->stop();
      LOG_ERR("OTAF", "body length %llu outside limit %u / expected %u", static_cast<unsigned long long>(length),
              static_cast<unsigned>(maxBytes), static_cast<unsigned>(expectedLength));
      return Result::TOO_LARGE;
    }
    head.reset();  // free the parser before the long body transfer

    size_t remaining = static_cast<size_t>(length);
    if (bodyLen > remaining) bodyLen = remaining;  // server sent more than it declared: ignore
    if (bodyLen > 0) {
      if (!onData(buf.get() + bodyStart, bodyLen, ctx)) {
        client->stop();
        return Result::SINK_ABORTED;
      }
      remaining -= bodyLen;
      at.delivered += bodyLen;
    }
    idleSince = millis();
    while (remaining > 0) {
      resetTaskWatchdogIfSubscribed();
      const int raw = client->read(buf.get(), remaining < CHUNK ? remaining : CHUNK);
      const int n = raw > 0 ? raw : (raw < 0 || (!client->connected() && client->available() == 0)) ? -1 : 0;
      if (n < 0) {
        // raw < 0: wolfSSL read failed (fatal TLS/transport error). raw == 0 with no connection: the
        // peer ended TLS cleanly (close_notify) or the TCP socket dropped without one.
        at.stop = raw < 0 ? "TLS_READ_ERROR" : "PEER_CLOSED";
        client->stop();
        LOG_ERR("OTAF", "%s with %u bytes outstanding", at.stop, static_cast<unsigned>(remaining));
        return Result::TRUNCATED;
      }
      if (n == 0) {
        if (millis() - idleSince > IDLE_TIMEOUT_MS) {
          at.stop = "BODY_IDLE";
          client->stop();
          return Result::TIMEOUT;
        }
        delay(2);
        continue;
      }
      idleSince = millis();
      if (!onData(buf.get(), static_cast<size_t>(n), ctx)) {
        client->stop();
        return Result::SINK_ABORTED;
      }
      remaining -= static_cast<size_t>(n);
      at.delivered += static_cast<uint64_t>(n);
    }
    client->stop();
    return Result::OK;
  }
  return Result::TOO_MANY_REDIRECTS;
}

void fillFailure(ota_diagnostics::Record* diag, const HopState& at, const int* lastStatus) {
  if (!diag) return;
  // Sampled after the failing hop's client and parser are gone: what the next attempt would see.
  diag->hop = at.hop;
  diag->setHost(at.host);
  diag->httpStatus = lastStatus ? *lastStatus : 0;
  diag->freeHeap = ESP.getFreeHeap();
  diag->maxAlloc = ESP.getMaxAllocHeap();
  if (at.stop[0] != '\0') diag->detail = at.stop;
}
}  // namespace

Result get(const char* urlText, const char* userAgent, const size_t maxBytes, const size_t expectedLength,
           const DataFn onData, void* ctx, int* lastStatus, ota_diagnostics::Record* diag) {
  HopState at;
  const Result result = getImpl(urlText, userAgent, maxBytes, expectedLength, 0, onData, ctx, lastStatus, at);
  if (result != Result::OK) fillFailure(diag, at, lastStatus);
  return result;
}

Result getResumable(const char* urlText, const char* userAgent, const size_t expectedLength, const DataFn onData,
                    void* ctx, int* lastStatus, ota_diagnostics::Record* diag) {
  if (expectedLength == 0) return Result::BAD_URL;  // resume needs the signed total to validate against
  uint64_t delivered = 0;
  int resumes = 0;
  while (true) {
    HopState at;
    const uint64_t before = delivered;
    const Result result =
        getImpl(urlText, userAgent, expectedLength, expectedLength, delivered, onData, ctx, lastStatus, at);
    delivered += at.delivered;
    if (diag) diag->resumes = static_cast<uint8_t>(resumes);
    if (result == Result::OK) return result;
    // Only a body that stopped part-way is resumed, and only while each attempt still makes progress.
    const bool interrupted = result == Result::TRUNCATED || result == Result::TIMEOUT;
    if (!interrupted || !ota_http::shouldResume(resumes, before, delivered, expectedLength)) {
      fillFailure(diag, at, lastStatus);
      return result;
    }
    resumes++;
    LOG_INF("OTAF", "resuming at %llu/%u after %s/%s (resume %d/%d)", static_cast<unsigned long long>(delivered),
            static_cast<unsigned>(expectedLength), resultName(result), at.stop, resumes, ota_http::MAX_RESUMES);
    for (int i = 0; i < 10; ++i) {  // 1 s for the link to settle, watchdog fed
      resetTaskWatchdogIfSubscribed();
      delay(100);
    }
  }
}

}  // namespace ota_fetch
