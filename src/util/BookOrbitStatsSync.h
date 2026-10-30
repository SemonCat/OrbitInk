#pragma once
#include <BookOrbitOwnerId.h>
#include <BookOrbitSyncClient.h>

#include <cstdint>
#include <string>

/**
 * App-layer policy for the BookOrbit page-stats feature: who is allowed to capture events, when
 * the device is allowed to talk to a server about them, and what a given server response means
 * for the durable queue.
 *
 * The protocol itself lives in lib/KOReaderSync (BookOrbitSyncClient, BookOrbitStatsQueue); this
 * is the piece that knows about settings, persisted state, and the sync lifecycle.
 *
 * EXPLICIT PROVIDER, not opportunistic detection: this reads BookOrbitCredentialStore, never
 * KOReaderCredentialStore. e51b383 inferred BookOrbit by probing whatever server generic KOSync
 * happened to be pointed at; that coupling is gone (see docs/bookorbit-feature-matrix.md and
 * src/util/BookOrbitMigration.h, which carries existing users' queued events across the change).
 *
 * Gating, in the order it is applied:
 *  1. The existing Reading Stats setting must be on. Off means this feature does not exist -- no
 *     probe, no capture, no queue, no request. It never turns itself on.
 *  2. BookOrbitCredentialStore credentials (username, password, AND server URL) must be
 *     configured.
 *  3. The server must be confirmed BookOrbit by capability probe (Session::probeCapabilities).
 *
 * The probe result is cached in persisted app state, keyed on the credential fingerprint, so a
 * confirmed server is probed once per configuration rather than once per sync. Unlike e51b383
 * there is no cached NEGATIVE: since the server is now explicitly configured by the user rather
 * than guessed, a probe that fails to confirm BookOrbit (endpoint absent, wrong-shaped response)
 * is a configuration problem to surface -- via BookOrbitAuthActivity's test-connection action --
 * not a fact to cache forever and act on by disabling the feature or discarding the queue. It is
 * simply retried on the next sync-eligible moment, at the cost of one extra small GET per attempt
 * until the server answers correctly or the user fixes the configuration.
 */
