#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "KOSyncTransport.h"

/**
 * HTTP client for BookOrbit's catalog browsing/download API surface: root sections, paged book
 * listings with search/author/series/library/collection/SmartScope filters and sort/read-status
 * selection, author/series/library/collection/SmartScope facet listings, book detail, per-book
 * read-status and star-rating edits, and single-file download.
 *
 * Wire contract (endpoint paths, query parameter names, response field shapes) for the surface
 * originally shipped (root sections, book listing with search/author/series filters, author/series
 * facets, book detail, download) is adapted, with attribution, from the MIT-licensed
 * CrossInk-BookOrbit fork (https://github.com/agosez/CrossInk-Bookorbit, commit 92cc1be,
 * src/network/BookOrbitCatalogClient.{h,cpp}). The surface added afterward -- library/collection/
 * SmartScope query filters and facet sections, continue-reading/recent/all-books root-section sort
 * and read-status filter, and the read-status/rating edit endpoints -- has no CrossInk client
 * counterpart to adapt from, so its field names and route paths are instead read, clean-room, as
 * protocol description directly from the official BookOrbit server's own route/DTO/type-definition
 * source (https://github.com/bookorbit/bookorbit, AGPL-3.0, commit
 * 9165dbed1dd9b30b075d74c8a718a1a4673f7e95: koreader-catalog.controller.ts,
 * dto/koreader-catalog-query.dto.ts, packages/types/src/koreader.ts) -- reading route/DTO/type
 * shapes for field names is the same clean-room precedent already used for BookOrbitBookmarks.h's
 * wire types, never copying that server's own service/business-logic implementation. Everything
 * below this comment -- the request/response plumbing, bounded-body accumulation, struct layout,
 * capability gating, and every line of C++ -- is an independent, clean-room implementation written
 * for this codebase's own host-testable KOSyncTransport abstraction and RAM ceiling, not a port of
 * either source's own implementation.
 *
 * Deliberately NOT implemented, because no verified field-level wire contract is usable from this
 * device (see docs/bookorbit-feature-matrix.md for the full per-item reasoning):
 *   - Dashboard scrollers (GET .../dashboard, .../dashboard/discover, .../dashboard/sections/:type)
 *     and bulk manifest download (GET .../manifest). Both routes and response shapes ARE now
 *     verified (gated behind Capabilities::hasCatalogDashboardSections /
 *     ::hasCatalogBulkManifest, which this client still never queries), but the aggregate dashboard
 *     response mixes three book lists with widget objects too large for this client's bounded-body
 *     accumulation strategy, and the manifest's cursor/restart contract needs a durable local
 *     checkpoint store this codebase does not yet have -- a prerequisite gap, not a contract gap.
 *
 * Base URL: BookOrbitCredentialStore::getBaseUrl(), the same "/api/v1/koreader" base every other
 * BookOrbitSyncClient request uses -- catalog routes are additional endpoints on the same
 * koreader-plugin controller as /plugin/version and /plugin/page-stats, not a separate API root.
 *
 * CrossPoint is EPUB-only, so every method that returns file entries has already filtered out any
 * non-EPUB format the server lists; a book whose only files are e.g. PDF/MOBI comes back with an
 * empty file list rather than files the reader could never open.
 */
class BookOrbitCatalogClient {
 public:
  using AbortCallback = KOSyncTransport::AbortCallback;
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;

  enum Error {
    OK = 0,
    NO_CREDENTIALS,
    NETWORK_ERROR,
    AUTH_FAILED,
    SERVER_ERROR,
    JSON_ERROR,
    LOW_MEMORY,
    UNSUPPORTED,  // endpoint absent on this server (404/405/501)
    BAD_REQUEST,  // server rejected the request shape (400/413/422)
    FILE_ERROR,   // local SD write/rename failure (download only)
    ABORTED,      // caller-provided AbortCallback returned true mid-transfer
  };

