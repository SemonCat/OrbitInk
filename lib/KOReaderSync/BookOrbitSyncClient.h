#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "BookOrbitBookmarks.h"
#include "BookOrbitStats.h"
#include "KOSyncTransport.h"

/**
 * Progress data for BookOrbit's KOSync-compatible progress endpoints.
 *
 * No metadata or rich-position fields: KOReaderMetadata and KOReaderRichPosition
 * (KOReaderSyncClient.h) are crosspoint-sync-only extensions BookOrbit's own server has no
 * concept of, so there is nothing here for them to carry.
 */
struct BookOrbitProgress {
  std::string document;     // Document hash -- see Session::getProgress's doc comment
  std::string progress;     // XPath-like progress string
  float percentage = 0.0f;  // Progress percentage (0.0 to 1.0)
  std::string device;       // Device display name
  // updateProgress() only: per-installation unique id (e.g. BookOrbitStats::statsDeviceId()),
  // never BookOrbitLimits::DEVICE_ID's shared literal. Contrary to this file's own earlier claim
  // that progress sync's device_id is "purely informational", the official BookOrbit server keys
  // a per-device progress row and a reset-convergence hold on it (koreader.service.ts
  // upsertDeviceProgress/resolveResetHold) -- two physical devices sharing an id would each be
  // told the other's reset already converged. getProgress()'s own read path stays
  // deviceId-agnostic (it picks whichever device row is most recently updated), so switching this
  // away from the old shared constant loses no data: the old shared-id row simply goes stale.
  // Parsed from GET responses as well, where the official server uses it to identify a standing
  // web reset (`bookorbit-web`).
  std::string deviceId;
  int64_t timestamp = 0;  // Unix timestamp of last update, as reported by the server

  bool isWebReset() const {
    return percentage == 0.0f && progress == "/body/DocFragment[1]/body" && device == "web" &&
           deviceId == "bookorbit-web";
  }
};

/**
 * HTTP client for the BookOrbit-specific API surface: authentication/test-connection, progress
 * sync, and the page-stats extension. Catalog browsing/download lives in
 * BookOrbitCatalogClient (a separate Session, same server); annotations and bookmarks are added
 * by later phases (see docs/bookorbit-feature-matrix.md).
 *
 * Deliberately NOT a method on KOReaderSyncClient::Session (which it was in e51b383): BookOrbit
 * is now an explicit, standalone provider with its own credential store (BookOrbitCredentialStore)
 * and its own server URL, so hanging its requests off the KOSync client would keep the exact
 * coupling this phase exists to remove. It shares KOSyncTransport (this codebase's existing
 * bounded, host-testable HTTP abstraction) and the request-shaping helpers in
 * KOSyncRequestHelpers.h -- the correct shared primitive, since both providers speak the same
 * KOSync-derived wire dialect (auth headers, "any 2xx is success", the same TLS heap gate) -- but
 * has its own Error vocabulary and Session type so its evolution (catalog, annotations, ...) never
 * has to touch KOReaderSyncClient's public API or vice versa.
 *
 * Base URL: BookOrbitCredentialStore::getBaseUrl() (self-hosted only, no public default),
 * normalized to end in "/api/v1/koreader".
 */
class BookOrbitSyncClient {
 public:
  using AbortCallback = KOSyncTransport::AbortCallback;

  enum Error {
    OK = 0,
    NO_CREDENTIALS,
    NETWORK_ERROR,
    AUTH_FAILED,
    SERVER_ERROR,
    JSON_ERROR,
    LOW_MEMORY,
    // The server answered, but not with a BookOrbit-shaped response (wrong body shape on
    // /plugin/version, or an "authorized" field that is present but not "OK") -- a genuine
    // configuration problem to surface to the user, distinct from a network/auth failure that
    // may resolve itself on retry.
    NOT_BOOKORBIT,
    // The server answered, but the endpoint does not exist there (404/405/501). Used only by the
    // page-stats extension, where "this BookOrbit server predates page-stats" is a capability
    // answer to cache, not a transient failure to retry.
    UNSUPPORTED,
    // The server rejected the request (400/413/422): never identifies an individual item, so it
    // is not on its own grounds to discard queued data. See BookOrbitStatsSync.cpp.
    BAD_REQUEST,
    // GET progress only: the server has never seen this document hash (HTTP 404 or 204), not a
    // transport failure -- mirrors KOReaderSyncClient::Error::NOT_FOUND's own contract.
    NOT_FOUND,
    PROGRESS_NOT_CONFIRMED  // PUT accepted, but the selected GET position did not match.
  };

  static const char* errorString(Error error);

  /**
   * Capabilities confirmed for the currently configured server, from GET .../plugin/version.
   * Named booleans rather than a generic string set/vector: the server's capabilities[] array is
   * open-ended and this device only ever needs to ask about a small, known set (bounded RAM, no
   * unbounded string storage) -- an unrecognized capability string is simply not represented here.
   */
  struct Capabilities {
    bool isBookOrbit = false;
    // capabilities[] entries this firmware currently knows to look for. Absent from the array,
    // or the array itself absent/malformed, both read as false -- callers must not assume a
    // capability is present just because detection succeeded.
    bool hasBookmarkSync = false;
    bool hasCatalogBulkManifest = false;
    bool hasCatalogDashboardSections = false;
  };

  // One HTTP session reused across every request made through it, mirroring
  // KOReaderSyncClient::Session's reuse contract (see its doc comment): construct ONE Session per
  // sync transaction and let it go out of scope when that transaction completes.
  class Session {
   public:
    Session();
    explicit Session(std::unique_ptr<KOSyncTransport> transport);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    /** Authenticated GET .../users/auth -- validates the configured credentials. */
    Error authenticate(const AbortCallback& shouldAbort = nullptr);

