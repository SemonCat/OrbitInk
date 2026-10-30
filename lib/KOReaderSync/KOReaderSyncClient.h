#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "BookOrbitStats.h"
#include "KOSyncTransport.h"

/**
 * Optional document metadata sent alongside progress sync requests.
 * Mirrors the metadata object added in KOReader PR #15306.
 * The official sync server ignores this field; custom servers may use it.
 */
struct KOReaderMetadata {
  std::string filename;  // e.g. "my_book.epub"
  std::string title;     // Document title from EPUB metadata
  std::string authors;   // Author(s) from EPUB metadata
};

/**
 * Rich CrossPoint position sent alongside progress uploads. Maps 1:1 onto the
 * crosspoint-sync extended `position` object (see crosspoint-sync docs/API.md).
 * It is only transmitted to sync.crosspointreader.com, where it makes
 * CrossPoint<->CrossPoint sync lossless instead of xpath-approximated.
 */
struct KOReaderRichPosition {
  uint32_t pctQ = 0;                       // Percentage quantized 0..1,000,000 (authoritative)
  uint16_t spineIndex = 0;                 // Spine (chapter) index
  uint16_t pageNumber = 0;                 // Page within spine (layout-dependent hint)
  uint16_t totalPages = 1;                 // Spine page count (layout-dependent hint)
  std::optional<uint16_t> paragraphIndex;  // Synthetic 1-based paragraph index
  std::string xpath;                       // KOReader-style xpath (server cap: 120 bytes)
};

/**
 * Progress data from KOReader sync server.
 */
struct KOReaderProgress {
  std::string document;                          // Document hash
  std::string progress;                          // XPath-like progress string
  float percentage = 0.0f;                       // Progress percentage (0.0 to 1.0)
  std::string device;                            // Device name
  std::string deviceId;                          // Device ID
  int64_t timestamp = 0;                         // Unix timestamp of last update
  std::optional<KOReaderMetadata> metadata;      // Optional document metadata
  std::optional<KOReaderRichPosition> position;  // Optional rich position (crosspoint-sync servers only)
};

/**
 * HTTP client for KOReader sync API.
 *
 * Base URL: https://sync.koreader.rocks:443/
 *
 * API Endpoints:
 *   GET /users/auth - Authenticate (validate credentials)
 *   GET /syncs/progress/:document - Get progress for a document
 *   PUT /syncs/progress - Update progress for a document
 *
 * Authentication:
 *   x-auth-user: username
 *   x-auth-key: MD5 hash of password
 */
class KOReaderSyncClient {
 public:
  // Checked periodically during a request (between header lines and body chunks); returning true
  // aborts the transfer cleanly (see SecureHttpClient's own AbortCallback, which this forwards
  // to). Used by the auto-sync paths (KOReaderAutoSync) to bail out promptly if the user presses
  // a button mid-sync, rather than running the request to its own timeout. nullptr (the default)
  // never aborts, preserving existing callers' behavior.
  using AbortCallback = KOSyncTransport::AbortCallback;

  enum Error {
    OK = 0,
    NO_CREDENTIALS,
    NETWORK_ERROR,
    AUTH_FAILED,
    SERVER_ERROR,
    JSON_ERROR,
    NOT_FOUND,
    LOW_MEMORY,
    USER_EXISTS,
    // The server answered, but the endpoint does not exist there (404/405/501). Used only by
    // the optional BookOrbit page-stats extension, where "this server has no page-stats
    // endpoint" is a capability answer to cache, not a transient failure to retry. Never
    // produced by the core progress requests, whose own 404 means "no progress stored yet".
    UNSUPPORTED,
    // The server rejected the request (400/413/422). Distinguished from a transient failure so
    // a caller can react differently -- e.g. retry an oversized batch smaller -- but it never
    // identifies an individual item, so it is not on its own grounds to discard queued data.
    BAD_REQUEST
  };

  // One HTTP session reused across every request made through it, so a full sync transaction
  // (primary GET, optional alternate-hash smart-sync GET, PUT) normally pays for one TLS
  // handshake instead of one per request -- freeink::SecureHttpClient already keeps a connection
  // open and reuses it whenever scheme/host/port match (setReuse(true) is its default; see
  // SecureHttpClientTransport.h), this class just avoids discarding that reusable connection
  // between calls the way the one-shot static methods below intentionally do.
  //
  // Construct ONE Session per sync transaction (manual sync, auto push, auto pull) and let it
  // go out of scope when that transaction completes -- non-copyable, mirrors this codebase's
  // "one expensive shared resource, lent for a bounded scope, no leak" RAII idiom (see
  // GfxRenderer::FrameBufferLoan). No global/static client instance exists, and nothing here
  // spawns a background task: all requests remain synchronous, called from whichever task owns
  // the transaction (main task for auto push/pull, the KOReaderSyncActivity's normal task for
  // manual sync) exactly as the static methods already did.
  //
  // hashKind labels each request for the INFO log (see .cpp) as "primary" or "alternate" --
  // smart-sync manual sync probes both a document's primary and alternate match-method hashes;
  // this makes that distinction visible in a release-level serial capture without logging the
  // hash value itself.
  class Session {
   public:
    Session();
    void end();
    // Test-only: inject a fake transport instead of the real SecureHttpClient-backed one.
    explicit Session(std::unique_ptr<KOSyncTransport> transport);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    Error authenticate();
    Error createUser();
    Error getProgress(const std::string& documentHash, KOReaderProgress& outProgress,
                      const AbortCallback& shouldAbort = nullptr, const char* hashKind = "primary");
    Error updateProgress(const KOReaderProgress& progress, const AbortCallback& shouldAbort = nullptr,
                         const char* hashKind = "primary");

    /** HTTP status code from the last request on this session (for diagnostics). */
    int lastHttpCode() const { return lastHttpCode_; }

   private:
    std::unique_ptr<KOSyncTransport> transport_;
    int lastHttpCode_ = 0;
  };

  // One-shot convenience wrappers for callers that make a single request and don't need
  // connection reuse (e.g. KOReaderAuthActivity's one-time authenticate()/createUser() calls).
  // Each constructs a throwaway Session internally; behavior and signatures are unchanged from
  // before this class gained Session.
  static Error authenticate();
  static Error createUser();
  static Error getProgress(const std::string& documentHash, KOReaderProgress& outProgress,
                           const AbortCallback& shouldAbort = nullptr);
  static Error updateProgress(const KOReaderProgress& progress, const AbortCallback& shouldAbort = nullptr);

  /**
   * Get human-readable error message.
   */
  static const char* errorString(Error error);

  /** HTTP status code from the last request made via the static one-shot methods above. */
  static int lastHttpCode;
};
