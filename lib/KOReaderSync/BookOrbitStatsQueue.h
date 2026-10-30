#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "BookOrbitOwnerId.h"
#include "BookOrbitStats.h"

/**
 * Durable, bounded on-SD queue of per-page reading events, one file per book inside the
 * book's own cache directory (<documentHash>/bo_stats.bin -- see docs/file-formats.md).
 *
 * Design constraints this exists to satisfy on the X3 (380 KB RAM, no PSRAM):
 *  - EXPLICIT little-endian codec with a version byte and a per-record checksum, following
 *    BookReadingStats.cpp's readLe/writeLe house pattern. Records are never raw structs:
 *    a memcpy'd struct would carry padding, endianness and RISC-V alignment hazards into a
 *    persisted format (see CLAUDE.md's alignment section).
 *  - FIXED, SMALL RAM footprint. Nothing here ever loads the whole queue: reads fill a
 *    caller-provided array of at most MAX_EVENTS_PER_BATCH records (32 x 12 = 384 bytes) and
 *    the drop-oldest rewrite streams one record at a time.
 *  - BOUNDED on disk. Beyond MAX_QUEUED_EVENTS the OLDEST events are dropped -- recent
 *    reading is worth more than stale backlog -- and events older than MAX_EVENT_AGE_SECONDS
 *    are dropped on read, so a device that never reaches its server cannot hoard forever.
 *  - CORRUPTION TOLERANT. A short/absent header, trailing partial bytes from a torn write,
 *    and individual records failing their checksum are all survivable: whatever remains
 *    decodes, the rest is skipped. Nothing here aborts or throws (-fno-exceptions).
 *  - STABLE TIMESTAMPS. Records are written once at capture and are never re-stamped;
 *    removal happens only after the server confirms it ingested them, so a retry re-sends
 *    byte-identical events and hits the server's own dedup key.
 */
class BookOrbitStatsQueue {
 public:
  // Fixed denominator reported as `totalPages`: positions are basis points, so page/totalPages
  // reproduces the exact reading fraction on any pagination engine.
  static constexpr uint16_t PROGRESS_SCALE = 10000;

  // Header: 3 magic bytes + version + the 16-byte owner fingerprint (see below).
  // Records are 12 bytes each.
  static constexpr size_t HEADER_BYTES = 4 + BookOrbitOwnerId::BYTES;
  static constexpr size_t RECORD_BYTES = 12;
  // v2 widened the owner from a 32-bit hash to a 128-bit fingerprint. A v1 file is not migrated
  // -- its 32-bit owner cannot be re-derived into the new space, and guessing would be exactly
  // the cross-account mistake the fingerprint exists to prevent -- so it is discarded like any
  // other unreadable header. Nothing has shipped, so this costs at most un-uploaded events.
  static constexpr uint8_t FORMAT_VERSION = 2;

  // On-SD ceiling: 512 x 12 B = 6 KB per book, worst case.
  static constexpr size_t MAX_QUEUED_EVENTS = 512;

  // Events buffered in the reader during one session, and the number uploaded per request.
  // Both deliberately small: 32 x 12 B = 384 bytes of RAM, and a request body of roughly
  // 2 KB, which is built and serialised before any TLS session is opened.
  static constexpr size_t MAX_EVENTS_PER_BATCH = 32;

  // Staleness bound. An event older than this is dropped rather than uploaded: BookOrbit
  // clusters by absolute time, so month-old backlog would land as phantom history.
  static constexpr uint32_t MAX_EVENT_AGE_SECONDS = 30u * 24u * 60u * 60u;

  // ---- Owner binding --------------------------------------------------------------------
  // Every queue records the 128-bit fingerprint of the server+account whose reading it captured
  // (see BookOrbitOwnerId). Reading history is personal data: if the user reconfigures KOSync to
  // a different account or server, events captured under the old one must never be uploaded to
  // the new one. A mismatch is therefore not merged or re-owned but discarded -- those events
  // can no longer reach the account they belong to, and keeping them would only be a standing
  // risk of sending them to the wrong one.

