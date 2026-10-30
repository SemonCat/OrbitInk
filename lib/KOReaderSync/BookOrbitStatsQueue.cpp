#include "BookOrbitStatsQueue.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

namespace {
// Stats live OUTSIDE the per-book cache directory, in a directory this feature owns, and are
// keyed by the KOSync DOCUMENT HASH rather than by anything derived from the file's path.
//
// Two separate problems drive that. The cache directory is rebuildable scratch that "Clear
// Cache" and every cache rebuild delete wholesale, so events waiting there could be destroyed by
// a routine space reclaim. And the cache directory's own name is a hash of the book's full path,
// so moving or renaming the book produced a new name and stranded whatever had not been
// uploaded -- an unremarkable bit of library tidying, silently losing telemetry.
//
// The document hash fixes both, and is the right key for a further reason: it is the exact
// identity these events will be uploaded under. The queue and the payload cannot disagree about
// which book they concern, and the queue survives precisely the changes the server's own view of
// the book survives -- a move under filename matching, and a move or rename under binary
// matching. (Changing the match method changes the document identity itself, so it starts a
// fresh queue, exactly as it does for progress.)
constexpr char STATS_DIR[] = "/.crosspoint/bo_stats";

// Document hashes are 32 hex characters, so they need no escaping; anything unexpected is
// reduced to a safe, still-deterministic file name rather than reaching the filesystem raw.
std::string sanitizedKey(const std::string& documentHash) {
  if (documentHash.empty()) return "unknown";
  std::string key;
  key.reserve(documentHash.size());
  for (const char c : documentHash) {
    const bool safe =
        (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    key.push_back(safe ? c : '_');
  }
  return key;
}

// Header: 'B','O','Q', version. A mismatched header discards the file rather than being
// reinterpreted -- a bump costs at most the events still waiting to upload.
constexpr uint8_t MAGIC_0 = 'B';
constexpr uint8_t MAGIC_1 = 'O';
constexpr uint8_t MAGIC_2 = 'Q';

// Record layout (12 bytes, explicit little-endian -- never a raw struct):
//   [0-3]   startTime        uint32 LE
//   [4-5]   durationSeconds  uint16 LE
//   [6-7]   page             uint16 LE
//   [8-9]   totalPages       uint16 LE
//   [10-11] checksum         uint16 LE, additive over bytes [0-9] plus a non-zero seed
// The checksum is what makes a torn or garbled record detectable: a record half-written when
// power dropped decodes to nonsense, and nonsense that happened to satisfy every field bound
// would otherwise be uploaded as real reading history.
constexpr uint16_t CHECKSUM_SEED = 0xB0Bu;

uint16_t readLe16(const uint8_t* data, const size_t offset) {
  return static_cast<uint16_t>(static_cast<uint16_t>(data[offset]) |
                               static_cast<uint16_t>(static_cast<uint16_t>(data[offset + 1]) << 8));
}

uint32_t readLe32(const uint8_t* data, const size_t offset) {
  return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
         (static_cast<uint32_t>(data[offset + 2]) << 16) | (static_cast<uint32_t>(data[offset + 3]) << 24);
}

void writeLe16(uint8_t* data, const size_t offset, const uint16_t value) {
  data[offset] = static_cast<uint8_t>(value & 0xFF);
  data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

void writeLe32(uint8_t* data, const size_t offset, const uint32_t value) {
  data[offset] = static_cast<uint8_t>(value & 0xFF);
  data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  data[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
  data[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

uint16_t checksum(const uint8_t* data) {
  uint16_t sum = CHECKSUM_SEED;
  for (size_t i = 0; i < BookOrbitStatsQueue::RECORD_BYTES - 2; i++) {
    sum = static_cast<uint16_t>(sum + static_cast<uint16_t>(data[i]) * static_cast<uint16_t>(i + 1));
  }
  return sum;
}

// Number of whole records the file holds, ignoring any trailing partial record left by a torn
// write. A file shorter than the header holds none.
size_t recordCountForSize(const size_t fileBytes) {
  if (fileBytes < BookOrbitStatsQueue::HEADER_BYTES) return 0;
  return (fileBytes - BookOrbitStatsQueue::HEADER_BYTES) / BookOrbitStatsQueue::RECORD_BYTES;
}

// What an attempt to open the live queue found. The distinction that matters is between a file
// this feature may REPLACE and one it merely could not read this time: treating the second as
// the first means a transient SD hiccup silently destroys a valid queue of unsent events.
enum class QueueOpen {
  Absent,      // no live file at all -- a fresh queue may be created
  Ok,          // header read and valid; outOwner/outRecordCount are populated
  Foreign,     // the bytes were read successfully and are not ours (bad magic, or an older
               // format version) -- an intentional, safe policy replacement
  Unreadable,  // the file exists but could not be opened or its header not fully read; nothing
               // is known about its contents, so every caller must fail closed
};

// Opens the queue for reading and validates its header, leaving the cursor at the first record.
QueueOpen openValidatedForRead(const std::string& path, HalFile& file, size_t& outRecordCount,
                               BookOrbitOwnerId& outOwner) {
  outRecordCount = 0;
  outOwner = BookOrbitOwnerId{};
  if (!Storage.exists(path.c_str())) return QueueOpen::Absent;
  if (!Storage.openFileForRead("BOQ", path, file)) return QueueOpen::Unreadable;

  // A file genuinely shorter than a header is a torn creation -- its content is known, and it is
  // known to be useless, so it is replaceable. A SHORT READ of a file that is long enough is an
  // I/O failure instead, and says nothing about what the file holds.
  const size_t fileBytes = file.fileSize();
  uint8_t header[BookOrbitStatsQueue::HEADER_BYTES] = {};
  const int headerRead = file.read(header, sizeof(header));
  if (headerRead != static_cast<int>(sizeof(header))) {
    return fileBytes < BookOrbitStatsQueue::HEADER_BYTES ? QueueOpen::Foreign : QueueOpen::Unreadable;
  }
  if (!BookOrbitStatsQueue::headerValid(header, sizeof(header))) return QueueOpen::Foreign;

  outOwner = BookOrbitStatsQueue::headerOwner(header);
  outRecordCount = recordCountForSize(fileBytes);
  return QueueOpen::Ok;
}

// Promote a fully written <path>.tmp into place, retiring the current live file through
// <path>.bak rather than deleting it first.
//
// The ordering is the whole point. Deleting live and then renaming tmp leaves a window in which
// the only complete copy of the queue is a file that reads ignore: a rename failure, or a power
// cut of a few milliseconds, loses every un-uploaded event. Here the live file is renamed ASIDE,
// so at every instant at least one of live/bak is a complete queue, and a failed promotion is
// undone by putting the backup back.
bool promoteReplacement(const std::string& documentHash) {
  const std::string path = BookOrbitStatsQueue::filePath(documentHash);
  const std::string tmpPath = BookOrbitStatsQueue::tempPath(documentHash);
  const std::string bakPath = BookOrbitStatsQueue::backupPath(documentHash);

  if (!Storage.exists(tmpPath.c_str())) {
    LOG_ERR("BOQ", "Replacement temp file vanished before promotion");
    return false;
  }

  // A backup that already exists here is NOT ours to clear. Every entry point runs recovery
  // first, so a surviving .bak at this point means recovery could not resolve it -- and if the
  // live file is also missing, that backup is the only copy of the queue in existence. Deleting
  // it to make room for a new one would destroy exactly the data the backup was protecting.
  // Refuse instead, and leave it for recovery to restore on a later attempt.
  if (Storage.exists(bakPath.c_str())) {
    LOG_ERR("BOQ", "An unresolved stats queue backup is present; refusing to replace over it");
    Storage.remove(tmpPath.c_str());
    return false;
  }

  if (Storage.exists(path.c_str()) && !Storage.rename(path.c_str(), bakPath.c_str())) {
    // Could not stage the old queue aside. Abandon the replacement rather than force it: the
    // live file is still intact and still the one reads will use.
    LOG_ERR("BOQ", "Could not stage the live stats queue aside; keeping it");
    Storage.remove(tmpPath.c_str());
    return false;
  }

  if (!Storage.rename(tmpPath.c_str(), path.c_str())) {
    LOG_ERR("BOQ", "Could not promote replacement stats queue; restoring backup");
    if (Storage.exists(bakPath.c_str()) && !Storage.rename(bakPath.c_str(), path.c_str())) {
      // Both renames failed. The data still exists as .bak and recoverInterruptedReplacement()
      // restores it on the next access, so nothing is lost even here.
      LOG_ERR("BOQ", "Could not restore backup; it stays as .bak for recovery on next access");
    }
    Storage.remove(tmpPath.c_str());
    return false;
  }

  Storage.remove(bakPath.c_str());
  return true;
}

// Rebuilds the queue from record `dropCount` onwards, writing exactly whole records via a temp
// file and a staged rename so a power loss mid-rewrite can never leave the live queue
// half-written or missing.
//
// Also the repair path for a TORN TAIL: because it writes `recordCount - dropCount` whole
// records and nothing else, any trailing partial bytes left by an interrupted append are gone
// from the result. That matters before appending, since appending onto a file whose length is
// not header + N*RECORD_BYTES would push every later record off the fixed 12-byte grid that
// reads walk, quietly turning the partial tail AND every event after it into checksum
// failures.
bool rewriteDroppingFirst(const std::string& documentHash, const size_t dropCount) {
  const std::string path = BookOrbitStatsQueue::filePath(documentHash);

  HalFile source;
  size_t recordCount = 0;
  // The owner is READ BACK and written out again, never re-derived from the current
  // configuration: re-owning a queue here would quietly hand one account's reading history to
  // another through the repair path this function also serves.
  BookOrbitOwnerId ownerKey;
  const QueueOpen status = openValidatedForRead(path, source, recordCount, ownerKey);
  if (status == QueueOpen::Unreadable) {
    // The file is there but this attempt could not read its header. Deleting it here -- which is
    // what a plain "not readable" check used to do -- would discard a perfectly good queue over
    // a transient SD error. Report failure and leave it exactly where it is.
    LOG_ERR("BOQ", "Could not read the stats queue header; leaving the file untouched");
    return false;
  }
  if (status != QueueOpen::Ok) {
    // Absent, or a file whose bytes were read and are demonstrably not ours (bad magic or an
    // older format). Only the latter is worth retiring so it does not linger.
    if (status == QueueOpen::Foreign) Storage.remove(path.c_str());
    return true;
  }
  if (dropCount >= recordCount) {
    source.close();
    BookOrbitStatsQueue::clear(documentHash);
    return true;
  }

  if (!source.seekSet(BookOrbitStatsQueue::HEADER_BYTES + dropCount * BookOrbitStatsQueue::RECORD_BYTES)) {
    LOG_ERR("BOQ", "Could not seek past dropped stats records");
    return false;
  }

  const std::string tmpPath = BookOrbitStatsQueue::tempPath(documentHash);
  Storage.ensureDirectoryExists(BookOrbitStatsQueue::storageDirectory().c_str());
  Storage.remove(tmpPath.c_str());
  bool ok = true;
  {
    HalFile dest;
    if (!Storage.openFileForWrite("BOQ", tmpPath, dest)) {
      LOG_ERR("BOQ", "Could not open temp stats queue");
      return false;
    }
    uint8_t header[BookOrbitStatsQueue::HEADER_BYTES];
    BookOrbitStatsQueue::encodeHeader(header, ownerKey);
    ok = dest.write(header, sizeof(header)) == sizeof(header);

    uint8_t record[BookOrbitStatsQueue::RECORD_BYTES];
    for (size_t i = dropCount; ok && i < recordCount; i++) {
      // A short READ is a failure, not an early finish. Breaking out with ok still true would
      // promote a temp holding fewer records than the live file over that live file -- turning a
      // transient read error into permanent, silent loss of every record it could not copy.
      if (source.read(record, sizeof(record)) != static_cast<int>(sizeof(record))) {
        LOG_ERR("BOQ", "Short read rewriting stats queue; abandoning the replacement");
        ok = false;
        break;
      }
      ok = dest.write(record, sizeof(record)) == sizeof(record);
    }
    dest.flush();
    // Durability, not just byte counts. flush() returns void and so proves nothing; close() is
    // what runs the underlying FatFile/ExFatFile sync() and reports whether the final sectors
    // actually reached the card. Promoting a temp whose writeback failed would replace a
    // complete live queue with an incomplete file and then delete the backup behind it.
    if (!dest.close()) {
      LOG_ERR("BOQ", "Replacement stats queue did not sync to the card");
      ok = false;
    }
  }
  source.close();

  if (!ok) {
    // The replacement was never completed, so the live queue is still the only valid copy and
    // has not been touched. Drop the half-written temp and leave everything else alone --
    // crucially WITHOUT calling promoteReplacement(), which would retire the backup.
    LOG_ERR("BOQ", "Could not write a complete replacement stats queue");
    Storage.remove(tmpPath.c_str());
    return false;
  }
  return promoteReplacement(documentHash);
}

// Same read-and-rebuild shape as rewriteDroppingFirst(dropCount=0), except the header is written
// with `newOwner` rather than the owner read back from the source -- the one seam allowed to
// re-own a queue, and only ever called by BookOrbitMigration.h's one-shot upgrade.
bool rewriteOwnerImpl(const std::string& documentHash, const BookOrbitOwnerId& oldOwner,
                      const BookOrbitOwnerId& newOwner) {
  const std::string path = BookOrbitStatsQueue::filePath(documentHash);

  HalFile source;
  size_t recordCount = 0;
  BookOrbitOwnerId currentOwner;
  const QueueOpen status = openValidatedForRead(path, source, recordCount, currentOwner);
  if (status == QueueOpen::Unreadable) {
    LOG_ERR("BOQ", "Could not read the stats queue header; leaving the file untouched for a later attempt");
    return false;
  }
  if (status != QueueOpen::Ok || currentOwner != oldOwner) {
    // Absent, foreign (nothing of ours to migrate), or already owned by something other than the
    // exact legacy owner this migration targets -- including the common case where a previous
    // migration attempt already completed. Not an error either way: nothing to do.
    return true;
  }
  if (oldOwner == newOwner) return true;  // normalized identically; no rewrite needed

  const std::string tmpPath = BookOrbitStatsQueue::tempPath(documentHash);
  Storage.ensureDirectoryExists(BookOrbitStatsQueue::storageDirectory().c_str());
  Storage.remove(tmpPath.c_str());
  bool ok = true;
  {
    HalFile dest;
    if (!Storage.openFileForWrite("BOQ", tmpPath, dest)) {
      LOG_ERR("BOQ", "Could not open temp stats queue for owner migration");
      source.close();
      return false;
    }
    uint8_t header[BookOrbitStatsQueue::HEADER_BYTES];
    BookOrbitStatsQueue::encodeHeader(header, newOwner);
    ok = dest.write(header, sizeof(header)) == sizeof(header);

    uint8_t record[BookOrbitStatsQueue::RECORD_BYTES];
    for (size_t i = 0; ok && i < recordCount; i++) {
      if (source.read(record, sizeof(record)) != static_cast<int>(sizeof(record))) {
        LOG_ERR("BOQ", "Short read migrating stats queue owner; abandoning the rewrite");
        ok = false;
        break;
      }
      ok = dest.write(record, sizeof(record)) == sizeof(record);
    }
    dest.flush();
    if (!dest.close()) {
      LOG_ERR("BOQ", "Owner-migrated stats queue did not sync to the card");
      ok = false;
    }
  }
  source.close();

  if (!ok) {
    LOG_ERR("BOQ", "Could not write a complete owner-migrated stats queue; keeping the legacy-owned queue");
    Storage.remove(tmpPath.c_str());
    return false;
  }
  return promoteReplacement(documentHash);
}
}  // namespace

std::string BookOrbitStatsQueue::filePath(const std::string& documentHash) {
  return std::string(STATS_DIR) + "/" + sanitizedKey(documentHash) + ".bin";
}

std::string BookOrbitStatsQueue::storageDirectory() { return STATS_DIR; }

std::string BookOrbitStatsQueue::tempPath(const std::string& documentHash) { return filePath(documentHash) + ".tmp"; }

std::string BookOrbitStatsQueue::backupPath(const std::string& documentHash) { return filePath(documentHash) + ".bak"; }

bool BookOrbitStatsQueue::recoverInterruptedReplacement(const std::string& documentHash) {
  const std::string path = filePath(documentHash);
  const std::string tmpPath = tempPath(documentHash);
  const std::string bakPath = backupPath(documentHash);

  const bool haveLive = Storage.exists(path.c_str());
  const bool haveBak = Storage.exists(bakPath.c_str());
  const bool haveTmp = Storage.exists(tmpPath.c_str());
  if (!haveBak && !haveTmp) return true;  // the overwhelmingly common case: nothing to do

  if (haveLive) {
    // Promotion had already completed; the backup and any temp are leftovers from the crash.
    // The live file is valid either way, so this is a resolved state even if the tidy-up below
    // fails -- a stale backup only blocks the next replacement, it cannot lose anything.
    if (haveBak) LOG_INF("BOQ", "Retiring a stats queue backup left by an interrupted replacement");
    Storage.remove(bakPath.c_str());
    Storage.remove(tmpPath.c_str());
    return true;
  }

  if (haveBak) {
    // Crashed after staging the live queue aside but before promoting the replacement. Restore
    // the backup rather than promoting the temp: the backup is the last state known to be
    // complete, and preferring it costs only a re-upload of events the server deduplicates.
    LOG_INF("BOQ", "Restoring the stats queue from a backup left by an interrupted replacement");
    if (!Storage.rename(bakPath.c_str(), path.c_str())) {
      // The backup is now the ONLY copy of this queue and it is not readable in place. Report
      // the state as unresolved so every operation above fails closed rather than proceeding to
      // build a fresh queue -- which would otherwise promote over, and destroy, this backup.
      LOG_ERR("BOQ", "Could not restore the stats queue backup; refusing further access until it is recovered");
      return false;
    }
    Storage.remove(tmpPath.c_str());
    return true;
  }

  // Only a temp survives: the crash happened while it was still being written, so it was never
  // verified complete. Discard it -- there is no live queue it could safely stand in for.
  LOG_INF("BOQ", "Discarding an unverified stats queue temp file");
  Storage.remove(tmpPath.c_str());
  return true;
}

void BookOrbitStatsQueue::encodeHeader(uint8_t* out, const BookOrbitOwnerId& owner) {
  out[0] = MAGIC_0;
  out[1] = MAGIC_1;
  out[2] = MAGIC_2;
  out[3] = FORMAT_VERSION;
  memcpy(out + 4, owner.bytes, BookOrbitOwnerId::BYTES);
}

BookOrbitOwnerId BookOrbitStatsQueue::headerOwner(const uint8_t* data) {
  BookOrbitOwnerId owner;
  memcpy(owner.bytes, data + 4, BookOrbitOwnerId::BYTES);
  return owner;
}

bool BookOrbitStatsQueue::headerValid(const uint8_t* data, const size_t len) {
  if (!data || len < HEADER_BYTES) return false;
  return data[0] == MAGIC_0 && data[1] == MAGIC_1 && data[2] == MAGIC_2 && data[3] == FORMAT_VERSION;
}

void BookOrbitStatsQueue::encodeRecord(const BookOrbitStatEvent& event, uint8_t* out) {
  writeLe32(out, 0, event.startTime);
  writeLe16(out, 4, event.durationSeconds);
  writeLe16(out, 6, event.page);
  writeLe16(out, 8, event.totalPages);
  writeLe16(out, 10, checksum(out));
}

bool BookOrbitStatsQueue::decodeRecord(const uint8_t* in, BookOrbitStatEvent& out) {
  if (!in) return false;
  if (readLe16(in, 10) != checksum(in)) return false;

  BookOrbitStatEvent decoded;
  decoded.startTime = readLe32(in, 0);
  decoded.durationSeconds = readLe16(in, 4);
  decoded.page = readLe16(in, 6);
  decoded.totalPages = readLe16(in, 8);

  // A checksum-clean record can still be nonsense if it was written by a future/buggy build;
  // enforce the server's own field contract here rather than letting the server 400 on it.
  // durationSeconds needs no upper check: uint16_t cannot exceed the server's limit (see the
  // static_assert in makeEvent()).
  if (decoded.startTime < 1) return false;
  if (decoded.totalPages < 1) return false;
  if (decoded.page > decoded.totalPages) return false;

  out = decoded;
  return true;
}

size_t BookOrbitStatsQueue::overflowDropCount(const size_t existingCount, const size_t addCount) {
  const size_t total = existingCount + addCount;
  if (total <= MAX_QUEUED_EVENTS) return 0;
  // Never drop more than what is already queued: a batch larger than the whole cap is the
  // caller's problem to trim, not something to satisfy by dropping negative history.
  return std::min(total - MAX_QUEUED_EVENTS, existingCount);
}

bool BookOrbitStatsQueue::isUploadable(const BookOrbitStatEvent& event, const uint32_t nowEpochSeconds) {
  if (event.startTime < 1) return false;
  if (event.totalPages < 1 || event.page > event.totalPages) return false;
  if (nowEpochSeconds != 0 && event.startTime + MAX_EVENT_AGE_SECONDS < nowEpochSeconds) return false;
  return true;
}

bool BookOrbitStatsQueue::chapterProgressAtEndOfPage(const int currentPage, const int knownPageCount,
                                                     const int estimatedTotal, float& out) {
  out = 0.0f;
  if (currentPage < 0) return false;

  // Take the LARGEST credible chapter length. knownPageCount can be a build watermark rather
  // than the chapter total (Section::isPartial()), and on the last built page of a partially
  // laid-out giant spine it equals currentPage + 1 -- which would read as "chapter finished"
  // and inflate whole-book progress. estimatedTotalPages() is the reader's own best-known
  // figure and exceeds the watermark in exactly that case. For a finalized section the two
  // agree, so this is numerically identical to dividing by pageCount.
  int denominator = knownPageCount > estimatedTotal ? knownPageCount : estimatedTotal;
  // The page being finished is itself proof the chapter has at least this many pages, which
  // keeps the fraction in range even if both inputs are stale or zero.
  if (denominator < currentPage + 1) denominator = currentPage + 1;
  if (denominator <= 0) return false;

  out = static_cast<float>(currentPage + 1) / static_cast<float>(denominator);
  if (out < 0.0f) out = 0.0f;
  if (out > 1.0f) out = 1.0f;
  return true;
}

bool BookOrbitStatsQueue::makeEvent(const uint32_t nowEpochSeconds, const uint32_t dwellSeconds,
                                    const float overallProgress, BookOrbitStatEvent& out) {
  if (dwellSeconds == 0) return false;
  // The clock must be able to place the START of the page, not just its end.
  if (nowEpochSeconds <= dwellSeconds) return false;

  BookOrbitStatEvent event;
  event.startTime = nowEpochSeconds - dwellSeconds;
  // Clamped rather than trusted: the caller's dwell gate keeps this far below the ceiling
  // today (the reader discards any dwell past its 300 s idle threshold), but a future
  // threshold change must not silently truncate into uint16_t.
  //
  // uint16_t caps a record at 65535 s ~ 18 hours, which is deliberately NARROWER than the
  // server's own 86400 s limit: no single page dwell can legitimately approach either, and a
  // 16-bit field keeps the record at 12 bytes. The static_assert pins that relationship so the
  // clamp can never emit a value the server would reject.
  event.durationSeconds = static_cast<uint16_t>(std::min<uint32_t>(dwellSeconds, UINT16_MAX));
  static_assert(UINT16_MAX <= BookOrbitLimits::MAX_EVENT_DURATION_SECONDS,
                "a full-range durationSeconds must still be acceptable to the server");

  // NaN and out-of-range progress both land on 0 or PROGRESS_SCALE rather than wrapping:
  // PageStatEventDto.page carries no server-side @Max, so a bad value would pass validation
  // and surface as impossible >100% reading progress.
  float clamped = overallProgress;
  if (!(clamped >= 0.0f)) clamped = 0.0f;  // false for NaN as well as negatives
  if (clamped > 1.0f) clamped = 1.0f;
  event.page = static_cast<uint16_t>(
      std::min<uint32_t>(static_cast<uint32_t>(clamped * static_cast<float>(PROGRESS_SCALE) + 0.5f), PROGRESS_SCALE));
  event.totalPages = PROGRESS_SCALE;

  out = event;
  return true;
}

bool BookOrbitStatsQueue::appendBatch(const std::string& documentHash, const BookOrbitOwnerId& owner,
                                      const BookOrbitStatEvent* events, const size_t count) {
  if (!events || count == 0) return true;
  // Fail closed on an unresolved backup: with the live file gone and the backup unrestorable,
  // continuing would build a fresh queue and promote it over the only surviving copy.
  if (!recoverInterruptedReplacement(documentHash)) return false;
  // A batch bigger than the whole cap keeps only its most recent tail: same drop-oldest rule,
  // applied to the incoming events themselves.
  const size_t take = std::min(count, MAX_QUEUED_EVENTS);
  const BookOrbitStatEvent* first = events + (count - take);

  const std::string path = filePath(documentHash);

  size_t existingCount = 0;
  size_t partialTailBytes = 0;
  bool ownedByUs = false;
  {
    HalFile probe;
    BookOrbitOwnerId existingOwner;
    const QueueOpen status = openValidatedForRead(path, probe, existingCount, existingOwner);
    if (status == QueueOpen::Unreadable) {
      // A live queue exists but this attempt could not read it. Nothing is known about its
      // contents -- in particular, not whether it is ours and full of unsent events -- so it
      // must NOT be treated as absent or foreign, which would build a fresh queue and promote it
      // straight over valid history. Fail closed; the caller keeps its buffer for a later retry.
      LOG_ERR("BOQ", "Could not read the existing stats queue; keeping it and deferring this batch");
      return false;
    }
    if (status == QueueOpen::Ok) {
      ownedByUs = existingOwner == owner;
      if (ownedByUs) {
        // Bytes past the last whole record: an append interrupted by power loss.
        partialTailBytes = probe.fileSize() - (HEADER_BYTES + existingCount * RECORD_BYTES);
      } else {
        LOG_INF("BOQ", "Stats queue belongs to a different sync account; replacing it");
      }
    }
    if (!ownedByUs) existingCount = 0;
  }

  if (!ownedByUs) {
    // The live file is absent, unreadable, an older format, or -- the case that matters -- owned
    // by a DIFFERENT account. In none of those may this batch be appended in place: doing so
    // would file the new account's reading under the old account's header, which is precisely
    // the cross-account exposure the owner fingerprint exists to prevent.
    //
    // So the replacement is built COMPLETE, as a temp file carrying this owner's header, and
    // only then promoted (which stages the old live file aside through .bak and renames the temp
    // over it). Every failure below leaves the old queue exactly where it was and returns false:
    // fail closed, never a half-built live file that a later append could extend.
    const std::string tmpPath = tempPath(documentHash);
    Storage.ensureDirectoryExists(STATS_DIR);
    Storage.remove(tmpPath.c_str());

    bool built = true;
    {
      HalFile dest;
      if (!Storage.openFileForWrite("BOQ", tmpPath, dest)) {
        LOG_ERR("BOQ", "Could not open a replacement stats queue; keeping the existing one");
        return false;
      }
      uint8_t header[HEADER_BYTES];
      encodeHeader(header, owner);
      built = dest.write(header, sizeof(header)) == sizeof(header);

      uint8_t record[RECORD_BYTES];
      for (size_t i = 0; built && i < take; i++) {
        encodeRecord(first[i], record);
        built = dest.write(record, sizeof(record)) == sizeof(record);
      }
      dest.flush();
      // Same reasoning as the rewrite path: only close() reports whether the bytes were synced,
      // and an unsynced temp must never be promoted over an existing queue.
      if (!dest.close()) {
        LOG_ERR("BOQ", "New stats queue did not sync to the card");
        built = false;
      }
    }
    if (!built) {
      LOG_ERR("BOQ", "Could not write a complete replacement stats queue; keeping the existing one");
      Storage.remove(tmpPath.c_str());
      return false;
    }
    if (!promoteReplacement(documentHash)) return false;

    LOG_INF("BOQ", "Started a stats queue with %u event(s)", static_cast<unsigned>(take));
    return true;
  }

  // From here the live file is known to carry THIS owner's header, so appending to it in place
  // cannot cross an account boundary.
  const size_t drop = overflowDropCount(existingCount, take);
  if (drop > 0) {
    LOG_INF("BOQ", "Stats queue over cap, dropping %u oldest event(s)", static_cast<unsigned>(drop));
    if (!rewriteDroppingFirst(documentHash, drop)) return false;
  } else if (partialTailBytes > 0) {
    // A previous append was cut short by power loss, leaving bytes that are not a whole record.
    // Appending onto them would put every new record off the fixed-size grid that reads walk,
    // so the partial tail and everything after it would decode as checksum failures. Rebuild the
    // file from its whole records first -- the drop-oldest path above already does this, so this
    // branch only covers the case where no records needed dropping.
    LOG_INF("BOQ", "Repairing %u torn byte(s) at the tail of the stats queue", static_cast<unsigned>(partialTailBytes));
    if (!rewriteDroppingFirst(documentHash, 0)) return false;
  }

  HalFile file = Storage.open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND);
  if (!file.isOpen()) {
    LOG_ERR("BOQ", "Could not open stats queue for append");
    return false;
  }

  bool ok = true;
  uint8_t record[RECORD_BYTES];
  for (size_t i = 0; ok && i < take; i++) {
    encodeRecord(first[i], record);
    ok = file.write(record, sizeof(record)) == sizeof(record);
  }
  file.flush();
  const size_t totalBytes = file.fileSize();
  // close() is the only durability signal available; its bool is the underlying sync() result.
  const bool synced = file.close();
  if (!synced) LOG_ERR("BOQ", "Appended stats events did not sync to the card");

  if (!ok || !synced) {
    // Reported as failure so the caller keeps its RAM batch and retries later. Whatever reached
    // the card may be a torn tail, which the repair at the top of this function removes on the
    // next attempt; re-appending events that did land is harmless, because the server dedups on
    // (user, book, device, page, startTime) and those stamps never change.
    if (ok) return false;
    LOG_ERR("BOQ", "Short write appending %u stats event(s)", static_cast<unsigned>(take));
    return false;
  }
  LOG_INF("BOQ", "Queued %u stats event(s); file now holds %u", static_cast<unsigned>(take),
          static_cast<unsigned>(recordCountForSize(totalBytes)));
  return true;
}

size_t BookOrbitStatsQueue::readOldest(const std::string& documentHash, const BookOrbitOwnerId& owner,
                                       BookOrbitStatEvent* out, const size_t maxOut, size_t& outRecordsScanned) {
  outRecordsScanned = 0;
  if (!out || maxOut == 0) return 0;
  if (!recoverInterruptedReplacement(documentHash)) return 0;

  HalFile file;
  size_t recordCount = 0;
  BookOrbitOwnerId existingOwner;
  // Absent, foreign AND unreadable all report an empty batch. That is already fail-closed for
  // every caller: uploadQueued() returns without removing anything when it reads nothing, so a
  // transient error defers the drain rather than retiring data.
  if (openValidatedForRead(filePath(documentHash), file, recordCount, existingOwner) != QueueOpen::Ok) return 0;
  // Refuse to hand another account's reading history to the current one. Reported as an empty
  // queue rather than deleted here: appendBatch() owns the retirement, so a read never destroys
  // data.
  if (existingOwner != owner) return 0;

  // Scanning is bounded by maxOut file records, not by how many decode cleanly, so one corrupt
  // record cannot make this walk the whole file looking for a full batch.
  const size_t scanLimit = std::min(recordCount, maxOut);
  size_t written = 0;
  uint8_t record[RECORD_BYTES];
  for (size_t i = 0; i < scanLimit; i++) {
    if (file.read(record, sizeof(record)) != static_cast<int>(sizeof(record))) break;
    outRecordsScanned++;
    BookOrbitStatEvent event;
    // A record that fails its checksum or the field contract is skipped, not fatal: the rest of
    // the file is still good data, and the corrupt slot is retired with the batch around it --
    // which is why the caller removes outRecordsScanned records, not `written`.
    if (decodeRecord(record, event)) {
      out[written++] = event;
    }
  }
  return written;
}

bool BookOrbitStatsQueue::removeOldest(const std::string& documentHash, const size_t count) {
  if (count == 0) return true;
  if (!recoverInterruptedReplacement(documentHash)) return false;
  return rewriteDroppingFirst(documentHash, count);
}

void BookOrbitStatsQueue::clear(const std::string& documentHash) {
  // Retires every file the replacement protocol can leave behind, so "clear" really does bound
  // this book's footprint to zero rather than stranding a temp or backup for a later recovery
  // pass to resurrect.
  Storage.remove(filePath(documentHash).c_str());
  Storage.remove(tempPath(documentHash).c_str());
  Storage.remove(backupPath(documentHash).c_str());
}

bool BookOrbitStatsQueue::rewriteOwner(const std::string& documentHash, const BookOrbitOwnerId& oldOwner,
                                       const BookOrbitOwnerId& newOwner) {
  if (!recoverInterruptedReplacement(documentHash)) return false;
  return rewriteOwnerImpl(documentHash, oldOwner, newOwner);
}

size_t BookOrbitStatsQueue::queuedCount(const std::string& documentHash, const BookOrbitOwnerId& owner) {
  if (!recoverInterruptedReplacement(documentHash)) return 0;
  HalFile file;
  size_t recordCount = 0;
  BookOrbitOwnerId existingOwner;
  // Diagnostic only; an unreadable queue simply reports nothing rather than claiming emptiness
  // to any decision-making code.
  if (openValidatedForRead(filePath(documentHash), file, recordCount, existingOwner) != QueueOpen::Ok) return 0;
  return existingOwner == owner ? recordCount : 0;
}
