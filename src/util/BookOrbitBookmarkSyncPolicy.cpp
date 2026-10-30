#include "BookOrbitBookmarkSyncPolicy.h"

#include <cstdio>
#include <cstring>

#include "BookmarkUtil.h"

namespace BookOrbitBookmarkSync {

namespace {

bool computeLocalKey(const BookmarkEntry& b, char (&outKey)[BookOrbitBookmarkKeyRow::DIGEST_SIZE]) {
  if (b.boDatetime.empty() || b.xpath.empty()) return false;
  return bookOrbitBookmarkKey(b.boDatetime.c_str(), b.xpath.c_str(), outKey);
}

}  // namespace

bool mintMissingDatetimes(std::vector<BookmarkEntry>& bookmarks, const bool clockAvailable,
                          const uint32_t nowEpochSeconds, bool& outAnyUnminted) {
  outAnyUnminted = false;
  bool mintedAny = false;
  char buf[20];
  for (auto& b : bookmarks) {
    if (!b.boDatetime.empty()) continue;
    if (!clockAvailable || !bookOrbitFormatBookmarkDatetime(nowEpochSeconds, buf)) {
      outAnyUnminted = true;
      continue;
    }
    b.boDatetime = buf;
    // boSynced stays false: a freshly minted identity has never been offered to the server yet.
    mintedAny = true;
  }
  return mintedAny;
}

void buildOutgoing(const std::vector<BookmarkEntry>& bookmarks, const bool anyUnminted, const size_t maxKeys,
                   const size_t maxChanges, const std::string& currentOwnerHex,
                   std::vector<BookOrbitBookmarkKeyRow>& outKeys, bool& outKeysComplete,
                   std::vector<BookOrbitBookmark>& outChanges, std::vector<size_t>& outChangeIndices) {
  outKeys.clear();
  outChanges.clear();
  outChangeIndices.clear();

  bool complete = !anyUnminted;
  for (const auto& b : bookmarks) {
    char k[BookOrbitBookmarkKeyRow::DIGEST_SIZE];
    if (!computeLocalKey(b, k)) {
      // A boDatetime with no usable key (e.g. an empty xpath) is a data problem, not "not yet
      // minted" -- either way the set is not complete this round.
      if (!b.boDatetime.empty()) complete = false;
      continue;
    }
    if (outKeys.size() >= maxKeys) {
      complete = false;
      break;
    }
    BookOrbitBookmarkKeyRow row;
    memcpy(row.k, k, sizeof(row.k));
    snprintf(row.dt, sizeof(row.dt), "%s", b.boDatetime.c_str());
    outKeys.push_back(row);
  }
  outKeysComplete = complete;

  for (size_t i = 0; i < bookmarks.size() && outChanges.size() < maxChanges; i++) {
    const BookmarkEntry& b = bookmarks[i];
    const bool syncedToCurrentOwner = b.boSynced && b.boOwner == currentOwnerHex;
    if (b.boDatetime.empty() || syncedToCurrentOwner || b.xpath.empty()) continue;
    BookOrbitBookmark out;
    snprintf(out.datetime, sizeof(out.datetime), "%s", b.boDatetime.c_str());
    out.pos = b.xpath;
    out.pageno = b.computedChapterPageCount > 0 ? static_cast<uint16_t>(b.computedChapterProgress + 1) : 0;
    outChanges.push_back(out);
    outChangeIndices.push_back(i);
  }
}

namespace {
// A candidate is fair game for the CURRENT owner to match against (by key or by position) only
// when it is either already that owner's (the ordinary case) or has no owner recorded at all (a
// bookmark that predates this field, or one this device created before ever syncing it -- freely
// adoptable by whichever account syncs it first). An entry recorded under a DIFFERENT owner is
// invisible to this pass entirely, so a coincidental key or xpath collision with a
// previously-configured account's bookmark can never re-own or corrupt it.
bool ownedByOrAdoptableBy(const BookmarkEntry& b, const std::string& currentOwnerHex) {
  return b.boOwner.empty() || b.boOwner == currentOwnerHex;
}
}  // namespace

void applyIncoming(std::vector<BookmarkEntry>& bookmarks, const uint32_t nowEpochSecondsOrZero,
                   const std::string& currentOwnerHex, const std::vector<BookOrbitIncomingBookmark>& incoming,
                   std::vector<BookOrbitBookmarkAck>& outApplied, std::vector<uint32_t>& outDeletedIds,
                   bool& outChanged) {
  outApplied.clear();
  outDeletedIds.clear();
  outChanged = false;

  for (const BookOrbitIncomingBookmark& in : incoming) {
    if (in.deleted) {
      for (size_t i = 0; i < bookmarks.size();) {
        char k[BookOrbitBookmarkKeyRow::DIGEST_SIZE];
        if (computeLocalKey(bookmarks[i], k) && in.key == k && ownedByOrAdoptableBy(bookmarks[i], currentOwnerHex)) {
          bookmarks.erase(bookmarks.begin() + static_cast<std::ptrdiff_t>(i));
          outChanged = true;
          // Do not advance i: the erase shifted a new element into this slot.
        } else {
          i++;
        }
      }
      // Ack regardless of whether a local match was found: an already-applied deletion from an
      // earlier, un-acked round must still be re-acked -- this is exactly what stops a lost ack
      // from resurrecting it (the deletion already happened and was already persisted; only the
      // ack, which cannot undo anything, was missing).
      outDeletedIds.push_back(in.serverId);
      continue;
    }

    if (in.pos.empty()) {
      // Can never be placed -- a permanent failure, not a "try again later" case.
      BookOrbitBookmarkAck ack;
      ack.serverId = in.serverId;
      ack.failed = true;
      outApplied.push_back(ack);
      continue;
    }

    // Already applied in an earlier, un-acked round? Prefer matching by identity key when the
    // server sent one. It usually will not for an add: per BookOrbitBookmarks.h's own class
    // comment, the server has no key for a bookmark IT created until this device's ack supplies
    // one -- so a lost ack causes the server to re-offer the exact same add with `key` still
    // empty, not a key that would fail to match. Falling back to matching by position (`pos`)
    // in that case is what makes THIS retry recognize the entry it already created last round,
    // instead of minting a fresh identity and duplicating it indefinitely on every lost ack.
    int existingIndex = -1;
    if (!in.key.empty()) {
      for (size_t i = 0; i < bookmarks.size(); i++) {
        char k[BookOrbitBookmarkKeyRow::DIGEST_SIZE];
        if (computeLocalKey(bookmarks[i], k) && in.key == k && ownedByOrAdoptableBy(bookmarks[i], currentOwnerHex)) {
          existingIndex = static_cast<int>(i);
          break;
        }
      }
    }
    if (existingIndex < 0) {
      for (size_t i = 0; i < bookmarks.size(); i++) {
        if (bookmarks[i].xpath == in.pos && ownedByOrAdoptableBy(bookmarks[i], currentOwnerHex)) {
          existingIndex = static_cast<int>(i);
          break;
        }
      }
    }

    if (existingIndex < 0) {
      if (nowEpochSecondsOrZero == 0) {
        // No usable clock this round: cannot mint an identity for a genuinely new entry. Leave it
        // out of the acknowledgment entirely so the server re-offers it once a clock is available.
        continue;
      }
      char datetime[20];
      if (!bookOrbitFormatBookmarkDatetime(nowEpochSecondsOrZero, datetime)) continue;  // same as above

      BookmarkEntry entry;
      entry.xpath = in.pos;
      entry.summary = BookmarkUtil::sanitizeBookmarkSummary(in.title);
      entry.percentage = 0.0f;  // unknown without a loaded Epub -- see BookOrbitBookmarkSync.h
      entry.boDatetime = datetime;
      entry.boSynced = true;  // this identity originated from, and is already known to, the server
      entry.boOwner = currentOwnerHex;
      bookmarks.push_back(entry);
      existingIndex = static_cast<int>(bookmarks.size() - 1);
      outChanged = true;
    }

    char k[BookOrbitBookmarkKeyRow::DIGEST_SIZE];
    BookmarkEntry& local = bookmarks[static_cast<size_t>(existingIndex)];
    if (!computeLocalKey(local, k)) continue;  // should be unreachable: just-built/found entries always key
    if (local.boOwner != currentOwnerHex) {
      // Adopts a pre-owner-field entry (boOwner was empty) into the account that just confirmed
      // it; a no-op when it already matched, since ownedByOrAdoptableBy() only let a match through
      // when boOwner was empty or already currentOwnerHex.
      local.boOwner = currentOwnerHex;
      outChanged = true;
    }

    BookOrbitBookmarkAck ack;
    ack.serverId = in.serverId;
    ack.failed = false;
    ack.key = k;
    snprintf(ack.datetime, sizeof(ack.datetime), "%s", local.boDatetime.c_str());
    ack.pos = local.xpath;
    outApplied.push_back(ack);
  }
}

}  // namespace BookOrbitBookmarkSync