  // ---- Pure codec (no storage; host-testable on its own) --------------------------------
  static void encodeHeader(uint8_t* out, const BookOrbitOwnerId& owner);
  static bool headerValid(const uint8_t* data, size_t len);
  // Owner recorded in a valid header. Undefined for an invalid one; check headerValid first.
  static BookOrbitOwnerId headerOwner(const uint8_t* data);
  static void encodeRecord(const BookOrbitStatEvent& event, uint8_t* out);
  // False when the 12 bytes fail their checksum or carry an out-of-contract field.
  static bool decodeRecord(const uint8_t* in, BookOrbitStatEvent& out);
  // Number of leading events that must be dropped so existingCount + addCount fits the cap.
  static size_t overflowDropCount(size_t existingCount, size_t addCount);
  // True when the event satisfies the server's own field contract and is not stale relative to
  // nowEpochSeconds (0 disables the staleness check, for callers with no clock).
  static bool isUploadable(const BookOrbitStatEvent& event, uint32_t nowEpochSeconds);

  /**
   * Build one event for a page that has just been finished.
   *
   * `nowEpochSeconds` is the capture-time wall clock, `dwellSeconds` a dwell that has ALREADY
   * passed the reader's minimum/idle qualification, and `overallProgress` the position at the
   * END of that page as a fraction of the whole book. startTime is derived here, once, and is
   * never recomputed later -- the server's (user, book, device, page, startTime) dedup key
   * depends on a retry carrying the identical stamp.
   *
   * False when the inputs cannot yield a server-valid event (no usable clock, zero dwell, or a
   * dwell that would place the start before the epoch). Pure: no clock, no storage, no logging.
   */
  static bool makeEvent(uint32_t nowEpochSeconds, uint32_t dwellSeconds, float overallProgress,
                        BookOrbitStatEvent& out);

  // ---- Storage-backed queue ------------------------------------------------------------
  // Appends up to `count` events, dropping the oldest queued events if the cap would be
  // exceeded. Empty batch is a no-op success. False only on genuine storage failure.
  // A queue owned by a DIFFERENT ownerKey is discarded and replaced rather than appended to.
  static bool appendBatch(const std::string& documentHash, const BookOrbitOwnerId& owner,
                          const BookOrbitStatEvent* events, size_t count);

  // Fills `out` with the decodable events among the OLDEST `maxOut` file records, and reports
  // via outRecordsScanned how many file records that covered. Returns how many events were
  // written to `out`.
  //
  // The two counts differ when a record fails its checksum or the field contract: such a record
  // is skipped, not returned. Callers MUST pass outRecordsScanned (not the return value) to
  // removeOldest() once the batch is confirmed -- otherwise a corrupt record at the head of the
  // queue would never be removed, and would shift every subsequent removal onto the wrong
  // records. Scanning is bounded by maxOut, so this never walks the whole file.
  // Returns 0 (and scans nothing) when the queue belongs to a different ownerKey, so events
  // captured under one account can never be handed to another.
  static size_t readOldest(const std::string& documentHash, const BookOrbitOwnerId& owner, BookOrbitStatEvent* out,
                           size_t maxOut, size_t& outRecordsScanned);

  // Drops the first `count` records, rewriting the file via a temp-file-then-rename so a power
  // loss mid-rewrite cannot leave a half-written queue. Removing at or beyond the record count
  // deletes the file outright.
  static bool removeOldest(const std::string& documentHash, size_t count);

  // Deletes the queue file. A missing file is success.
  static void clear(const std::string& documentHash);

