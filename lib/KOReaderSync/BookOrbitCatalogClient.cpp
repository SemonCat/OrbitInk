#include "BookOrbitCatalogClient.h"

#include <ArduinoJson.h>
#include <BookOrbitCredentialStore.h>
#include <BookOrbitStats.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "BookOrbitJsonKeyDropper.h"
#include "DefaultKOSyncTransport.h"
#include "KOSyncRequestHelpers.h"

// BookOrbitCredentialStore.h above is included via angle brackets so a host test's stub store
// wins the -I search order ahead of the real header, exactly like BookOrbitSyncClient.cpp.
//
// JSON parsing here follows BookOrbitSyncClient.cpp's already-reviewed pattern: bound the raw
// response body to a fixed cap inside the transport's streaming onData callback (never buffer an
// unbounded body), then run one plain deserializeJson() over that bounded string and read out only
// the known fields. CrossInk-BookOrbit's own client instead streams to a temp file and applies
// ArduinoJson's DeserializationOption::Filter; that shape doesn't carry over directly because
// KOSyncTransport's push-style onData callback (this codebase's host-testable transport
// abstraction, shared with BookOrbitSyncClient) has no stream::Reader to hand Filter, and adding a
// second, file-backed parsing path solely for catalog responses would duplicate an already-bounded
// mechanism rather than reuse it. The raw-byte cap is the primary bound in both designs; reading
// out only named fields from the parsed DOM (never iterating "every key present") is this client's
// equivalent of Filter's field-pruning intent.

namespace {

using Error = BookOrbitCatalogClient::Error;

Error categorizeStatus(const int httpCode) {
  if (httpCode <= 0) return Error::NETWORK_ERROR;
  if (KOSyncRequest::isHttpSuccess(httpCode)) return Error::OK;
  if (httpCode == 404 || httpCode == 405 || httpCode == 501) return Error::UNSUPPORTED;
  if (httpCode == 400 || httpCode == 413 || httpCode == 422) return Error::BAD_REQUEST;
  if (httpCode == 401) return Error::AUTH_FAILED;
  return Error::SERVER_ERROR;
}

void applyBookOrbitAuthHeaders(KOSyncTransport& http) {
  KOSyncRequest::applyAuthHeaders(http, BOOKORBIT_STORE.getUsername(), BOOKORBIT_STORE.getMd5Password(),
                                  BOOKORBIT_STORE.getPassword());
}

void logOutcome(const char* method, int httpCode, Error result) {
  LOG_INF("BOCatalog", "%s: http=%d (%s)", method, httpCode, BookOrbitCatalogClient::errorString(result));
}

constexpr char DOWNLOAD_TRANSACTION_MAGIC[] = "CrossFade BookOrbit catalog download transaction v1\n";

std::string downloadTransactionContents(const std::string& destPath) {
  return std::string(DOWNLOAD_TRANSACTION_MAGIC) + destPath;
}

bool writeDownloadTransaction(const std::string& markerPath, const std::string& destPath) {
  HalFile marker;
  if (!Storage.openFileForWrite("BOCatalog", markerPath, marker)) return false;
  const std::string contents = downloadTransactionContents(destPath);
  const size_t written = marker.write(reinterpret_cast<const uint8_t*>(contents.data()), contents.size());
  const bool closed = marker.close();
  return written == contents.size() && closed;
}

bool matchesDownloadTransaction(const std::string& markerPath, const std::string& destPath) {
  HalFile marker;
  if (!Storage.openFileForRead("BOCatalog", markerPath, marker)) return false;
  const std::string expected = downloadTransactionContents(destPath);
  bool matches = marker.fileSize() == expected.size();
  size_t offset = 0;
  uint8_t buffer[64];
  while (matches && offset < expected.size()) {
    const size_t want = std::min(sizeof(buffer), expected.size() - offset);
    const int got = marker.read(buffer, want);
    if (got != static_cast<int>(want)) {
      matches = false;
      break;
    }
    for (size_t i = 0; i < want; i++) {
      if (buffer[i] != static_cast<uint8_t>(expected[offset + i])) {
        matches = false;
        break;
      }
    }
    offset += want;
  }
  return marker.close() && matches;
}

// Delete a temp file only while the transaction marker proves it is ours. The marker is removed
// last; if either probe/removal fails, retaining it makes the same cleanup safely retryable.
bool discardOwnedDownloadTemp(const std::string& tmpPath, const std::string& markerPath) {
  const HalStorage::PathStatus tmpStatus = Storage.probePath(tmpPath.c_str());
  if (tmpStatus == HalStorage::PathStatus::Error) return false;
  if (tmpStatus == HalStorage::PathStatus::Present && !Storage.remove(tmpPath.c_str())) return false;
  return Storage.remove(markerPath.c_str());
}

bool caseInsensitiveEquals(const std::string& a, const char* b) {
  size_t i = 0;
  for (; b[i] != '\0'; i++) {
    if (i >= a.size()) return false;
    const char ca = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
    const char cb = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] - 'A' + 'a') : b[i];
    if (ca != cb) return false;
  }
  return i == a.size();
}

