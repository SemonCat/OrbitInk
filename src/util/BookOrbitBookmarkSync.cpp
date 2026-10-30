#include "BookOrbitBookmarkSync.h"

#include <BookOrbitCredentialStore.h>
#include <BookOrbitSessionClock.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>

#include "BookOrbitStatsSync.h"
#include "BookmarkFile.h"
#include "BookmarkUtil.h"
#include "CrossPointSettings.h"

namespace BookOrbitBookmarkSync {

void sync(BookOrbitSyncClient::Session& session, const std::string& epubPath, const std::string& documentHash,
          const AbortCallback& shouldAbort) {
  if (!BOOKORBIT_STORE.hasCredentials() || documentHash.size() != 32) return;

  BookOrbitSyncClient::Capabilities caps;
  if (BookOrbitStats::ensureCapabilitiesConfirmed(session, caps, shouldAbort) != BookOrbitSyncClient::OK) return;
  if (!caps.hasBookmarkSync) return;

  // Scopes which bookmarks buildOutgoing()/applyIncoming() treat as already belonging to THIS
  // account -- see BookmarkEntry::boOwner's own doc comment for why this must not simply be
  // boSynced/boDatetime alone (switching BookOrbit account never clears them).
  const std::string currentOwnerHex = BookOrbitStats::currentOwner().toHex();

  // See loadFailureRequiresAbort()'s own doc comment for why "load failed and data might still
  // exist" cannot be treated the same as "no file yet". Check AFTER load() has attempted its
  // interrupted-save recovery, and require both live and .bak to be PROVEN absent: SdFat's
  // boolean exists() also returns false on I/O failure, which is not an authoritative empty set.
  std::vector<BookmarkEntry> bookmarks;
  const std::string bookmarkPath = BookmarkUtil::getBookmarkPath(epubPath);
  const bool loadOk = BookmarkFile::load(epubPath, bookmarks);
  bool bookmarkAbsenceConfirmed = false;
  if (!loadOk) {
    bookmarkAbsenceConfirmed = Storage.probePath(bookmarkPath.c_str()) == HalStorage::PathStatus::Absent &&
                               Storage.probePath((bookmarkPath + ".bak").c_str()) == HalStorage::PathStatus::Absent;
  }
  if (loadFailureRequiresAbort(loadOk, bookmarkAbsenceConfirmed)) {
    LOG_ERR("BOBookmark",
            "Bookmark data was not confirmed absent after a failed load; aborting sync rather than risking "
            "an overwrite or a false delete acknowledgment");
    return;
  }

  bool clockAvailable = false;
  uint32_t nowEpochSeconds = 0;
  uint32_t epochMinutes = 0;
  if (halClock.getEpochMinutes(epochMinutes) && BookOrbitSessionClock::isPlausible(epochMinutes)) {
    clockAvailable = true;
    nowEpochSeconds = epochMinutes * 60u;
  }

  bool anyUnminted = false;
  // Gated on loadOk defensively: BookmarkFile::load() clears `bookmarks` before it can fail, so
  // a failed load already leaves nothing here to mint -- this guard costs nothing and removes
  // any reliance on that ordering surviving a future change to load()'s own implementation.
  if (loadOk && mintMissingDatetimes(bookmarks, clockAvailable, nowEpochSeconds, anyUnminted)) {
    // Persist BEFORE building the request: a crash here still leaves the minted identity durable
    // for the next attempt to resend unchanged. If the write itself fails, abandon this round
    // rather than send a request built from identities that might not survive a crash/retry.
    if (!BookmarkFile::save(epubPath, bookmarks)) return;
  }

  std::vector<BookOrbitBookmarkKeyRow> keys;
  bool keysComplete = false;
  std::vector<BookOrbitBookmark> changes;
  std::vector<size_t> changeIndices;
  buildOutgoing(bookmarks, anyUnminted || !loadOk, BOOKORBIT_BOOKMARK_MAX_KEYS, BOOKORBIT_BOOKMARK_BATCH,
                currentOwnerHex, keys, keysComplete, changes, changeIndices);

  // A persisted per-installation unique id, NOT BookOrbitLimits::DEVICE_ID's shared literal: the
  // server's bookmark-exchange service partitions its per-device delivery/tombstone state by
  // this value (see BookOrbitSyncClient::Session::exchangeBookmarks()'s own doc comment), so two
  // physical devices sharing an id here would silently drop each other's still-undelivered
  // changes.
  const std::string deviceId = BookOrbitStats::statsDeviceId();
  const std::string deviceModel = SETTINGS.getEffectiveDeviceName();
  bool outUnmatched = false;
  std::vector<BookOrbitIncomingBookmark> incoming;
  bool outMorePending = false;
  const BookOrbitSyncClient::Error result =
      session.exchangeBookmarks(documentHash, deviceId, deviceModel, keys.data(), keys.size(), keysComplete,
                                changes.data(), changes.size(), outUnmatched, incoming, outMorePending, shouldAbort);
  if (result != BookOrbitSyncClient::OK) {
    LOG_DBG("BOBookmark", "Exchange deferred: %s (http=%d)", BookOrbitSyncClient::errorString(result),
            session.lastHttpCode());
    return;
  }
  if (outUnmatched) {
    LOG_INF("BOBookmark", "Server has no book for this hash; bookmarks not exchanged this round");
    return;
  }

  // Mark just-uploaded entries synced TO THIS OWNER -- their identity is now confirmed known to
  // the currently configured server/account (see BookmarkEntry::boOwner's own doc comment).
  bool changed = false;
  for (const size_t idx : changeIndices) {
    bookmarks[idx].boSynced = true;
    bookmarks[idx].boOwner = currentOwnerHex;
    changed = true;
  }

  std::vector<BookOrbitBookmarkAck> applied;
  std::vector<uint32_t> deletedIds;
  bool incomingChanged = false;
  applyIncoming(bookmarks, clockAvailable ? nowEpochSeconds : 0, currentOwnerHex, incoming, applied, deletedIds,
                incomingChanged);
  changed = changed || incomingChanged;

  // Persist BEFORE acking, unconditionally when anything changed -- see this feature's own
  // durability-model comment: the ack must never be the thing that makes a change durable. The
  // guarantee only holds if the persist is KNOWN to have succeeded, though: acking an applied add
  // whose write actually failed would tell the server to retire a pending change this device
  // never durably has, permanently losing it (unlike an outgoing boSynced write, an incoming ack
  // destroys the server's own copy, so there is no idempotent recovery once it is sent). Abandon
  // the round instead of acking anything when the write fails -- the server simply re-offers
  // every still-pending change next sync.
  const bool persisted = !changed || BookmarkFile::save(epubPath, bookmarks);
  if (changed && !persisted) return;

  if (!applied.empty() || !deletedIds.empty()) {
    session.ackBookmarks(documentHash, deviceId, deviceModel, applied, deletedIds, shouldAbort);
    // Soft-fail on the ACK ITSELF (not the persist above, already verified): an unacknowledged
    // change is simply re-offered and re-applied (a no-op) next sync -- see applyIncoming()'s own
    // doc comment.
  }

  if (outMorePending) {
    LOG_DBG("BOBookmark", "More server-side bookmark changes remain; they will arrive next sync");
  }
}

}  // namespace BookOrbitBookmarkSync
