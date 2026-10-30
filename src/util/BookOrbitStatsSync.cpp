#include "BookOrbitStatsSync.h"

#include <Arduino.h>
#include <BookOrbitCredentialStore.h>
#include <BookOrbitSessionClock.h>
#include <BookOrbitStatsQueue.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "FileContentEquals.h"
#include "StatsDrainCursor.h"

namespace BookOrbitStats {

namespace {

// Two full batches (64 events) of actual draining, plus one spare so an oversized batch can
// still spend an attempt on a smaller retry instead of ending the sync having sent nothing. A
// deliberate ceiling either way on how long one drain may hold the radio.
constexpr int MAX_BATCHES_PER_TRANSACTION = 3;
// drainWindow() bounds: directory entries examined per window (the queue directory holds at most
// one live file + transient sidecars per book), and candidate ids examined after the cursor.
constexpr size_t MAX_OTHER_QUEUE_SCAN_ENTRIES = 1024;
constexpr size_t OTHER_QUEUE_CANDIDATES = 8;

bool migrationPending() { return APP_STATE.boCredentialsMigrated && !APP_STATE.boMigrationAttempted; }

DrainResult drainOne(BookOrbitSyncClient::Session& session, const std::string& documentHash,
                     const BookOrbitSyncClient::AbortCallback& shouldAbort);

}  // namespace

const std::string& statsDeviceId() {
  // Cached for the boot and persisted across boots. A state reset intentionally creates a new id:
  // BookOrbit's delivery/tombstone/reset state belongs to an installation, not immutable hardware.
  static std::string cached;
  if (!cached.empty()) return cached;

  if (bookOrbitStatsDeviceIdIsCurrent(APP_STATE.koStatsDeviceId)) {
    cached = APP_STATE.koStatsDeviceId;
    return cached;
  }

  uint8_t entropy[BookOrbitLimits::STATS_DEVICE_ID_ENTROPY_BYTES] = {};
  bookOrbitFirmwareRandom(entropy, sizeof(entropy));
  const std::string candidate = bookOrbitStatsDeviceId(entropy);
  if (candidate.empty()) {
    LOG_ERR("BOStats", "Could not generate a BookOrbit installation id");
    return cached;
  }

  // A 16-character value is the cycle-1 hardware-derived format; the shared fallback is shorter.
  // Treat both as migration inputs, not durable identities. Persist the replacement BEFORE
  // returning it: if state storage fails, callers receive empty and the protocol layer refuses to
  // send, rather than using an ephemeral or shared id that can acknowledge another client's data.
  const std::string previous = APP_STATE.koStatsDeviceId;
  APP_STATE.koStatsDeviceId = candidate;
  if (!APP_STATE.saveToFile()) {
    APP_STATE.koStatsDeviceId = previous;
    LOG_ERR("BOStats", "Could not persist the BookOrbit installation id; sync is disabled for this attempt");
    return cached;
  }
  cached = candidate;
  return cached;
}

BookOrbitOwnerId currentOwner() {
  if (!BOOKORBIT_STORE.hasCredentials()) return BookOrbitOwnerId{};
  return BookOrbitOwnerId::derive(BOOKORBIT_STORE.getBaseUrl(), BOOKORBIT_STORE.getUsername());
}

BookOrbitSyncClient::Capabilities cachedCapabilities() {
  BookOrbitSyncClient::Capabilities caps;
  const BookOrbitOwnerId owner = currentOwner();
  if (owner.isZero()) return caps;
  BookOrbitOwnerId recorded;
  if (!BookOrbitOwnerId::fromHex(APP_STATE.koStatsServerId, recorded) || recorded != owner) return caps;
  if (APP_STATE.koStatsServerKind != 1) return caps;

  caps.isBookOrbit = true;
  caps.hasBookmarkSync = APP_STATE.koStatsCapBookmarkSync;
  caps.hasCatalogBulkManifest = APP_STATE.koStatsCapCatalogBulkManifest;
  caps.hasCatalogDashboardSections = APP_STATE.koStatsCapCatalogDashboardSections;
  return caps;
}

void recordCapabilities(const BookOrbitSyncClient::Capabilities& caps) {
  // koStatsServerId still carries the LEGACY owner while a migration is waiting to rewrite its
  // queues. Progress/bookmark/Test Connection all share this recorder; allowing any of their
  // successful probes to replace that id with the new BookOrbit owner would make the next boot
  // reject the legacy detection as stale and abandon the unfinished rewrite. Defer the cache
  // write for this short-lived state; runOnce() retries at boot and clears the gate when done.
  if (migrationPending()) {
    LOG_DBG("BOStats", "Deferring capability cache update while the legacy migration is pending");
    return;
  }

  const BookOrbitOwnerId owner = currentOwner();
  if (owner.isZero()) return;
  const std::string hex = owner.toHex();
  const uint8_t kind = caps.isBookOrbit ? 1 : 0;
  if (APP_STATE.koStatsServerId == hex && APP_STATE.koStatsServerKind == kind &&
      APP_STATE.koStatsCapBookmarkSync == caps.hasBookmarkSync &&
      APP_STATE.koStatsCapCatalogBulkManifest == caps.hasCatalogBulkManifest &&
      APP_STATE.koStatsCapCatalogDashboardSections == caps.hasCatalogDashboardSections) {
    return;  // value-change guard: never rewrite state.json for an unchanged answer
  }
  APP_STATE.koStatsServerId = hex;
  APP_STATE.koStatsServerKind = kind;
  APP_STATE.koStatsCapBookmarkSync = caps.hasBookmarkSync;
  APP_STATE.koStatsCapCatalogBulkManifest = caps.hasCatalogBulkManifest;
  APP_STATE.koStatsCapCatalogDashboardSections = caps.hasCatalogDashboardSections;
  if (!APP_STATE.saveToFile()) {
    LOG_ERR("BOStats", "Could not persist BookOrbit capability probe result");
  }
}

bool drainPossible() {
  // While a legacy-KOSync migration is IN PROGRESS (credentials already copied, but the
  // queue-owner rewrite/koStatsServerId update not yet committed -- see BookOrbitMigration.cpp),
  // appendBatch()'s own "different owner -> replace" policy would treat the still-legacy-owned
  // queue as a stranger's leftover and destructively promote a fresh one over it, permanently
  // erasing whatever legacy events the migration was about to re-own. BookOrbitMigration::runOnce()
  // reruns every boot (see main.cpp), so this gate is self-healing: a transient SD hiccup only
  // delays capture/drain by one boot cycle, not indefinitely, and needs no retry bookkeeping here.
  return SETTINGS.shouldTrackReadingStats() && BOOKORBIT_STORE.hasCredentials() && !migrationPending();
}

bool captureEnabled() { return drainPossible() && cachedCapabilities().isBookOrbit; }

BookOrbitSyncClient::Error ensureCapabilitiesConfirmed(BookOrbitSyncClient::Session& session,
                                                       BookOrbitSyncClient::Capabilities& outCaps,
                                                       const BookOrbitSyncClient::AbortCallback& shouldAbort) {
  outCaps = cachedCapabilities();
  if (outCaps.isBookOrbit) return BookOrbitSyncClient::OK;

  const BookOrbitSyncClient::Error probe = session.probeCapabilities(outCaps, shouldAbort);
  if (probe != BookOrbitSyncClient::OK) {
    // Unanswered (network/auth/heap), unsupported (endpoint absent), or not BookOrbit-shaped.
    // None of these settle the question: an explicitly-configured server that fails to answer
    // correctly is a configuration problem for the user to fix (BookOrbitAuthActivity's
    // test-connection surfaces it), not a fact to cache and act on here.
    LOG_DBG("BOStats", "BookOrbit capability probe inconclusive: %s", BookOrbitSyncClient::errorString(probe));
    return probe;
  }
  recordCapabilities(outCaps);
  LOG_INF("BOStats", "BookOrbit capabilities confirmed (bookmarkSync=%d)", outCaps.hasBookmarkSync ? 1 : 0);
  return BookOrbitSyncClient::OK;
}

DrainResult uploadQueued(const std::string& documentHash, const BookOrbitSyncClient::AbortCallback& shouldAbort) {
  BookOrbitSyncClient::Session session;
  return uploadQueued(session, documentHash, shouldAbort);
}

DrainResult uploadQueued(BookOrbitSyncClient::Session& session, const std::string& documentHash,
                         const BookOrbitSyncClient::AbortCallback& shouldAbort) {
  return drainOne(session, documentHash, shouldAbort);
}

bool isQueueFileName(const char* name, std::string& outHash) {
  // Exactly the shape filePath() produces for a real stats id: 32 lowercase hex + ".bin". Temp
  // and backup sidecars (".bin.tmp"/".bin.bak") and sanitized non-hex keys are not candidates.
  if (name == nullptr) return false;
  constexpr size_t HASH_CHARS = 32;
  constexpr char SUFFIX[] = ".bin";
  if (strlen(name) != HASH_CHARS + sizeof(SUFFIX) - 1 || strcmp(name + HASH_CHARS, SUFFIX) != 0) return false;
  for (size_t i = 0; i < HASH_CHARS; i++) {
    const char c = name[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  outHash.assign(name, HASH_CHARS);
  return true;
}

namespace {
// Owner-scoped round-robin cursor for drainWindow(): the last OTHER-book id examined. Advisory --
// a missing, torn or foreign-owner file just restarts the rotation from the smallest id. Its
// name never matches isQueueFileName() or BookOrbitMigration's .bin/.tmp/.bak scan.
constexpr char CURSOR_FILE[] = "/drain_cursor.dat";
constexpr uint8_t CURSOR_MAGIC[3] = {'B', 'O', 'C'};
constexpr uint8_t CURSOR_VERSION = 1;
constexpr size_t CURSOR_ID_CHARS = 32;
constexpr size_t CURSOR_BYTES = 3 + 1 + BookOrbitOwnerId::BYTES + CURSOR_ID_CHARS + 1;

uint8_t cursorChecksum(const uint8_t* data, const size_t len) {
  uint8_t sum = 0x5A;
  for (size_t i = 0; i < len; i++) sum = static_cast<uint8_t>((sum << 1 | sum >> 7) ^ data[i]);
  return sum;
}

bool readCursor(const BookOrbitOwnerId& owner, char (&out)[CURSOR_ID_CHARS + 1]) {
  out[0] = '\0';
  const std::string path = BookOrbitStatsQueue::storageDirectory() + CURSOR_FILE;
  if (!Storage.exists(path.c_str())) return false;
  uint8_t rec[CURSOR_BYTES];
  HalFile f;
  if (!Storage.openFileForRead("BOStats", path, f)) return false;
  if (f.read(rec, sizeof(rec)) != static_cast<int>(sizeof(rec))) return false;
  if (memcmp(rec, CURSOR_MAGIC, 3) != 0 || rec[3] != CURSOR_VERSION) return false;
  if (cursorChecksum(rec, CURSOR_BYTES - 1) != rec[CURSOR_BYTES - 1]) return false;
  if (memcmp(rec + 4, owner.bytes, BookOrbitOwnerId::BYTES) != 0) return false;
  memcpy(out, rec + 4 + BookOrbitOwnerId::BYTES, CURSOR_ID_CHARS);
  out[CURSOR_ID_CHARS] = '\0';
  std::string ignored;
  char name[CURSOR_ID_CHARS + 5];
  snprintf(name, sizeof(name), "%s.bin", out);
  if (!isQueueFileName(name, ignored)) {
    out[0] = '\0';
    return false;
  }
  return true;
}

void writeCursor(const BookOrbitOwnerId& owner, const char* id) {
  uint8_t rec[CURSOR_BYTES];
  memcpy(rec, CURSOR_MAGIC, 3);
  rec[3] = CURSOR_VERSION;
  memcpy(rec + 4, owner.bytes, BookOrbitOwnerId::BYTES);
  memcpy(rec + 4 + BookOrbitOwnerId::BYTES, id, CURSOR_ID_CHARS);
  rec[CURSOR_BYTES - 1] = cursorChecksum(rec, CURSOR_BYTES - 1);
  const std::string path = BookOrbitStatsQueue::storageDirectory() + CURSOR_FILE;
  if (fileContentEquals("BOStats", path.c_str(), rec, sizeof(rec))) return;  // value-change guard
  const std::string tmp = path + ".tmp";
  {
    HalFile f;
    if (!Storage.openFileForWrite("BOStats", tmp, f)) return;
    if (f.write(rec, sizeof(rec)) != sizeof(rec)) {
      LOG_ERR("BOStats", "Short write on drain cursor");
      return;
    }
    f.flush();
  }
  // Advisory state: the brief no-cursor window after remove() only restarts the rotation.
  Storage.remove(path.c_str());
  if (!Storage.rename(tmp.c_str(), path.c_str())) LOG_ERR("BOStats", "Could not store drain cursor");
}
}  // namespace

WindowResult drainWindow(BookOrbitSyncClient::Session& session, const std::string& currentHash,
                         const WindowLimits& limits, const BookOrbitSyncClient::AbortCallback& shouldAbort) {
  WindowResult out;
  if (!drainPossible()) return out;
  const BookOrbitOwnerId owner = currentOwner();
  if (owner.isZero()) return out;

  // ONE deadline for the whole stats phase -- current book, directory scan, other books -- and it
  // is the abort every request receives, so SecureHttpClient::sendRequest's own shouldAbort polling
  // cuts an in-flight request off at the deadline, not merely between requests.
  const uint32_t startMs = static_cast<uint32_t>(millis());
  const uint32_t budgetMs = limits.budgetMs;
  const BookOrbitSyncClient::AbortCallback bounded = [&shouldAbort, startMs, budgetMs]() {
    if (static_cast<uint32_t>(millis()) - startMs >= budgetMs) return true;  // unsigned: wrap-safe
    return shouldAbort ? shouldAbort() : false;
  };

  if (!currentHash.empty()) {
    out.current = drainOne(session, currentHash, bounded);
    // Gating/transport/auth/endpoint failure on the current book would only repeat on the next.
    if (out.current == DrainResult::Stopped) return out;
  }
  if (limits.maxOtherBooks == 0 || bounded()) return out;

  char cursor[CURSOR_ID_CHARS + 1];
  readCursor(owner, cursor);
  // Heap, not stack: 2 x CAPACITY x 33 bytes would exceed this codebase's 256-byte local limit.
  auto selector = makeUniqueNoThrow<StatsDrainSelector<OTHER_QUEUE_CANDIDATES>>(cursor[0] ? cursor : nullptr);
  if (!selector) {
    LOG_ERR("BOStats", "OOM: drain selector");
    return out;
  }
  {
    // Names only; no queue file is opened while the directory is being iterated.
    auto dir = Storage.open(BookOrbitStatsQueue::storageDirectory().c_str());
    if (!dir || !dir.isDirectory()) return out;
    dir.rewindDirectory();
    char name[48];
    size_t scanned = 0;
    for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
      if (++scanned > MAX_OTHER_QUEUE_SCAN_ENTRIES || bounded()) break;
      if (file.isDirectory()) continue;
      if (file.getName(name, sizeof(name)) == 0) continue;
      std::string hash;
      if (!isQueueFileName(name, hash)) continue;
      selector->offer(hash.c_str(), currentHash.c_str());
    }
    out.scannedEntries = scanned;
  }

  const char* lastExamined = nullptr;
  for (size_t i = 0; i < selector->count(); i++) {
    if (out.otherAttempted >= limits.maxOtherBooks || bounded()) break;
    const char* id = selector->at(i);
    // A foreign-owner, stale-format or empty queue costs one header read and no request, and the
    // cursor still moves past it, so it can never pin the rotation.
    if (BookOrbitStatsQueue::queuedCount(id, owner) == 0) {
      lastExamined = id;
      continue;
    }
    out.otherAttempted++;
    const DrainResult r = drainOne(session, id, bounded);
    if (r == DrainResult::Stopped) {
      out.stopped = true;
      break;  // not advanced past: this book is first in line next window
    }
    lastExamined = id;
  }
  if (lastExamined != nullptr) writeCursor(owner, lastExamined);
  return out;
}

namespace {
DrainResult drainOne(BookOrbitSyncClient::Session& session, const std::string& documentHash,
                     const BookOrbitSyncClient::AbortCallback& shouldAbort) {
  if (!drainPossible() || documentHash.empty()) return DrainResult::Stopped;

  BookOrbitSyncClient::Capabilities caps;
  if (ensureCapabilitiesConfirmed(session, caps, shouldAbort) != BookOrbitSyncClient::OK) {
    return DrainResult::Stopped;
  }

  // Staleness reference. A missing/implausible RTC just disables the age check (0) rather than
  // blocking the upload: the events already carry the absolute stamps they were captured with.
  uint32_t nowEpochSeconds = 0;
  uint32_t nowEpochMinutes = 0;
  if (halClock.getEpochMinutes(nowEpochMinutes) && BookOrbitSessionClock::isPlausible(nowEpochMinutes)) {
    nowEpochSeconds = nowEpochMinutes * 60u;
  }

  // Every queue read and write is scoped to the identity that captured it, so a reconfigured
  // server/account never uploads the previous one's reading history (the queue discards it).
  const BookOrbitOwnerId owner = currentOwner();
  // Shrunk to a single event after the server rejects a batch as too large, so the retry has a
  // chance of fitting -- see the BAD_REQUEST branch below. It stays shrunk for the rest of this
  // transaction: going straight back to full batches would just repeat the rejection.
  size_t batchLimit = BookOrbitStatsQueue::MAX_EVENTS_PER_BATCH;
  BookOrbitStatEvent batch[BookOrbitStatsQueue::MAX_EVENTS_PER_BATCH];
  for (int attempt = 0; attempt < MAX_BATCHES_PER_TRANSACTION; attempt++) {
    // `scanned` is how many file records the batch covers, which is what a later removal must
    // use: it includes any corrupt record that was skipped rather than returned.
    size_t scanned = 0;
    const size_t read = BookOrbitStatsQueue::readOldest(documentHash, owner, batch, batchLimit, scanned);
    if (scanned == 0) return DrainResult::Drained;  // queue empty or unreadable
    if (read == 0) {
      // Every record in this window was corrupt. Retire the window so the queue makes progress.
      LOG_ERR("BOStats", "Discarding %u undecodable queued record(s)", static_cast<unsigned>(scanned));
      if (!BookOrbitStatsQueue::removeOldest(documentHash, scanned)) return DrainResult::Stopped;
      continue;
    }

    // Drop stale events without spending a request on them. readOldest() already rejected
    // corrupt records, so anything filtered out here is simply too old to be meaningful.
    size_t sendCount = 0;
    for (size_t i = 0; i < read; i++) {
      if (BookOrbitStatsQueue::isUploadable(batch[i], nowEpochSeconds)) batch[sendCount++] = batch[i];
    }
    if (sendCount == 0) {
      LOG_INF("BOStats", "Dropping %u stale/unusable queued event(s)", static_cast<unsigned>(read));
      if (!BookOrbitStatsQueue::removeOldest(documentHash, scanned)) return DrainResult::Stopped;
      continue;
    }

    BookOrbitStatsAck ack;
    const BookOrbitSyncClient::Error result = session.uploadPageStats(
        documentHash, statsDeviceId(), SETTINGS.getEffectiveDeviceName(), batch, sendCount, ack, shouldAbort);

    if (result == BookOrbitSyncClient::OK && ack.matched) {
      LOG_INF("BOStats", "Uploaded %u event(s): accepted=%u duplicates=%u", static_cast<unsigned>(sendCount),
              static_cast<unsigned>(ack.accepted), static_cast<unsigned>(ack.duplicates));
      if (!BookOrbitStatsQueue::removeOldest(documentHash, scanned)) return DrainResult::Stopped;
      continue;
    }

    if (result == BookOrbitSyncClient::OK && !ack.matched && !ack.unmatched) {
      LOG_INF("BOStats", "Upload unconfirmed by the server; retaining %u queued event(s)",
              static_cast<unsigned>(BookOrbitStatsQueue::queuedCount(documentHash, owner)));
      return DrainResult::Retained;
    }

    if (result == BookOrbitSyncClient::OK && ack.unmatched) {
      LOG_INF("BOStats", "Server has no book for this hash; retaining %u queued event(s)",
              static_cast<unsigned>(BookOrbitStatsQueue::queuedCount(documentHash, owner)));
      return DrainResult::Retained;
    }

    if (result == BookOrbitSyncClient::UNSUPPORTED || result == BookOrbitSyncClient::NOT_BOOKORBIT) {
      // Unlike e51b383, this is no longer a settled negative: the server was explicitly
      // configured as BookOrbit, so its page-stats endpoint being absent (an older BookOrbit
      // version) or wrong-shaped (a proxy hiccup) is a transient or configuration problem, not a
      // permanent fact. Keep the queue -- the cap and staleness bound already keep that bounded
      // -- and leave the capability cache exactly as it was (recordCapabilities() above already
      // ran before this point only for the success case).
      LOG_ERR("BOStats",
              "Page-stats endpoint did not answer as BookOrbit (%s); retaining the queue for a later attempt",
              BookOrbitSyncClient::errorString(result));
      return DrainResult::Stopped;
    }

    if (result == BookOrbitSyncClient::BAD_REQUEST) {
      const int httpCode = session.lastHttpCode();
      if (httpCode == 413 && sendCount > 1) {
        LOG_ERR("BOStats", "Server rejected a batch of %u as too large; retrying one event at a time",
                static_cast<unsigned>(sendCount));
        batchLimit = 1;
        continue;
      }
      LOG_ERR("BOStats", "Server rejected the request (http=%d); retaining %u event(s) for a later attempt", httpCode,
              static_cast<unsigned>(sendCount));
      return DrainResult::Retained;
    }

    // Retryable: auth, network, server error, low heap, or an aborted transfer. Keep everything.
    LOG_DBG("BOStats", "Stats upload deferred: %s (http=%d)", BookOrbitSyncClient::errorString(result),
            session.lastHttpCode());
    return DrainResult::Stopped;
  }
  // Batch budget spent with events still queued; the rest waits for the next window.
  return DrainResult::Retained;
}
}  // namespace

}  // namespace BookOrbitStats