// Percent-encodes one query PARAMETER VALUE (not a whole URL): everything outside
// [A-Za-z0-9-_.~] is escaped, including '&'/'='/'+'/'#', which UrlUtils::encodeUnsafeUrlChars
// deliberately leaves alone because that helper encodes an already-structured URL, not a raw
// value being interpolated into one. Needed here because search text is free-form user input that
// may itself contain those characters.
std::string encodeQueryValue(const std::string& value) {
  static constexpr char HEX_DIGITS[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(value.size());
  for (const unsigned char c : value) {
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                            c == '_' || c == '.' || c == '~';
    if (unreserved) {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += HEX_DIGITS[(c >> 4) & 0xF];
      out += HEX_DIGITS[c & 0xF];
    }
  }
  return out;
}

// Bounded-body limits. Sized generously above a realistic full page (PAGE_SIZE=20 items) of the
// smallest plausible JSON encoding of each response, so a legitimate response is never truncated
// mid-item; a server that pads far beyond this is treated as misbehaving and its excess is simply
// never retained (the callback below stops appending once the cap is hit, it does not abort the
// transfer -- the HTTP request itself still completes normally).
// These caps bound what is RETAINED after BookOrbitJsonKeyDropper has replaced the values of the
// fields below (never read by this client) with null as the bytes arrive.
constexpr size_t MAX_ROOT_BODY = 2048;
constexpr size_t MAX_BOOKS_PAGE_BODY = 16384;
constexpr size_t MAX_FACET_PAGE_BODY = 16384;  // One streamed facet item plus envelope
constexpr size_t MAX_BOOK_DETAIL_BODY = 6144;

// Server fields this client never reads, dropped while streaming (packages/types/src/koreader.ts).
// The detail's description and related-book sections are the ones that matter: they can run to
// several KB and description precedes `files`, which a plain byte cap would cut off.
constexpr const char* ROOT_DROPPED_KEYS[] = {"href", "booksHref", "icon", "subtitle"};
constexpr const char* BOOKS_DROPPED_KEYS[] = {"thumbnailUrl", "detailUrl", "formats",     "hasCover",
                                              "addedAt",      "updatedAt", "nextUrl",     "previousUrl",
                                              "lastReadAt",   "hasPrevious", "seriesSummary"};
constexpr const char* FACET_DROPPED_KEYS[] = {"href", "booksHref", "icon", "subtitle", "nextUrl", "previousUrl", "query"};
constexpr const char* DETAIL_DROPPED_KEYS[] = {
    "description", "relatedSections", "collections", "genres",      "tags",        "thumbnailUrl",
    "detailUrl",   "downloadUrl",     "devicePath",  "subtitle",    "publisher",   "publishedDate",
    "isbn10",      "isbn13",          "addedAt",     "updatedAt",   "lastReadAt",  "formats",
    "progress",    "libraryName",     "language",    "durationSeconds"};

template <size_t N>
BookOrbitJsonKeyDropper makeDropper(const char* const (&keys)[N], std::string& body, size_t cap) {
  return BookOrbitJsonKeyDropper(keys, N, body, cap);
}


// Caps applied to server-supplied display strings so one pathological field (an author name
// measured in kilobytes) can't blow past this device's tiny per-item RAM budget when a page of
// PAGE_SIZE items is held in a std::vector at once.
constexpr size_t MAX_TITLE_CHARS = 200;
constexpr size_t MAX_AUTHORS_CHARS = 200;
// Bounds BookDetail::readStatus, an opaque server-reported string never validated against
// SETTABLE_READ_STATUSES on the read path (see BookOrbitCatalogClient.h's BookDetail comment) --
// generous enough for any real status value plus headroom for a future longer server vocabulary,
// without letting a pathological field consume unbounded RAM.
constexpr size_t MAX_STATUS_CHARS = 32;

// Truncates at a safe UTF-8 codepoint boundary, never a raw byte cut -- a server-supplied title
// or author name is arbitrary UTF-8 (Traditional Chinese included: MAX_TITLE_CHARS=200 is only
// ~66 CJK codepoints, well within reach of a real title on this branch), and a raw byte cut can
// sever a multi-byte sequence mid-codepoint. utf8SafeTruncateBuffer (lib/Utf8) is the same
// primitive StringUtils::sanitizeFilename already relies on for this.
std::string boundedString(const char* value, size_t maxChars) {
  if (!value) return "";
  std::string s(value);
  if (s.size() > maxChars) {
    s.resize(static_cast<size_t>(utf8SafeTruncateBuffer(s.data(), static_cast<int>(maxChars))));
  }
  return s;
}
constexpr size_t MAX_ID_CHARS = 128;

bool isLowerHex32(const char* s) {
  size_t n = 0;
  for (; s[n] != '\0'; ++n) {
    if (n >= 32 || !((s[n] >= '0' && s[n] <= '9') || (s[n] >= 'a' && s[n] <= 'f'))) return false;
  }
  return n == 32;
}

// BookOrbit's catalog sends book/file ids as JSON numbers (packages/types/src/koreader.ts:
// KoreaderCatalogBookListItem.id, KoreaderCatalogFile.id) and section/facet ids as strings. Accept
// either shape; anything else (null, float, object) is treated as absent so the entry is skipped.
// An id is an identifier, never display text: one longer than MAX_ID_CHARS is rejected (entry
// skipped), never truncated into a different, possibly colliding, id.
std::string jsonId(JsonVariantConst value) {
  if (value.is<const char*>()) {
    const char* id = value.as<const char*>();
    return strlen(id) <= MAX_ID_CHARS ? std::string(id) : std::string();
  }
  if (value.is<uint64_t>()) return std::to_string(value.as<uint64_t>());
  if (value.is<int64_t>()) return std::to_string(value.as<int64_t>());
  return {};
}

// Display text for the server's seriesIndex (a number such as 2 or 2.5, or a string).
std::string seriesIndexText(JsonVariantConst value) {
  if (value.is<const char*>()) return boundedString(value.as<const char*>(), 16);
  if (value.is<int64_t>()) return std::to_string(value.as<int64_t>());
  if (value.is<float>()) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%g", static_cast<double>(value.as<float>()));
    return buf;
  }
  return {};
}
constexpr size_t MAX_SERIES_CHARS = 120;