  static const char* errorString(Error error);

  /** One entry in the server's root catalog menu (GET .../plugin/catalog/root). */
  struct Section {
    std::string id;     // machine key, e.g. "all-books", "authors", "series"
    std::string title;  // server-supplied display title
  };

  /** One book as listed in a page of results (never carries file info -- see BookDetail). */
  struct BookSummary {
    std::string id;
    std::string title;
    std::string authors;            // joined with ", "; bounded, see BookOrbitCatalogLimits::MAX_AUTHORS_CHARS
    float progressPercent = -1.0f;  // server's progressPercentage 0-100, or -1 when absent
    std::string readStatus;         // display-only, opaque; empty when absent
  };

  /** One page of book listing results. */
  struct BookPage {
    std::vector<BookSummary> items;
    bool hasNext = false;
  };

  /** One author or series facet entry (GET .../plugin/catalog/sections/{id}). */
  struct FacetEntry {
    std::string id;
    std::string title;
    uint32_t bookCount = 0;
  };

  /** One page of facet listing results. */
  struct FacetPage {
    std::vector<FacetEntry> items;
    bool hasNext = false;
  };

  /** One downloadable file on a book, already filtered to formats this reader can open (EPUB). */
  struct BookFile {
    std::string id;
    std::string format;
    uint64_t sizeBytes = 0;
    // Server's KOReader partial MD5 of the streamed bytes (32 lowercase hex), or empty when the
    // server did not send one. Used to verify a pre-existing local copy before linking it.
    std::string fileHash;
  };

  /** Full detail for one book (GET .../plugin/catalog/books/{id}). */
  struct BookDetail {
    std::string id;
    std::string title;
    std::string authors;
    std::vector<BookFile> files;  // EPUB-only; empty if the book has no compatible format
    // Server's current read-status for this book, opaque and display-only: shown verbatim (see
    // BookOrbitCatalogBrowserActivity), never fed back into setReadStatus() unmodified, because
    // this field's server-side vocabulary is not verified to be a strict subset of
    // SETTABLE_READ_STATUSES below -- only a value the user explicitly picked from that fixed list
    // is ever sent back. Empty when the server omitted or nulled the field.
    std::string readStatus;
    int rating = -1;  // 0-5 = current star rating; -1 = unrated (server sent null/omitted)
  };

  /**
   * Every value setReadStatus() accepts, exactly KOREADER_CATALOG_SETTABLE_READ_STATUSES from the
   * server's own koreader-catalog-query.dto.ts (see this header's class comment for the source).
   * NOT the same vocabulary as BookQuery::readStatus below (a 3-value read-only filter: "unread" /
   * "reading" / "finished"), and NOT the "complete"-spelled vocabulary a bulk book-states upload
   * would use -- three distinct, non-interchangeable enumerations on three different routes.
   */
  static constexpr const char* SETTABLE_READ_STATUSES[] = {
      "unread", "want_to_read", "reading", "on_hold", "read", "abandoned",
  };
  static constexpr size_t SETTABLE_READ_STATUS_COUNT = 6;

  /** Filter/sort/paging parameters for fetchBooks(). Empty strings mean "no filter". */
  struct BookQuery {
    std::string searchText;
    std::string authorId;
    std::string seriesId;
    std::string libraryId;
    std::string collectionId;
    std::string smartScopeId;
    std::string sort;        // "" = server default; else one of KOREADER_CATALOG_SORTS (e.g. "title")
    std::string readStatus;  // "" = no filter; else "unread" / "reading" / "finished"
    int page = 0;            // UI/navigation page, 0-based; serialized to BookOrbit as page + 1
  };

  // One page of book listing results, matching CrossInk's own PAGE_SIZE so the server-side
  // pagination this client relies on (hasNext / total items per page) behaves identically to the
  // fork this protocol is adapted from.
  static constexpr int PAGE_SIZE = 20;