    /**
     * Identity + capability probe: authenticated GET .../plugin/version. Populates `out`
     * regardless of outcome (zero-valued on failure) and returns OK only when the server answered
     * with a recognizably BookOrbit-shaped body; NOT_BOOKORBIT for a well-formed but wrong-shaped
     * 2xx (e.g. a reverse-proxy catch-all), UNSUPPORTED when the endpoint itself is absent (a
     * server that predates this probe), and the usual transport/auth/heap errors otherwise.
     */
    Error probeCapabilities(Capabilities& out, const AbortCallback& shouldAbort = nullptr);

    /**
     * GET .../syncs/progress/:document -- fetch the server-selected latest reading position for this
     * document hash. Callers must pass KOReaderDocumentId::calculateForStats()'s binary
     * partial-MD5 (never a filename-mode hash): BookOrbitCredentialStore.h's own doc comment
     * notes BookOrbit has no document-matching-method setting, unlike KOReaderCredentialStore --
     * this is an inference from that design, not independently verified against a live server's
     * /syncs/progress route specifically (only its page-stats route has been).
     *
     * Populates `outProgress` only on OK; leaves it default-constructed otherwise, including
     * NOT_FOUND.
     */
    Error getProgress(const std::string& documentHash, BookOrbitProgress& outProgress,
                      const AbortCallback& shouldAbort = nullptr);

    /** PUT .../syncs/progress -- push a reading position. `progress.document` must be set. */
    Error updateProgress(const BookOrbitProgress& progress, const AbortCallback& shouldAbort = nullptr);

    // Normal pushes require a readback. A mismatch preserves local progress and must not
    // trigger another PUT in this transaction. Reset-start acknowledgement uses raw PUT:
    // BookOrbit deliberately keeps returning its synthetic reset until advance converges.
    Error updateProgressAndVerify(const BookOrbitProgress& progress, const AbortCallback& shouldAbort = nullptr);

    /**
     * POST .../plugin/page-stats for one document. Moved here unchanged (payload shape, server
     * contract, and error categorization) from KOReaderSyncClient::Session::uploadPageStats in
     * e51b383 -- see BookOrbitSyncClient.cpp for the full contract comment that used to live next
     * to that method.
     */
    Error uploadPageStats(const std::string& documentHash, const std::string& deviceId, const std::string& deviceModel,
                          const BookOrbitStatEvent* events, size_t count, BookOrbitStatsAck& outAck,
                          const AbortCallback& shouldAbort = nullptr);

    /**
     * POST .../plugin/bookmarks/exchange -- two-way: local changes go up in `changes`, and
     * whatever the server wants applied here comes back in `outIncoming` (capped at
     * BOOKORBIT_BOOKMARK_BATCH; a fuller server-side queue is reflected in `outMorePending` and
     * simply arrives on a later call, never as a bigger response here).
     *
     * `keys` describes the device's COMPLETE local bookmark set, used for the server's own
     * deletion diff: an id present on the server but absent from `keys` is deleted here. Pass
     * `keysComplete=false` when the set could not be built in full (e.g. some local bookmarks
     * lack a minted identity this round) -- the server then skips deletion processing entirely
     * rather than reading the gap as deletions. Sending an empty `keys` WITH `keysComplete=true`
     * is meaningful too: it tells the server this device holds no bookmarks for this book at all.
     *
     * `outUnmatched` is set when the server does not recognize `documentHash` at all (distinct
     * from "recognizes it but has nothing new") -- callers should retain everything and retry
     * later rather than treat this as an error.
     *
     * `deviceId` MUST be a persisted per-installation unique identifier (e.g. BookOrbitStats::
     * statsDeviceId()), never BookOrbitLimits::DEVICE_ID's shared literal: the server's own
     * bookmark-exchange service partitions its per-device delivery/tombstone-tracking state by
     * this value (loadWorkingSet(userId, deviceId, bookId), deleteLinks(userId, deviceId, ...)).
     * Two physical devices sharing an id here would each be told the other's already-applied
     * changes are already theirs too, silently dropping changes that were never actually
     * delivered to them. (Progress sync's own device_id -- BookOrbitProgress::deviceId -- has a
     * different, milder per-device dependency; see its doc comment.)
     */
    Error exchangeBookmarks(const std::string& documentHash, const std::string& deviceId,
                            const std::string& deviceModel, const BookOrbitBookmarkKeyRow* keys, size_t keyCount,
                            bool keysComplete, const BookOrbitBookmark* changes, size_t changeCount, bool& outUnmatched,
                            std::vector<BookOrbitIncomingBookmark>& outIncoming, bool& outMorePending,
                            const AbortCallback& shouldAbort = nullptr);

    /**
     * POST .../plugin/bookmarks/exchange-ack -- acknowledge server-side changes that were applied
     * (or definitively could not be, via `applied`'s `failed` flag) or deleted locally. The server
     * keeps every change it offered pending until named here, so an entry left out of both lists
     * is simply re-offered on the next exchangeBookmarks() call -- callers must only ever name
     * what they are certain landed (or definitively failed), never anything left unresolved.
     *
     * `deviceId` must be the SAME persisted per-installation unique identifier passed to
     * exchangeBookmarks() -- see that method's own doc comment for why.
     */
    Error ackBookmarks(const std::string& documentHash, const std::string& deviceId, const std::string& deviceModel,
                       const std::vector<BookOrbitBookmarkAck>& applied, const std::vector<uint32_t>& deletedServerIds,
                       const AbortCallback& shouldAbort = nullptr);

    int lastHttpCode() const { return lastHttpCode_; }

   private:
    std::unique_ptr<KOSyncTransport> transport_;
    int lastHttpCode_ = 0;
  };
};