// Joins a JSON string array ("authors": ["A", "B"]) into one bounded, comma-joined display
// string, stopping once the shared cap is reached rather than growing without limit. Each
// separator+entry is measured and truncated as one unit BEFORE appending, so the result never
// overshoots maxChars and never ends on a bare trailing ", " with nothing after it.
std::string joinBoundedList(JsonArrayConst array, size_t maxChars) {
  std::string out;
  for (JsonVariantConst entry : array) {
    const char* s = entry.as<const char*>();
    if (!s) continue;
    const std::string separator = out.empty() ? "" : ", ";
    const std::string piece = separator + s;
    if (out.size() + piece.size() > maxChars) {
      const size_t remaining = maxChars - out.size();
      if (remaining > separator.size()) {
        const size_t safeLen = static_cast<size_t>(utf8SafeTruncateBuffer(piece.data(), static_cast<int>(remaining)));
        // Only keep the truncated piece if it holds more than the bare separator -- otherwise
        // this entry contributes nothing and the list ends cleanly on the previous one.
        if (safeLen > separator.size()) out.append(piece.data(), safeLen);
      }
      break;
    }
    out += piece;
  }
  return out;
}

std::string buildQueryUrl(const std::string& path, const std::vector<std::pair<std::string, std::string>>& params) {
  std::string url = BOOKORBIT_STORE.getBaseUrl() + path;
  bool first = true;
  for (const auto& param : params) {
    if (param.second.empty()) continue;
    url += (first ? '?' : '&');
    first = false;
    url += param.first;
    url += '=';
    url += encodeQueryValue(param.second);
  }
  return url;
}

}  // namespace

const char* BookOrbitCatalogClient::errorString(Error error) {
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
    case LOW_MEMORY:
      return "Not enough memory — please retry";
    case UNSUPPORTED:
      return "Not supported by this server";
    case BAD_REQUEST:
      return "Request rejected by server";
    case FILE_ERROR:
      return "Could not save file to SD card";
    case ABORTED:
      return "Cancelled";
  }
  return "Unknown error";
}

BookOrbitCatalogClient::Session::Session()
    : transport_(makeDefaultKOSyncTransport()), fileFetcher_(bookOrbitDefaultFileFetch) {}

// Deliberately does NOT default fileFetcher_ to bookOrbitDefaultFileFetch: doing so would make
// that function's address reachable from this constructor, which every JSON-only test uses (to
// inject a fake KOSyncTransport without touching downloads at all) -- and taking that address
// would require linking src/network/BookOrbitCatalogDownload.cpp (and transitively
// HttpDownloader.cpp) into a host test binary that has no ESP-IDF/wolfSSL stand-ins, exactly the
// coupling makeDefaultKOSyncTransport() above avoids by the same reasoning. A Session built this
// way simply cannot download (see the guard at the top of downloadFile()); only Session() and the
// (transport, fileFetcher) test constructor below can.
BookOrbitCatalogClient::Session::Session(std::unique_ptr<KOSyncTransport> transport)
    : transport_(std::move(transport)) {}

BookOrbitCatalogClient::Session::Session(std::unique_ptr<KOSyncTransport> transport, FileFetcher fileFetcher)
    : transport_(std::move(transport)), fileFetcher_(std::move(fileFetcher)) {}

BookOrbitCatalogClient::Session::~Session() {
  if (transport_) transport_->end();
}

bool BookOrbitCatalogClient::Session::heapTooLowForRequest() {
  if (prepareHeap_) prepareHeap_(prepareHeapContext_);
  return KOSyncRequest::insufficientHeap();
}