  /** Outcome of one fetch-to-file attempt, as reported by a FileFetcher (see below). */
  struct FetchOutcome {
    int httpCode = 0;             // 0 if the transport never reached a server
    bool localFileError = false;  // SD write/flush failed even though the HTTP layer succeeded
    bool aborted = false;         // caller's AbortCallback/cancelFlag stopped the transfer
  };

  // Fetches `url` to `tmpPath` on SD (bounded memory, streamed, never buffering the whole body).
  // Injectable so downloadFile()'s atomic temp+backup+promote sequence -- the part actually worth
  // covering with red-capable host tests (short write, close/flush failure, rename failure) -- is
  // testable without ESP-IDF/wolfSSL headers, a real device, or a network at all. Production code
  // never constructs this directly; Session's default constructor wires it to
  // bookOrbitDefaultFileFetch() (declared below, defined in
  // src/network/BookOrbitCatalogDownload.cpp -- see that file's comment for why it is not defined
  // alongside this class).
  using FileFetcher = std::function<FetchOutcome(const std::string& url, const std::string& tmpPath,
                                                 const ProgressCallback& progress, bool* cancelFlag)>;

  // One HTTP session reused across every request made through it, mirroring
  // BookOrbitSyncClient::Session's reuse contract: construct ONE Session per browsing/download
  // transaction and let it go out of scope when that transaction completes.
  class Session {
   public:
    Session();
    explicit Session(std::unique_ptr<KOSyncTransport> transport);
    // Test-only: injects both the JSON transport and the download fetch step. downloadFile()'s
    // JSON sibling methods above only ever need the transport, so this overload exists solely for
    // exercising downloadFile()'s SD-side atomicity from a host test.
    Session(std::unique_ptr<KOSyncTransport> transport, FileFetcher fileFetcher);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Called synchronously right before each request's TLS heap gate (every network method,
    // including downloadFile()). The caller frees memory it can rebuild -- on device, the SD font
    // page arenas the status screen just rendered with -- so the gate measures the heap the
    // handshake will actually get. Not called for requests rejected before the gate (missing
    // credentials, bad arguments). The gate itself and its thresholds are unchanged.
    using PrepareHeapFn = void (*)(void* context);
    void setPrepareHeap(PrepareHeapFn fn, void* context) {
      prepareHeap_ = fn;
      prepareHeapContext_ = context;
    }

    /** GET .../plugin/catalog/root -- the server's advertised top-level browsing entry points. */
    Error fetchRootSections(std::vector<Section>& out, const AbortCallback& shouldAbort = nullptr);

    /** GET .../plugin/catalog/books, filtered/paged/sorted per `query`. */
    Error fetchBooks(const BookQuery& query, BookPage& out, const AbortCallback& shouldAbort = nullptr);

    /**
     * GET .../plugin/catalog/sections/{sectionId} -- facet listing for one of the server's
     * root-advertised drill-down sections: "authors", "series", "libraries", "collections", or
     * "smart-scopes". An unrecognized sectionId returns whatever the server itself reports
     * (typically UNSUPPORTED or BAD_REQUEST via categorizeStatus), not a client-side rejection --
     * this client does not maintain its own copy of which section ids are valid.
     */
    Error fetchSectionEntries(const std::string& sectionId, int page, FacetPage& out,
                              const AbortCallback& shouldAbort = nullptr);

    /**
     * GET .../plugin/catalog/books/{bookId} -- full detail, files filtered to EPUB only, plus the
     * book's current read-status/rating (BookDetail::readStatus / ::rating) so a caller can show
     * them before offering an edit.
     *
     * `deviceId` MUST be a persisted per-installation unique identifier (e.g. BookOrbitStats::
     * statsDeviceId()), never BookOrbitLimits::DEVICE_ID's shared literal: the server uses it to
     * look up this device's own file-naming-pattern override (getDeviceFileNamingPattern(userId,
     * deviceId) in the official server's koreader-catalog.service.ts), so two physical devices
     * sharing an id here would each read back the other's naming-pattern setting. The same stable
     * installation id is also used by BookOrbit progress and bookmark calls; only the sibling
     * generic-KOSync client retains BookOrbitLimits::DEVICE_ID's shared literal.
     */
    Error fetchBookDetail(const std::string& bookId, const std::string& deviceId, BookDetail& out,
                          const AbortCallback& shouldAbort = nullptr);