namespace BookOrbitStats {

/**
 * 128-bit fingerprint of the configured server+account (see BookOrbitOwnerId). All-zero when
 * credentials are missing, which never matches a recorded value, so an unconfigured device
 * always reads back unconfirmed.
 */
BookOrbitOwnerId currentOwner();

/**
 * This installation's random, persisted BookOrbit identifier. It is supplied to page stats,
 * progress, bookmark, and catalog calls whose server state is verified to be per-device. A state
 * reset intentionally creates a new identity so a fresh installation cannot inherit an old
 * client's delivery acknowledgements, tombstones, or progress reset-convergence state.
 *
 * Separate from BookOrbitLimits::DEVICE_ID, which remains only in the sibling generic-KOSync
 * progress client. BookOrbit builds a reading-session id from the first eight characters of THIS
 * value, so two devices on one account need distinct ones or their sessions merge server-side.
 */
const std::string& statsDeviceId();

/**
 * Cached capabilities for the CURRENTLY configured server (see BookOrbitSyncClient::Capabilities).
 * isBookOrbit is false when the key has changed since the last confirmation, or nothing has been
 * confirmed yet -- callers must re-probe (uploadQueued() below does this automatically) rather
 * than treat that as a settled negative.
 */
BookOrbitSyncClient::Capabilities cachedCapabilities();

/**
 * Persist a confirmed-BookOrbit probe result against the current identity key. Defers the write
 * while a legacy migration is pending because koStatsServerId still carries the legacy queue
 * owner that runOnce() needs to finish reconciliation safely.
 */
void recordCapabilities(const BookOrbitSyncClient::Capabilities& caps);

/**
 * True when the reader should capture and persist per-page events: reading stats on, credentials
 * present, and this server already confirmed BookOrbit.
 *
 * A first session against a newly configured BookOrbit server reads false, because the probe has
 * not run yet -- it runs during that session's own closing sync. Only that one session's events
 * are missed, in exchange for the hard guarantee that a server not yet confirmed BookOrbit never
 * causes a single queue write.
 */
bool captureEnabled();

/**
 * True when a stats drain could achieve anything for the configured server: reading stats on and
 * credentials present. Unlike e51b383's drainPossible() this is no longer additionally gated on
 * "server not already settled as plain KOSync" -- that settled-negative state no longer exists
 * (see the class comment) -- so this differs from captureEnabled() only in not yet requiring
 * confirmation, which lets an unconfirmed server's drain still run its own probe. It is also
 * false while a legacy credential/queue-owner migration is pending, preventing capture or drain
 * from replacing a still-legacy-owned queue before runOnce() can reconcile it.
 *
 * Exists so callers can skip deriving the stats document id at all when there is nothing this
 * drain could do. That id is KOReader's binary partial MD5, which reads chunks from the book
 * file, and there is no reason to spend those SD reads when no credentials are configured.
 */
bool drainPossible();

/**
 * Confirm the server is BookOrbit if not already cached, probing on `session` when needed.
 *
 * Factored out of uploadQueued() so BookOrbitBookmarkSync (src/util/BookOrbitBookmarkSync.h) can
 * share the exact same probe-and-cache discipline rather than re-probing independently -- two
 * separate probes in the same sync window would be wasted requests, and a diverging gating rule
 * between stats and bookmarks would be a correctness hazard, not just duplication.
 *
 * Returns the confirmed (or still-unconfirmed) capabilities in `outCaps` regardless of outcome.
 * A non-OK probe result is a configuration problem to surface elsewhere (BookOrbitAuthActivity's
 * test-connection), not a fact this call acts on -- callers simply see `outCaps.isBookOrbit`
 * remain false and skip whatever they gated on it.
 */
BookOrbitSyncClient::Error ensureCapabilitiesConfirmed(BookOrbitSyncClient::Session& session,
                                                       BookOrbitSyncClient::Capabilities& outCaps,
                                                       const BookOrbitSyncClient::AbortCallback& shouldAbort = nullptr);

/** What one book's drain established, for a caller deciding whether to try another book. */
enum class DrainResult : uint8_t {
  Drained,   // queue empty, or every uploaded batch acknowledged
  Retained,  // book-specific retention: unmatched, unconfirmed, request rejected, batch budget spent
  Stopped,   // gating, capability, endpoint, transport, auth, low memory, abort/deadline or storage
};

/**
 * Drain the book's queued events to the server on `session`.
 *
 * Every failure here is soft by construction: it logs, leaves the queue intact for a later attempt,
 * and cannot affect anything else the caller is doing. The returned DrainResult only tells a
 * caller draining several books in one window whether trying another book could succeed.
 *
 * `documentHash` must be the STATS identity -- KOReaderDocumentId::calculateForStats(), the
 * binary partial MD5 -- never a filename-mode progress hash, which BookOrbit can never resolve
 * through its page-stats endpoint (see KOReaderDocumentId.h's own doc comment on this).
 *
 * Response handling:
 *  - Confirmed ingestion -> drop exactly the records that were uploaded.
 *  - 2xx but the book is unmatched on the server -> keep them (the book may be added later); the
 *    cap and staleness bound keep that bounded.
 *  - Endpoint absent, or a 2xx that is not BookOrbit-shaped -> KEEP the queue and leave the
 *    capability cache unconfirmed (see the class comment: this is now a configuration problem to
 *    surface, not a settled fact to act on by discarding history).
 *  - Request rejected (400/413/422) -> retain and defer. The status describes the REQUEST, not
 *    any one event, so nothing here identifies a record worth deleting. An oversized batch (413)
 *    is retried smaller first, since that is the one case where request size is the actual
 *    remedy.
 *  - Auth/network/server/low-memory -> keep everything and stop for this transaction.
 */
DrainResult uploadQueued(BookOrbitSyncClient::Session& session, const std::string& documentHash,
                         const BookOrbitSyncClient::AbortCallback& shouldAbort = nullptr);

/**
 * Convenience overload: opens its own throwaway BookOrbitSyncClient::Session and delegates.
 * Unlike e51b383, this no longer rides an already-open KOSync progress-sync connection, since
 * BookOrbit is now commonly a different host entirely -- every production call site uses this
 * overload. Callers do not need Wi-Fi already up (this establishes its own connection), but
 * calling it only when Wi-Fi already happens to be connected for another reason (as both current
 * call sites do) avoids an extra radio wake solely for this feature -- see
 * docs/bookorbit-feature-matrix.md's Phase 3 plan to give this its own independent sync trigger.
 */
DrainResult uploadQueued(const std::string& documentHash,
                         const BookOrbitSyncClient::AbortCallback& shouldAbort = nullptr);

/**
 * True when `name` is exactly a live queue file for a real stats id (32 lowercase hex + ".bin"),
 * with the id in `outHash`. Temp/backup sidecars and sanitized non-hex keys are rejected.
 */
bool isQueueFileName(const char* name, std::string& outHash);

struct WindowLimits {
  uint32_t budgetMs = 15000;  // whole stats phase: current book + scan + other books
  size_t maxOtherBooks = 3;   // non-empty other-book queues drained per window
};

struct WindowResult {
  DrainResult current = DrainResult::Drained;
  size_t otherAttempted = 0;  // non-empty other-book queues drained
  size_t scannedEntries = 0;  // directory entries examined
  bool stopped = false;       // an other-book drain returned Stopped
};

/**
 * One sync window's stats phase on the caller's already-open `session`: the current book's queue
 * (if `currentHash` is non-empty), then up to `limits.maxOtherBooks` OTHER books' non-empty queues
 * of the current owner -- so events read offline for books that are not reopened still reach the
 * server, without another WiFi connection or session.
 *
 *  - Stops the whole window as soon as any drain returns Stopped (it would only repeat).
 *  - One deadline covers the current book, the directory scan and every other book, and is the
 *    abort passed to each request, so the transport's own abort polling enforces it mid-request.
 *  - Fair: other books rotate through a persisted owner-scoped cursor (StatsDrainSelector), so
 *    empty, stale or foreign-owner queue files early in directory order can never starve a pending
 *    queue later in it; every queue is examined within ceil(N / 8) windows for a directory of up to
 *    1024 entries. A book whose drain Stopped is not passed: it is first in line next window.
 *  - No other book's EPUB is opened: a queue's file name is its stats id.
 */
WindowResult drainWindow(BookOrbitSyncClient::Session& session, const std::string& currentHash,
                         const WindowLimits& limits, const BookOrbitSyncClient::AbortCallback& shouldAbort = nullptr);

}  // namespace BookOrbitStats