BookOrbitCatalogClient::Error BookOrbitCatalogClient::Session::fetchRootSections(std::vector<Section>& out,
                                                                                 const AbortCallback& shouldAbort) {
  out.clear();
  lastHttpCode_ = 0;
  if (!BOOKORBIT_STORE.hasCredentials()) return NO_CREDENTIALS;

  const std::string url = BOOKORBIT_STORE.getBaseUrl() + "/plugin/catalog/root";
  LOG_DBG("BOCatalog", "Fetching root sections: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (heapTooLowForRequest()) return LOW_MEMORY;

  if (!transport_->begin(url)) {
    LOG_ERR("BOCatalog", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  applyBookOrbitAuthHeaders(*transport_);

  std::string body;
  auto dropper = makeDropper(ROOT_DROPPED_KEYS, body, MAX_ROOT_BODY);
  const int httpCode = transport_->sendRequest(
      "GET", nullptr, 0,
      [&dropper](const uint8_t* data, size_t len) {
        dropper.feed(data, len);
        return true;
      },
      shouldAbort);
  lastHttpCode_ = httpCode;

  Error result = categorizeStatus(httpCode);
  if (result == OK) {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, body.c_str());
    if (error || !doc["sections"].is<JsonArrayConst>()) {
      LOG_DBG("BOCatalog", "Root sections: unparseable or unexpected shape (%s)", error.c_str());
      result = JSON_ERROR;
    } else {
      for (JsonObjectConst entry : doc["sections"].as<JsonArrayConst>()) {
        const char* id = entry["section"].is<const char*>() ? entry["section"].as<const char*>()
                                                            : entry["id"] | static_cast<const char*>(nullptr);
        const char* title = entry["title"] | static_cast<const char*>(nullptr);
        if (!id || !title) continue;  // malformed entry: skip it, don't fail the whole page
        Section section;
        section.id = boundedString(id, MAX_ID_CHARS);
        section.title = boundedString(title, MAX_TITLE_CHARS);
        out.push_back(std::move(section));
      }
    }
  }

  logOutcome("GET /plugin/catalog/root", httpCode, result);
  return result;
}

BookOrbitCatalogClient::Error BookOrbitCatalogClient::Session::fetchBooks(const BookQuery& query, BookPage& out,
                                                                          const AbortCallback& shouldAbort) {
  out = BookPage{};
  lastHttpCode_ = 0;
  if (!BOOKORBIT_STORE.hasCredentials()) return NO_CREDENTIALS;

  char pageBuf[12];
  char sizeBuf[12];
  const uint32_t wirePage = static_cast<uint32_t>(std::max(0, query.page)) + 1;
  snprintf(pageBuf, sizeof(pageBuf), "%u", static_cast<unsigned>(wirePage));
  snprintf(sizeBuf, sizeof(sizeBuf), "%d", PAGE_SIZE);
  const std::string url = buildQueryUrl("/plugin/catalog/books", {{"page", pageBuf},
                                                                  {"size", sizeBuf},
                                                                  {"q", query.searchText},
                                                                  {"author", query.authorId},
                                                                  {"seriesId", query.seriesId},
                                                                  {"libraryId", query.libraryId},
                                                                  {"collectionId", query.collectionId},
                                                                  {"smartScopeId", query.smartScopeId},
                                                                  {"sort", query.sort},
                                                                  {"readStatus", query.readStatus}});
  LOG_DBG("BOCatalog", "Fetching books: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (heapTooLowForRequest()) return LOW_MEMORY;

  if (!transport_->begin(url)) {
    LOG_ERR("BOCatalog", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  applyBookOrbitAuthHeaders(*transport_);

  std::string body;
  auto dropper = makeDropper(BOOKS_DROPPED_KEYS, body, MAX_BOOKS_PAGE_BODY);
  bool invalidItem = false;
  dropper.setItemSink([&](const char* data, size_t len) {
    if (out.items.size() >= static_cast<size_t>(PAGE_SIZE)) return;
    JsonDocument item;
    if (deserializeJson(item, data, len) || !item.is<JsonObjectConst>()) { invalidItem = true; return; }
    const auto entry = item.as<JsonObjectConst>();
    const std::string id = jsonId(entry["id"]);
    const char* title = entry["title"] | static_cast<const char*>(nullptr);
    if (id.empty() || !title) return;
    BookSummary book;
    book.id = id;
    if (entry["progressPercentage"].is<float>()) {
      const float pct = entry["progressPercentage"].as<float>();
      if (pct >= 0.0f && pct <= 100.0f) book.progressPercent = pct;
    }
    if (const char* rs = entry["readStatus"] | static_cast<const char*>(nullptr)) {
      book.readStatus = boundedString(rs, MAX_STATUS_CHARS);
    }
    book.title = boundedString(title, MAX_TITLE_CHARS);
    if (entry["authors"].is<JsonArrayConst>()) {
      book.authors = joinBoundedList(entry["authors"].as<JsonArrayConst>(), MAX_AUTHORS_CHARS);
    }
    if (const char* series = entry["seriesName"] | static_cast<const char*>(nullptr)) {
      book.seriesName = boundedString(series, MAX_SERIES_CHARS);
      book.seriesIndex = seriesIndexText(entry["seriesIndex"]);
    }
    out.items.push_back(std::move(book));
  });
  const int httpCode = transport_->sendRequest(
      "GET", nullptr, 0,
      [&dropper](const uint8_t* data, size_t len) {
        dropper.feed(data, len);
        return true;
      },
      shouldAbort);
  lastHttpCode_ = httpCode;

  Error result = categorizeStatus(httpCode);
  if (result == OK) {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, body.c_str());
    if (error || invalidItem || dropper.truncated() || !doc["items"].is<JsonArrayConst>()) {
      LOG_DBG("BOCatalog", "Books page: unparseable or unexpected shape (%s)", error.c_str());
      result = JSON_ERROR;
    } else {

      // Prefer an explicit total/page/size accounting when present; otherwise infer from whether
      // this page came back full (a partial last page cannot have a next page).
      if (doc["total"].is<uint32_t>()) out.total = doc["total"].as<uint32_t>();
      if (doc["total"].is<uint32_t>() && doc["size"].is<uint32_t>()) {
        const uint32_t total = doc["total"].as<uint32_t>();
        const uint32_t size = doc["size"].as<uint32_t>();
        const uint32_t page = doc["page"] | wirePage;
        out.hasNext = size > 0 && static_cast<uint64_t>(page) * size < total;
      } else {
        out.hasNext = out.items.size() >= static_cast<size_t>(PAGE_SIZE);
      }

    }
  }

  if (result != OK) out = BookPage{};
  logOutcome("GET /plugin/catalog/books", httpCode, result);
  return result;
}

BookOrbitCatalogClient::Error BookOrbitCatalogClient::Session::fetchSectionEntries(const std::string& sectionId,
                                                                                   const int page, FacetPage& out,
                                                                                   const AbortCallback& shouldAbort) {
  out = FacetPage{};
  lastHttpCode_ = 0;
  if (!BOOKORBIT_STORE.hasCredentials()) return NO_CREDENTIALS;
  if (sectionId.empty() || sectionId.size() > MAX_ID_CHARS) return BAD_REQUEST;

  char pageBuf[12];
  char sizeBuf[12];
  const uint32_t wirePage = static_cast<uint32_t>(std::max(0, page)) + 1;
  snprintf(pageBuf, sizeof(pageBuf), "%u", static_cast<unsigned>(wirePage));
  snprintf(sizeBuf, sizeof(sizeBuf), "%d", PAGE_SIZE);
  const std::string url =
      buildQueryUrl("/plugin/catalog/sections/" + encodeQueryValue(sectionId), {{"page", pageBuf}, {"size", sizeBuf}});
  LOG_DBG("BOCatalog", "Fetching section entries: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (heapTooLowForRequest()) return LOW_MEMORY;

  if (!transport_->begin(url)) {
    LOG_ERR("BOCatalog", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  applyBookOrbitAuthHeaders(*transport_);

  std::string body;
  auto dropper = makeDropper(FACET_DROPPED_KEYS, body, MAX_FACET_PAGE_BODY);
  bool invalidItem = false;
  dropper.setItemSink([&](const char* data, size_t len) {
    if (out.items.size() >= static_cast<size_t>(MAX_FACET_PAGE_ITEMS)) return;
    JsonDocument item;
    if (deserializeJson(item, data, len) || !item.is<JsonObjectConst>()) { invalidItem = true; return; }
    const auto entry = item.as<JsonObjectConst>();
    // A series entry's id ("series:<n>") is not a valid seriesId filter; its numeric seriesId is.
    const std::string id = entry["seriesId"].is<uint64_t>() ? jsonId(entry["seriesId"]) : jsonId(entry["id"]);
    const char* title = entry["title"] | static_cast<const char*>(nullptr);
    if (id.empty() || !title) return;
    FacetEntry facet;
    facet.id = id;
    facet.title = boundedString(title, MAX_TITLE_CHARS);
    facet.bookCount = entry["count"] | 0;
    facet.hasBookCount = entry["count"].is<uint32_t>();
    out.items.push_back(std::move(facet));
  });
  const int httpCode = transport_->sendRequest(
      "GET", nullptr, 0,
      [&dropper](const uint8_t* data, size_t len) {
        dropper.feed(data, len);
        return true;
      },
      shouldAbort);
  lastHttpCode_ = httpCode;

  Error result = categorizeStatus(httpCode);
  if (result == OK) {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, body.c_str());
    if (error || invalidItem || dropper.truncated() || !doc["items"].is<JsonArrayConst>()) {
      LOG_DBG("BOCatalog", "Section entries: unparseable or unexpected shape (%s)", error.c_str());
      result = JSON_ERROR;
    } else {

      // Only the server knows its facet page size, so without hasNext there is no next page.
      out.hasNext = doc["hasNext"].is<bool>() && doc["hasNext"].as<bool>();

    }
  }

  if (result != OK) out = FacetPage{};
  logOutcome("GET /plugin/catalog/sections/:id", httpCode, result);
  return result;
}

BookOrbitCatalogClient::Error BookOrbitCatalogClient::Session::fetchBookDetail(const std::string& bookId,
                                                                               const std::string& deviceId,
                                                                               BookDetail& out,
                                                                               const AbortCallback& shouldAbort) {
  out = BookDetail{};
  lastHttpCode_ = 0;
  if (!BOOKORBIT_STORE.hasCredentials()) return NO_CREDENTIALS;
  if (bookId.empty() || bookId.size() > MAX_ID_CHARS) return BAD_REQUEST;
  if (deviceId.empty()) {
    LOG_ERR("BOCatalog", "Refusing book-detail request without a persisted installation id");
    return BAD_REQUEST;
  }

  // Per-installation unique id, not BookOrbitLimits::DEVICE_ID's shared literal -- see this
  // method's own header doc comment for why (the server looks up a per-device file-naming-pattern
  // override keyed on this value).
  const std::string url = buildQueryUrl("/plugin/catalog/books/" + encodeQueryValue(bookId), {{"deviceId", deviceId}});
  LOG_DBG("BOCatalog", "Fetching book detail: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (heapTooLowForRequest()) return LOW_MEMORY;

  if (!transport_->begin(url)) {
    LOG_ERR("BOCatalog", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  applyBookOrbitAuthHeaders(*transport_);

  std::string body;
  auto dropper = makeDropper(DETAIL_DROPPED_KEYS, body, MAX_BOOK_DETAIL_BODY);
  const int httpCode = transport_->sendRequest(
      "GET", nullptr, 0,
      [&dropper](const uint8_t* data, size_t len) {
        dropper.feed(data, len);
        return true;
      },
      shouldAbort);
  lastHttpCode_ = httpCode;

  Error result = categorizeStatus(httpCode);
  if (result == OK) {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, body.c_str());
    const std::string id = error ? std::string() : jsonId(doc["id"]);
    const char* title = doc["title"] | static_cast<const char*>(nullptr);
    if (error || id.empty() || !title) {
      LOG_DBG("BOCatalog", "Book detail: unparseable or unexpected shape (%s)", error.c_str());
      result = JSON_ERROR;
    } else {
      out.id = id;
      out.title = boundedString(title, MAX_TITLE_CHARS);
      if (doc["authors"].is<JsonArrayConst>()) {
        out.authors = joinBoundedList(doc["authors"].as<JsonArrayConst>(), MAX_AUTHORS_CHARS);
      }
      if (const char* series = doc["seriesName"] | static_cast<const char*>(nullptr)) {
        out.seriesName = boundedString(series, MAX_SERIES_CHARS);
        out.seriesIndex = seriesIndexText(doc["seriesIndex"]);
      }
      if (doc["progressPercentage"].is<float>()) {
        const float pct = doc["progressPercentage"].as<float>();
        if (pct >= 0.0f && pct <= 100.0f) out.progressPercent = pct;
      }
      if (doc["files"].is<JsonArrayConst>()) {
        for (JsonObjectConst fileEntry : doc["files"].as<JsonArrayConst>()) {
          const std::string fileId = jsonId(fileEntry["id"]);
          const char* format = fileEntry["format"] | static_cast<const char*>(nullptr);
          if (fileId.empty() || !format) continue;
          if (!caseInsensitiveEquals(format, "epub")) continue;  // CrossPoint is EPUB-only
          BookFile file;
          file.id = fileId;
          file.format = "epub";
          file.sizeBytes = fileEntry["sizeBytes"] | 0;
          // KOReader partial MD5 of the exact bytes the download route streams. Kept only when it
          // is exactly 32 lowercase hex, so a malformed value can never match a local file.
          if (const char* hash = fileEntry["fileHash"] | static_cast<const char*>(nullptr)) {
            if (isLowerHex32(hash)) file.fileHash = hash;
          }
          out.files.push_back(std::move(file));
        }
      }
      if (const char* readStatus = doc["readStatus"] | static_cast<const char*>(nullptr)) {
        out.readStatus = boundedString(readStatus, MAX_STATUS_CHARS);
      }
      // Never `doc["rating"] | -1` here: ArduinoJson's `|` operator only yields the fallback on a
      // TYPE mismatch, and a JSON number (including a JSON null, which is its own type) both parse
      // as themselves -- `| -1` would happily return 0 for an explicit `"rating":0` and never
      // actually distinguish "unrated" from "unrated". isNull() is the correct, explicit check.
      out.rating = doc["rating"].isNull() ? -1 : doc["rating"].as<int>();
    }
  }

  logOutcome("GET /plugin/catalog/books/:id", httpCode, result);
  return result;
}

BookOrbitCatalogClient::Error BookOrbitCatalogClient::Session::setReadStatus(const std::string& bookId,
                                                                             const std::string& status,
                                                                             std::string& outReadStatus,
                                                                             const AbortCallback& shouldAbort) {
  outReadStatus.clear();
  lastHttpCode_ = 0;
  if (!BOOKORBIT_STORE.hasCredentials()) return NO_CREDENTIALS;
  if (bookId.empty() || bookId.size() > MAX_ID_CHARS) return BAD_REQUEST;
  bool validStatus = false;
  for (size_t i = 0; i < SETTABLE_READ_STATUS_COUNT; i++) {
    if (status == SETTABLE_READ_STATUSES[i]) {
      validStatus = true;
      break;
    }
  }
  if (!validStatus) return BAD_REQUEST;

  const std::string url =
      BOOKORBIT_STORE.getBaseUrl() + "/plugin/catalog/books/" + encodeQueryValue(bookId) + "/read-status";
  LOG_DBG("BOCatalog", "Setting read status: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (heapTooLowForRequest()) return LOW_MEMORY;

  JsonDocument doc;
  doc["status"] = status;
  std::string body;
  serializeJson(doc, body);

  if (!transport_->begin(url)) {
    LOG_ERR("BOCatalog", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  applyBookOrbitAuthHeaders(*transport_);
  transport_->addHeader("Content-Type", "application/json");

  std::string responseBody;
  const int httpCode = transport_->sendRequest(
      "PUT", reinterpret_cast<const uint8_t*>(body.data()), body.size(),
      [&responseBody](const uint8_t* data, size_t len) {
        if (responseBody.size() < MAX_BOOK_DETAIL_BODY)
          responseBody.append(reinterpret_cast<const char*>(data),
                              std::min(len, MAX_BOOK_DETAIL_BODY - responseBody.size()));
        return true;
      },
      shouldAbort);
  lastHttpCode_ = httpCode;

  Error result = categorizeStatus(httpCode);
  if (result == OK) {
    JsonDocument responseDoc;
    const DeserializationError error = deserializeJson(responseDoc, responseBody.c_str());
    const char* readStatus = responseDoc["readStatus"] | static_cast<const char*>(nullptr);
    if (error || !readStatus) {
      LOG_DBG("BOCatalog", "Read-status response: unparseable or unexpected shape (%s)", error.c_str());
      result = JSON_ERROR;
    } else {
      outReadStatus = boundedString(readStatus, MAX_STATUS_CHARS);
    }
  }

  logOutcome("PUT /plugin/catalog/books/:id/read-status", httpCode, result);
  return result;
}

BookOrbitCatalogClient::Error BookOrbitCatalogClient::Session::setRating(const std::string& bookId, const int rating,
                                                                         int& outRating,
                                                                         const AbortCallback& shouldAbort) {
  outRating = -1;
  lastHttpCode_ = 0;
  if (!BOOKORBIT_STORE.hasCredentials()) return NO_CREDENTIALS;
  if (bookId.empty() || bookId.size() > MAX_ID_CHARS) return BAD_REQUEST;
  if (rating < -1 || rating > 5) return BAD_REQUEST;

  const std::string url =
      BOOKORBIT_STORE.getBaseUrl() + "/plugin/catalog/books/" + encodeQueryValue(bookId) + "/rating";
  LOG_DBG("BOCatalog", "Setting rating: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (heapTooLowForRequest()) return LOW_MEMORY;

  JsonDocument doc;
  if (rating < 0) {
    doc["rating"] = nullptr;
  } else {
    doc["rating"] = rating;
  }
  std::string body;
  serializeJson(doc, body);

  if (!transport_->begin(url)) {
    LOG_ERR("BOCatalog", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  applyBookOrbitAuthHeaders(*transport_);
  transport_->addHeader("Content-Type", "application/json");

  std::string responseBody;
  const int httpCode = transport_->sendRequest(
      "PUT", reinterpret_cast<const uint8_t*>(body.data()), body.size(),
      [&responseBody](const uint8_t* data, size_t len) {
        if (responseBody.size() < MAX_BOOK_DETAIL_BODY)
          responseBody.append(reinterpret_cast<const char*>(data),
                              std::min(len, MAX_BOOK_DETAIL_BODY - responseBody.size()));
        return true;
      },
      shouldAbort);
  lastHttpCode_ = httpCode;

  Error result = categorizeStatus(httpCode);
  if (result == OK) {
    JsonDocument responseDoc;
    const DeserializationError error = deserializeJson(responseDoc, responseBody.c_str());
    if (error || !responseDoc.is<JsonObjectConst>()) {
      LOG_DBG("BOCatalog", "Rating response: unparseable or unexpected shape (%s)", error.c_str());
      result = JSON_ERROR;
    } else {
      outRating = responseDoc["rating"].isNull() ? -1 : responseDoc["rating"].as<int>();
    }
  }

  logOutcome("PUT /plugin/catalog/books/:id/rating", httpCode, result);
  return result;
}

BookOrbitCatalogClient::Error BookOrbitCatalogClient::Session::downloadFile(const std::string& fileId,
                                                                            const std::string& destPath,
                                                                            const ProgressCallback& progress,
                                                                            bool* cancelFlag) {
  lastHttpCode_ = 0;
  if (!fileFetcher_) {
    LOG_ERR("BOCatalog", "No file fetcher configured for this session");
    return FILE_ERROR;
  }
  if (!BOOKORBIT_STORE.hasCredentials()) return NO_CREDENTIALS;
  if (fileId.empty() || fileId.size() > MAX_ID_CHARS) return BAD_REQUEST;

  const std::string url =
      BOOKORBIT_STORE.getBaseUrl() + "/plugin/catalog/files/" + encodeQueryValue(fileId) + "/download";
  LOG_DBG("BOCatalog", "Downloading file: %s -> %s (heap: %u)", url.c_str(), destPath.c_str(),
          (unsigned)ESP.getFreeHeap());
  if (heapTooLowForRequest()) return LOW_MEMORY;

  const std::string tmpPath = destPath + ".tmp";
  const std::string bakPath = destPath + ".bak";
  const std::string markerPath = destPath + ".bookorbit-download.txn";

  // .tmp and .bak live beside a user-selected filename, so their names alone never prove that this
  // app created them. Probe all four paths once and touch sidecars only when the durable marker has
  // the exact transaction magic and destination. This preserves legitimate user files such as
  // dune.epub.bak while still making our own interrupted promotions recoverable.
  const HalStorage::PathStatus destStatus = Storage.probePath(destPath.c_str());
  const HalStorage::PathStatus bakStatus = Storage.probePath(bakPath.c_str());
  const HalStorage::PathStatus tmpStatus = Storage.probePath(tmpPath.c_str());
  const HalStorage::PathStatus markerStatus = Storage.probePath(markerPath.c_str());
  if (destStatus == HalStorage::PathStatus::Error || bakStatus == HalStorage::PathStatus::Error ||
      tmpStatus == HalStorage::PathStatus::Error || markerStatus == HalStorage::PathStatus::Error) {
    LOG_ERR("BOCatalog", "Could not determine download transaction state; refusing to modify any file");
    return FILE_ERROR;
  }

  if (markerStatus == HalStorage::PathStatus::Absent) {
    if (bakStatus == HalStorage::PathStatus::Present || tmpStatus == HalStorage::PathStatus::Present) {
      LOG_ERR("BOCatalog", "Unowned download sidecar exists; preserving it and refusing destination %s",
              destPath.c_str());
      return FILE_ERROR;
    }
  } else {
    if (!matchesDownloadTransaction(markerPath, destPath)) {
      LOG_ERR("BOCatalog", "Download transaction marker is unreadable or does not match; refusing recovery");
      return FILE_ERROR;
    }

    if (bakStatus == HalStorage::PathStatus::Present && destStatus == HalStorage::PathStatus::Absent) {
      if (!Storage.rename(bakPath.c_str(), destPath.c_str())) {
        LOG_ERR("BOCatalog", "Could not recover a hidden original file at %s", bakPath.c_str());
        return FILE_ERROR;  // marker retained, making a later retry safe
      }
      LOG_INF("BOCatalog", "Recovered a file left hidden by an interrupted download: %s", destPath.c_str());
      if (!discardOwnedDownloadTemp(tmpPath, markerPath)) {
        LOG_ERR("BOCatalog", "Recovered the original but could not finish transaction cleanup");
      }
      // Do not stack a fresh network transaction onto a call that just restored the only complete
      // copy. A manual/automatic retry proceeds after this recovery-only pass.
      return FILE_ERROR;
    }

    if (bakStatus == HalStorage::PathStatus::Present) {
      // The marker proves this is the old destination left after tmp -> dest had succeeded.
      if (!Storage.remove(bakPath.c_str())) {
        LOG_ERR("BOCatalog", "Could not clear an owned stale download backup at %s", bakPath.c_str());
        return FILE_ERROR;
      }
      LOG_INF("BOCatalog", "Cleared a download backup left over from an interrupted promotion cleanup");
    }
    if (!discardOwnedDownloadTemp(tmpPath, markerPath)) {
      LOG_ERR("BOCatalog", "Could not finish interrupted download transaction cleanup");
      return FILE_ERROR;
    }
  }

  // Ownership must become durable before the fetcher can create/truncate .tmp. A failed marker
  // close means no sidecar may be touched or sent to the network this attempt.
  if (!writeDownloadTransaction(markerPath, destPath)) {
    LOG_ERR("BOCatalog", "Could not persist the download transaction marker");
    Storage.remove(markerPath.c_str());
    return FILE_ERROR;
  }

  const FetchOutcome outcome = fileFetcher_(url, tmpPath, progress, cancelFlag);
  lastHttpCode_ = outcome.httpCode;

  if (outcome.aborted) {
    if (!discardOwnedDownloadTemp(tmpPath, markerPath)) {
      LOG_ERR("BOCatalog", "Could not clean up an aborted download transaction");
    }
    logOutcome("GET /plugin/catalog/files/:id/download", lastHttpCode_, ABORTED);
    return ABORTED;
  }
  if (outcome.localFileError) {
    if (!discardOwnedDownloadTemp(tmpPath, markerPath)) {
      LOG_ERR("BOCatalog", "Could not clean up a failed local download transaction");
    }
    logOutcome("GET /plugin/catalog/files/:id/download", lastHttpCode_, FILE_ERROR);
    return FILE_ERROR;
  }
  Error result = categorizeStatus(outcome.httpCode);
  if (result != OK) {
    if (!discardOwnedDownloadTemp(tmpPath, markerPath)) {
      LOG_ERR("BOCatalog", "Could not clean up a rejected download transaction");
    }
    logOutcome("GET /plugin/catalog/files/:id/download", lastHttpCode_, result);
    return result;
  }

  // Promote tmp -> dest, staging any pre-existing file at dest aside as .bak first (same ordering
  // as BookOrbitStatsQueue::promoteReplacement, and for the same reason: at every instant at least
  // one of dest/.bak is a complete file, so a rename failure or power loss here never leaves this
  // filename with no readable copy at all).
  const HalStorage::PathStatus destinationBeforePromote = Storage.probePath(destPath.c_str());
  if (destinationBeforePromote == HalStorage::PathStatus::Error) {
    LOG_ERR("BOCatalog", "Could not determine whether a destination exists before promotion");
    discardOwnedDownloadTemp(tmpPath, markerPath);
    return FILE_ERROR;
  }
  const bool stagedExisting = destinationBeforePromote == HalStorage::PathStatus::Present;
  if (stagedExisting && !Storage.rename(destPath.c_str(), bakPath.c_str())) {
    LOG_ERR("BOCatalog", "Could not stage existing file aside before promoting download");
    discardOwnedDownloadTemp(tmpPath, markerPath);
    return FILE_ERROR;
  }
  if (!Storage.rename(tmpPath.c_str(), destPath.c_str())) {
    LOG_ERR("BOCatalog", "Could not promote downloaded file; restoring previous file if any");
    bool restored = true;
    if (stagedExisting && !Storage.rename(bakPath.c_str(), destPath.c_str())) {
      LOG_ERR("BOCatalog", "Could not restore staged-aside file; it remains at %s", bakPath.c_str());
      restored = false;
    }
    if (restored && !discardOwnedDownloadTemp(tmpPath, markerPath)) {
      LOG_ERR("BOCatalog", "Could not finish cleanup after a failed promotion");
    }
    return FILE_ERROR;
  }
  if (stagedExisting && !Storage.remove(bakPath.c_str())) {
    // The new destination is complete. Retain the marker so the next call can prove ownership and
    // retry cleanup without ever confusing this backup for a user's file.
    LOG_ERR("BOCatalog", "Download succeeded but its old destination backup could not be removed");
  } else if (!Storage.remove(markerPath.c_str())) {
    LOG_ERR("BOCatalog", "Download succeeded but its transaction marker could not be removed");
  }

  logOutcome("GET /plugin/catalog/files/:id/download", lastHttpCode_, OK);
  return OK;
}