    /**
     * PUT .../plugin/catalog/books/{bookId}/read-status. `status` MUST be one of
     * SETTABLE_READ_STATUSES; any other value (including empty) is rejected client-side as
     * BAD_REQUEST without a request, since the server's own DTO validator would reject it anyway
     * and this avoids spending a round-trip finding that out. `outReadStatus` receives the server's
     * echoed value on success (normally identical to `status`).
     */
    Error setReadStatus(const std::string& bookId, const std::string& status, std::string& outReadStatus,
                        const AbortCallback& shouldAbort = nullptr);

    /**
     * PUT .../plugin/catalog/books/{bookId}/rating. `rating` of -1 clears the rating (sent as JSON
     * `null`, matching KoreaderCatalogSetRatingDto's `rating ?? null`); 0-5 sets it. Any other value
     * is rejected client-side as BAD_REQUEST without a request. `outRating` receives the server's
     * echoed value on success (-1 if cleared).
     */
    Error setRating(const std::string& bookId, int rating, int& outRating, const AbortCallback& shouldAbort = nullptr);

    /**
     * GET .../plugin/catalog/files/{fileId}/download, streamed directly to `destPath` on SD with
     * bounded memory (no full-body buffering) via the same temp+backup+promote atomic-replace
     * protocol as BookOrbitStatsQueue's queue-file rewrites -- see BookOrbitCatalogClient.cpp for
     * the full sequence. No byte-range resume: the server contract for Range/206 support is not
     * verified, so an interrupted download restarts from the beginning rather than risking a
     * silently-corrupt partial file being treated as valid (see docs/bookorbit-feature-matrix.md).
     * On any failure, destPath is left exactly as it was before the call (never partially
     * overwritten) and all temporary files this call created are removed.
     */
    Error downloadFile(const std::string& fileId, const std::string& destPath,
                       const ProgressCallback& progress = nullptr, bool* cancelFlag = nullptr);

    int lastHttpCode() const { return lastHttpCode_; }

   private:
    // prepareHeap_ then KOSyncRequest::insufficientHeap(): true means LOW_MEMORY.
    bool heapTooLowForRequest();

    std::unique_ptr<KOSyncTransport> transport_;
    FileFetcher fileFetcher_;
    PrepareHeapFn prepareHeap_ = nullptr;
    void* prepareHeapContext_ = nullptr;
    int lastHttpCode_ = 0;
  };
};

// Production FileFetcher: thin wrapper around HttpDownloader::downloadToFile() with BookOrbit's
// auth headers. Declared here but DEFINED in src/network/BookOrbitCatalogDownload.cpp, not
// alongside BookOrbitCatalogClient.cpp -- this header/class lives in lib/KOReaderSync, which
// PlatformIO's library dependency scanner does not give visibility into src/network's
// HttpDownloader.h (every existing consumer of that header is itself under src/); moving the one
// function that needs it into a src/-resident translation unit keeps this library's own
// dependency set (KOSyncTransport, BookOrbitCredentialStore) exactly as before, and is also what
// keeps this file host-testable -- see BookOrbitCatalogClientTest's CMakeLists.txt comment on why
// Session's default constructor (the only thing that references this function) is never called by
// any host test.
BookOrbitCatalogClient::FetchOutcome bookOrbitDefaultFileFetch(const std::string& url, const std::string& tmpPath,
                                                               const BookOrbitCatalogClient::ProgressCallback& progress,
                                                               bool* cancelFlag);