  /**
   * One-time migration seam: rewrites the queue's header from `oldOwner` to `newOwner`,
   * preserving every record, via the same atomic temp+backup+promote protocol as
   * removeOldest()/appendBatch(). Exists for BookOrbitMigration.h's upgrade from e51b383's
   * opportunistic KOReaderCredentialStore-derived owner to an explicit BookOrbitCredentialStore
   * one -- a credential COPY alone is not sufficient when the two stores' getBaseUrl() values
   * differ (e.g. BookOrbit's canonical "/api/v1/koreader" normalization), since a mismatched
   * owner would otherwise make appendBatch() silently discard the whole queue on next write.
   *
   * A no-op (returns true, touches nothing) when there is no live queue, the live queue is not
   * FORMAT_VERSION-valid, or its current owner is not exactly `oldOwner` -- in particular this
   * is safe to call speculatively without first checking whether a rewrite is needed. FORMAT
   * VERSION is deliberately left unchanged: the record layout is unaffected, and headerValid()
   * rejects any version it does not recognize (see the v1 precedent this format already
   * survived), so bumping it here would discard every untouched queue rather than migrate it.
   */
  static bool rewriteOwner(const std::string& documentHash, const BookOrbitOwnerId& oldOwner,
                           const BookOrbitOwnerId& newOwner);

  // Cheap diagnostic size probe; does not decode records. 0 for absent files, files with a
  // foreign header, and files owned by a different ownerKey.
  static size_t queuedCount(const std::string& documentHash, const BookOrbitOwnerId& owner);

  /**
   * Where this book's queue lives: a stats-owned directory, keyed by the KOSync DOCUMENT HASH.
   *
   * Every entry point below takes that hash as its book identity. It is deliberately not the
   * cache path or anything derived from it: the cache directory is deleted by "Clear Cache" and
   * by every cache rebuild, and its name is a hash of the book's full path, so moving or
   * renaming the book stranded whatever had not yet been uploaded. The document hash is also the
   * identity these events are uploaded under, so the queue and the payload can never disagree
   * about which book they concern.
   */
  static std::string filePath(const std::string& documentHash);
  static std::string storageDirectory();
  static std::string tempPath(const std::string& documentHash);
  static std::string backupPath(const std::string& documentHash);

  /**
   * Resolve whatever live/tmp/backup combination a previous interrupted replacement left behind,
   * bringing the book's queue files back to a single deterministic state. Called automatically
   * at the start of every entry point below, so an interrupted replacement is repaired on next
   * access rather than needing a boot-time sweep.
   *
   * The replacement protocol never destroys the only valid copy: the new content is written and
   * flushed to <path>.tmp first, the live file is renamed aside to <path>.bak, and only then is
   * tmp promoted to live and the backup retired. Recovery reads the surviving combination:
   *   live + bak  -> promotion completed, the backup is stale     -> drop bak (and any tmp)
   *   bak, no live -> crashed between retiring live and promoting -> restore bak as live
   *   tmp, no live/bak -> crashed while writing tmp, unverified   -> drop tmp
   * Restoring the backup rather than promoting the tmp is deliberate: the backup is the last
   * state known to be complete, and the only cost of preferring it is re-uploading events the
   * server already deduplicates.
   *
   * Returns false for exactly one state: the live file is gone, a backup is the only surviving
   * copy, and it could not be renamed back. Every operation above then FAILS CLOSED rather than
   * carrying on -- because carrying on means building a fresh queue and promoting it over that
   * backup, destroying the only copy of the data the backup exists to protect. A stale backup
   * alongside a valid live file is not this state; it is resolved, and only blocks the next
   * replacement until it can be removed.
   */
  static bool recoverInterruptedReplacement(const std::string& documentHash);

  /**
   * Fraction of the current chapter completed at the END of `currentPage`, against the best
   * known chapter total.
   *
   * `knownPageCount` is Section::pageCount, which for a partially built or suspended giant
   * spine is only a build WATERMARK, not the chapter length -- on the last built page it equals
   * currentPage + 1 and would read as "chapter finished". `estimatedTotal` is
   * Section::estimatedTotalPages(), the same best-known figure the reader's own time-left
   * estimate uses. Taking the larger of the two keeps a partial section from overstating
   * progress while leaving a finalized section (where the two agree) numerically identical.
   *
   * False when no sane denominator exists (negative page, no pages at all).
   */
  static bool chapterProgressAtEndOfPage(int currentPage, int knownPageCount, int estimatedTotal, float& out);
};
