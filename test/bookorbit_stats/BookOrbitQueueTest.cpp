// Durability tests for the REAL lib/KOReaderSync/BookOrbitStatsQueue.cpp (compiled directly,
// not reimplemented) against an in-memory read-write HalStorage stub.
//
// The queue's entire reason to exist is surviving things that go wrong on an e-reader: a cap
// reached by a device that never sees its server, a power loss mid-append, a file written by a
// different firmware version, and a retry that must not shift the reading history it re-sends.
// Every one of those is exercised here, several by hand-crafting byte-exact files that a real
// SD sequence could not be relied upon to produce.

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "BookOrbitOwnerId.h"
#include "BookOrbitStatsQueue.h"
#include "HalStorage.h"

namespace {

// The queue's book identity is the KOSync document hash, not a path -- see filePath().
constexpr char CACHE[] = "0123456789abcdef0123456789abcdef";
// Two distinct sync identities (see BookOrbitStats::identityKey()): reading history captured
// under one must never be uploaded to the other.
BookOrbitOwnerId makeOwner(const uint8_t seed) {
  BookOrbitOwnerId id;
  for (size_t i = 0; i < BookOrbitOwnerId::BYTES; i++) id.bytes[i] = static_cast<uint8_t>(seed + i);
  return id;
}
const BookOrbitOwnerId OWNER = makeOwner(0xA0);
const BookOrbitOwnerId OTHER_OWNER = makeOwner(0x10);
// A stats-owned directory, deliberately NOT the book's rebuildable cache directory, and named
// by the document hash so a move or rename cannot strand the queue.
const std::string QUEUE_PATH = "/.crosspoint/bo_stats/0123456789abcdef0123456789abcdef.bin";

BookOrbitStatEvent makeEvent(const uint32_t startTime, const uint16_t page = 100, const uint16_t duration = 30) {
  BookOrbitStatEvent e;
  e.startTime = startTime;
  e.durationSeconds = duration;
  e.page = page;
  e.totalPages = BookOrbitStatsQueue::PROGRESS_SCALE;
  return e;
}

std::vector<BookOrbitStatEvent> drain(const size_t maxOut = BookOrbitStatsQueue::MAX_EVENTS_PER_BATCH) {
  std::vector<BookOrbitStatEvent> out(maxOut);
  size_t scanned = 0;
  const size_t n = BookOrbitStatsQueue::readOldest(CACHE, OWNER, out.data(), maxOut, scanned);
  out.resize(n);
  return out;
}

std::vector<uint8_t>& rawFile() { return testStorage().files[QUEUE_PATH]; }

class BookOrbitQueueTest : public ::testing::Test {
 protected:
  void SetUp() override { testStorage().reset(); }
};

// ---- Codec ---------------------------------------------------------------------------------

TEST_F(BookOrbitQueueTest, RecordRoundTripsThroughExplicitLittleEndianCodec) {
  const BookOrbitStatEvent original = makeEvent(1893456000u, 7321, 275);
  uint8_t bytes[BookOrbitStatsQueue::RECORD_BYTES];
  BookOrbitStatsQueue::encodeRecord(original, bytes);

  BookOrbitStatEvent decoded;
  ASSERT_TRUE(BookOrbitStatsQueue::decodeRecord(bytes, decoded));
  EXPECT_EQ(decoded.startTime, original.startTime);
  EXPECT_EQ(decoded.durationSeconds, original.durationSeconds);
  EXPECT_EQ(decoded.page, original.page);
  EXPECT_EQ(decoded.totalPages, original.totalPages);
}

TEST_F(BookOrbitQueueTest, EncodingIsLittleEndianAtFixedOffsets) {
  // Pins the on-disk layout: a future refactor that reorders fields or changes endianness must
  // fail here rather than silently misreading every queue already on a user's SD card.
  uint8_t bytes[BookOrbitStatsQueue::RECORD_BYTES];
  BookOrbitStatsQueue::encodeRecord(makeEvent(0x04030201u, 0x0605, 0x0807), bytes);
  EXPECT_EQ(bytes[0], 0x01);
  EXPECT_EQ(bytes[1], 0x02);
  EXPECT_EQ(bytes[2], 0x03);
  EXPECT_EQ(bytes[3], 0x04);
  EXPECT_EQ(bytes[4], 0x07);  // durationSeconds low byte
  EXPECT_EQ(bytes[5], 0x08);
  EXPECT_EQ(bytes[6], 0x05);  // page low byte
  EXPECT_EQ(bytes[7], 0x06);
}

TEST_F(BookOrbitQueueTest, DecodeRejectsAnyFlippedBit) {
  uint8_t bytes[BookOrbitStatsQueue::RECORD_BYTES];
  BookOrbitStatsQueue::encodeRecord(makeEvent(1893456000u), bytes);

  for (size_t i = 0; i < BookOrbitStatsQueue::RECORD_BYTES; i++) {
    uint8_t corrupted[BookOrbitStatsQueue::RECORD_BYTES];
    memcpy(corrupted, bytes, sizeof(corrupted));
    corrupted[i] ^= 0x40;
    BookOrbitStatEvent decoded;
    EXPECT_FALSE(BookOrbitStatsQueue::decodeRecord(corrupted, decoded)) << "byte " << i << " went undetected";
  }
}

TEST_F(BookOrbitQueueTest, DecodeRejectsOutOfContractFields) {
  // A checksum-clean record can still be nonsense; the field contract is enforced locally so the
  // server never has to 400 on our payload.
  BookOrbitStatEvent zeroStart = makeEvent(0);
  uint8_t bytes[BookOrbitStatsQueue::RECORD_BYTES];
  BookOrbitStatsQueue::encodeRecord(zeroStart, bytes);
  BookOrbitStatEvent decoded;
  EXPECT_FALSE(BookOrbitStatsQueue::decodeRecord(bytes, decoded)) << "startTime 0 violates the server's @Min(1)";

  BookOrbitStatEvent pageBeyondTotal = makeEvent(1893456000u);
  pageBeyondTotal.page = static_cast<uint16_t>(BookOrbitStatsQueue::PROGRESS_SCALE + 1);
  BookOrbitStatsQueue::encodeRecord(pageBeyondTotal, bytes);
  EXPECT_FALSE(BookOrbitStatsQueue::decodeRecord(bytes, decoded)) << "page must not exceed totalPages";

  BookOrbitStatEvent noTotal = makeEvent(1893456000u);
  noTotal.totalPages = 0;
  noTotal.page = 0;
  BookOrbitStatsQueue::encodeRecord(noTotal, bytes);
  EXPECT_FALSE(BookOrbitStatsQueue::decodeRecord(bytes, decoded)) << "totalPages must satisfy @Min(1)";
}

TEST_F(BookOrbitQueueTest, MakeEventDerivesStartTimeAndClampsBasisPoints) {
  BookOrbitStatEvent event;
  ASSERT_TRUE(BookOrbitStatsQueue::makeEvent(1893456000u, 45u, 0.5f, event));
  EXPECT_EQ(event.startTime, 1893456000u - 45u);
  EXPECT_EQ(event.durationSeconds, 45);
  EXPECT_EQ(event.page, 5000);
  EXPECT_EQ(event.totalPages, BookOrbitStatsQueue::PROGRESS_SCALE);

  // page has no server-side @Max, so an out-of-range fraction must be clamped here or it would
  // pass validation and land as impossible >100% progress.
  ASSERT_TRUE(BookOrbitStatsQueue::makeEvent(1893456000u, 10u, 1.9f, event));
  EXPECT_EQ(event.page, BookOrbitStatsQueue::PROGRESS_SCALE);
  ASSERT_TRUE(BookOrbitStatsQueue::makeEvent(1893456000u, 10u, -0.4f, event));
  EXPECT_EQ(event.page, 0);
  ASSERT_TRUE(BookOrbitStatsQueue::makeEvent(1893456000u, 10u, std::nanf(""), event));
  EXPECT_EQ(event.page, 0);
}

TEST_F(BookOrbitQueueTest, MakeEventRefusesWhenTheClockCannotPlaceTheStart) {
  BookOrbitStatEvent event;
  EXPECT_FALSE(BookOrbitStatsQueue::makeEvent(1893456000u, 0u, 0.5f, event)) << "zero dwell is not an event";
  EXPECT_FALSE(BookOrbitStatsQueue::makeEvent(0u, 30u, 0.5f, event)) << "no clock means no event";
  EXPECT_FALSE(BookOrbitStatsQueue::makeEvent(20u, 30u, 0.5f, event)) << "start would precede the epoch";
}

// ---- Append / read / remove ----------------------------------------------------------------

TEST_F(BookOrbitQueueTest, AppendThenReadPreservesOrderAndValues) {
  const BookOrbitStatEvent batch[] = {makeEvent(1000), makeEvent(2000), makeEvent(3000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, batch, 3));
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 3u);

  const auto read = drain();
  ASSERT_EQ(read.size(), 3u);
  EXPECT_EQ(read[0].startTime, 1000u);
  EXPECT_EQ(read[1].startTime, 2000u);
  EXPECT_EQ(read[2].startTime, 3000u);
}

TEST_F(BookOrbitQueueTest, AppendAccumulatesAcrossSessions) {
  const BookOrbitStatEvent first[] = {makeEvent(1000)};
  const BookOrbitStatEvent second[] = {makeEvent(2000), makeEvent(3000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, first, 1));
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, second, 2));
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 3u);
  EXPECT_EQ(drain()[0].startTime, 1000u) << "oldest event must stay at the head";
}

TEST_F(BookOrbitQueueTest, EmptyBatchIsANoOpSuccessAndWritesNothing) {
  const BookOrbitStatEvent none[1] = {};
  EXPECT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, none, 0));
  EXPECT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, nullptr, 3));
  EXPECT_FALSE(Storage.exists(QUEUE_PATH.c_str())) << "a no-op must not create a queue file";
}

TEST_F(BookOrbitQueueTest, RemoveOldestDropsExactlyTheConfirmedPrefix) {
  std::vector<BookOrbitStatEvent> events;
  for (uint32_t i = 0; i < 10; i++) events.push_back(makeEvent(1000 + i));
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, events.data(), events.size()));

  ASSERT_TRUE(BookOrbitStatsQueue::removeOldest(CACHE, 4));
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 6u);
  const auto remaining = drain();
  ASSERT_EQ(remaining.size(), 6u);
  EXPECT_EQ(remaining[0].startTime, 1004u);
  EXPECT_EQ(remaining[5].startTime, 1009u);
}

TEST_F(BookOrbitQueueTest, RemovingEverythingDeletesTheFileRatherThanLeavingAnEmptyOne) {
  const BookOrbitStatEvent batch[] = {makeEvent(1000), makeEvent(2000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, batch, 2));
  ASSERT_TRUE(BookOrbitStatsQueue::removeOldest(CACHE, 2));
  EXPECT_FALSE(Storage.exists(QUEUE_PATH.c_str()));
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 0u);

  // Removing more than exists is not an error, and must not leave a stray temp file behind.
  const BookOrbitStatEvent again[] = {makeEvent(3000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, again, 1));
  EXPECT_TRUE(BookOrbitStatsQueue::removeOldest(CACHE, 99));
  EXPECT_FALSE(Storage.exists((QUEUE_PATH + ".tmp").c_str()));
}

TEST_F(BookOrbitQueueTest, ClearRemovesQueueAndAnyLeftoverTempFile) {
  const BookOrbitStatEvent batch[] = {makeEvent(1000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, batch, 1));
  testStorage().files[QUEUE_PATH + ".tmp"] = {0x01, 0x02};

  BookOrbitStatsQueue::clear(CACHE);
  EXPECT_FALSE(Storage.exists(QUEUE_PATH.c_str()));
  EXPECT_FALSE(Storage.exists((QUEUE_PATH + ".tmp").c_str()));
}

// ---- Cap and drop-oldest --------------------------------------------------------------------

TEST_F(BookOrbitQueueTest, OverflowDropCountKeepsTheQueueAtTheCap) {
  constexpr size_t CAP = BookOrbitStatsQueue::MAX_QUEUED_EVENTS;
  EXPECT_EQ(BookOrbitStatsQueue::overflowDropCount(0, 10), 0u);
  EXPECT_EQ(BookOrbitStatsQueue::overflowDropCount(CAP - 5, 5), 0u) << "exactly at the cap must not drop";
  EXPECT_EQ(BookOrbitStatsQueue::overflowDropCount(CAP, 1), 1u);
  EXPECT_EQ(BookOrbitStatsQueue::overflowDropCount(CAP, 20), 20u);
  // A batch larger than the whole cap cannot be satisfied by dropping more than exists.
  EXPECT_EQ(BookOrbitStatsQueue::overflowDropCount(10, CAP * 2), 10u);
}

TEST_F(BookOrbitQueueTest, AppendingPastTheCapDropsTheOLDESTEventsNotTheNewest) {
  constexpr size_t CAP = BookOrbitStatsQueue::MAX_QUEUED_EVENTS;
  std::vector<BookOrbitStatEvent> fill;
  for (uint32_t i = 0; i < CAP; i++) fill.push_back(makeEvent(1000 + i));
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, fill.data(), fill.size()));
  ASSERT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), CAP);

  const BookOrbitStatEvent fresh[] = {makeEvent(9000), makeEvent(9001), makeEvent(9002)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, fresh, 3));

  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), CAP) << "the queue must stay bounded";
  const auto head = drain(1);
  ASSERT_EQ(head.size(), 1u);
  EXPECT_EQ(head[0].startTime, 1003u) << "the three oldest events should have been dropped";

  // And the newest events survived: recent reading is worth more than stale backlog.
  ASSERT_TRUE(BookOrbitStatsQueue::removeOldest(CACHE, CAP - 3));
  const auto tail = drain();
  ASSERT_EQ(tail.size(), 3u);
  EXPECT_EQ(tail[2].startTime, 9002u);
}

TEST_F(BookOrbitQueueTest, ABatchLargerThanTheCapKeepsItsMostRecentTail) {
  constexpr size_t CAP = BookOrbitStatsQueue::MAX_QUEUED_EVENTS;
  std::vector<BookOrbitStatEvent> huge;
  for (uint32_t i = 0; i < CAP + 25; i++) huge.push_back(makeEvent(1000 + i));
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, huge.data(), huge.size()));

  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), CAP);
  const auto head = drain(1);
  ASSERT_EQ(head.size(), 1u);
  EXPECT_EQ(head[0].startTime, 1000u + 25u);
}

// ---- Account binding ---------------------------------------------------------------------------

TEST_F(BookOrbitQueueTest, EventsCapturedUnderOneAccountAreNeverReadableByAnother) {
  // Reading history is personal data. If the user repoints KOSync at a different account or
  // server, the previous account's queued events must not be uploadable under the new
  // credentials -- the queue is the only thing standing between them and the wrong account.
  const BookOrbitStatEvent mine[] = {makeEvent(1000), makeEvent(2000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, mine, 2));

  BookOrbitStatEvent out[BookOrbitStatsQueue::MAX_EVENTS_PER_BATCH];
  size_t scanned = 0;
  EXPECT_EQ(BookOrbitStatsQueue::readOldest(CACHE, OTHER_OWNER, out, sizeof(out) / sizeof(out[0]), scanned), 0u);
  EXPECT_EQ(scanned, 0u) << "a foreign queue must not even be scanned";
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OTHER_OWNER), 0u);

  // The rightful owner still sees them: a mismatch hides the queue, it does not destroy it.
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 2u);
  EXPECT_EQ(drain().size(), 2u);
}

TEST_F(BookOrbitQueueTest, AppendingUnderANewAccountDiscardsTheOldAccountsEvents) {
  const BookOrbitStatEvent theirs[] = {makeEvent(1000), makeEvent(2000), makeEvent(3000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OTHER_OWNER, theirs, 3));

  const BookOrbitStatEvent mine[] = {makeEvent(9000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, mine, 1));

  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 1u) << "the old account's events must be gone, not merged";
  const auto read = drain();
  ASSERT_EQ(read.size(), 1u);
  EXPECT_EQ(read[0].startTime, 9000u);
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OTHER_OWNER), 0u);
}

TEST_F(BookOrbitQueueTest, TheTornTailRepairPreservesTheOwnerRatherThanReclaimingTheQueue) {
  // The repair path rewrites the file. If it stamped the CURRENT configuration's owner instead
  // of the one it read, an append under a new account would silently re-own the old account's
  // events and upload them -- the exact leak the owner key exists to prevent, reintroduced
  // through the fix for a different bug.
  const BookOrbitStatEvent theirs[] = {makeEvent(1000), makeEvent(2000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OTHER_OWNER, theirs, 2));

  ASSERT_TRUE(BookOrbitStatsQueue::removeOldest(CACHE, 1));
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OTHER_OWNER), 1u) << "still theirs after a rewrite";
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 0u);
}

// ---- Atomic replacement, failure injection and crash recovery ---------------------------------

TEST_F(BookOrbitQueueTest, AFailedPromotionRenameKeepsTheQueueRatherThanLosingIt) {
  // The old sequence was "delete live, then rename tmp into place". A rename failure -- or a
  // power cut of a few milliseconds -- in that window destroyed every un-uploaded event. The
  // live file must survive a promotion that cannot complete.
  const BookOrbitStatEvent batch[] = {makeEvent(1000), makeEvent(2000), makeEvent(3000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, batch, 3));

  testStorage().failRenames = true;
  EXPECT_FALSE(BookOrbitStatsQueue::removeOldest(CACHE, 1));
  testStorage().failRenames = false;

  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 3u) << "nothing may be lost to a failed rewrite";
  EXPECT_EQ(drain().size(), 3u);
}

TEST_F(BookOrbitQueueTest, AFailureAtEachReplacementStageLeavesAReadableQueue) {
  // Walks the protocol stage by stage: 1 = staging the live file aside, 2 = promoting the temp.
  for (const int failAt : {1, 2}) {
    testStorage().reset();
    const BookOrbitStatEvent batch[] = {makeEvent(1000), makeEvent(2000), makeEvent(3000)};
    ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, batch, 3));

    testStorage().renameCount = 0;
    testStorage().failRenameAt = failAt;
    BookOrbitStatsQueue::removeOldest(CACHE, 1);
    testStorage().failRenameAt = 0;

    // Whether the trim succeeded or not, the events must still be readable afterwards -- via the
    // live file, or via the backup that the next access restores.
    const size_t remaining = BookOrbitStatsQueue::queuedCount(CACHE, OWNER);
    EXPECT_GT(remaining, 0u) << "stage " << failAt << " lost the whole queue";
    EXPECT_EQ(drain().size(), remaining) << "stage " << failAt;
  }
}

TEST_F(BookOrbitQueueTest, TempWriteFailureNeverTouchesTheLiveQueue) {
  const BookOrbitStatEvent batch[] = {makeEvent(1000), makeEvent(2000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, batch, 2));
  const std::vector<uint8_t> before = rawFile();

  testStorage().failWrites = true;
  EXPECT_FALSE(BookOrbitStatsQueue::removeOldest(CACHE, 1));
  testStorage().failWrites = false;

  EXPECT_EQ(rawFile(), before) << "a failed temp write must not have modified the live file";
  EXPECT_FALSE(Storage.exists((QUEUE_PATH + ".tmp").c_str()));
  EXPECT_EQ(drain().size(), 2u);
}

TEST_F(BookOrbitQueueTest, TempOpenFailureNeverTouchesTheLiveQueue) {
  const BookOrbitStatEvent batch[] = {makeEvent(1000), makeEvent(2000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, batch, 2));

  testStorage().failOpenWrite = true;
  EXPECT_FALSE(BookOrbitStatsQueue::removeOldest(CACHE, 1));
  testStorage().failOpenWrite = false;

  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 2u);
}

TEST_F(BookOrbitQueueTest, RecoversFromACrashBetweenStagingAsideAndPromoting) {
  // Simulates power loss at the one instant the live path holds no file: the backup carries the
  // data and a complete temp is also on the card. Recovery must restore the BACKUP -- the last
  // state known to be complete -- rather than trusting a temp whose write was never confirmed.
  const BookOrbitStatEvent batch[] = {makeEvent(1000), makeEvent(2000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, batch, 2));

  auto& files = testStorage().files;
  files[QUEUE_PATH + ".bak"] = files[QUEUE_PATH];
  files[QUEUE_PATH + ".tmp"] = files[QUEUE_PATH];
  files.erase(QUEUE_PATH);

  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 2u) << "recovery must restore the backup";
  EXPECT_EQ(drain().size(), 2u);
  EXPECT_FALSE(Storage.exists((QUEUE_PATH + ".bak").c_str()));
  EXPECT_FALSE(Storage.exists((QUEUE_PATH + ".tmp").c_str()));
}

TEST_F(BookOrbitQueueTest, RecoversFromACrashAfterPromotionByRetiringTheBackup) {
  const BookOrbitStatEvent batch[] = {makeEvent(1000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, batch, 1));
  auto& files = testStorage().files;
  files[QUEUE_PATH + ".bak"] = files[QUEUE_PATH];  // stale: promotion had already completed

  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 1u);
  EXPECT_FALSE(Storage.exists((QUEUE_PATH + ".bak").c_str())) << "a stale backup must be retired, not resurrected";
}

TEST_F(BookOrbitQueueTest, RecoveryDiscardsAnUnverifiedTempWithNoLiveOrBackup) {
  // Crash while the temp was still being written: it was never confirmed complete, and there is
  // no live file it could stand in for.
  testStorage().files[QUEUE_PATH + ".tmp"] = {'B', 'O', 'Q', 2, 0, 0, 0};
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 0u);
  EXPECT_FALSE(Storage.exists((QUEUE_PATH + ".tmp").c_str()));
}

TEST_F(BookOrbitQueueTest, ClearRetiresEveryFileTheProtocolCanLeaveBehind) {
  const BookOrbitStatEvent batch[] = {makeEvent(1000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, batch, 1));
  testStorage().files[QUEUE_PATH + ".tmp"] = {0x01};
  testStorage().files[QUEUE_PATH + ".bak"] = {0x02};

  BookOrbitStatsQueue::clear(CACHE);
  EXPECT_FALSE(Storage.exists(QUEUE_PATH.c_str()));
  EXPECT_FALSE(Storage.exists((QUEUE_PATH + ".tmp").c_str()));
  EXPECT_FALSE(Storage.exists((QUEUE_PATH + ".bak").c_str()));
}

TEST_F(BookOrbitQueueTest, AShortReadAbandonsTheReplacementInsteadOfPromotingATruncatedQueue) {
  // The subtle one: if the copy loop treats a short READ as "done" rather than as a failure, the
  // temp holds fewer records than the live file and is then promoted over it -- turning one
  // transient read error into permanent loss of every record that was not copied.
  const BookOrbitStatEvent batch[] = {makeEvent(1000), makeEvent(2000), makeEvent(3000), makeEvent(4000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, batch, 4));

  // Truncate the live file mid-record so the rewrite's second read comes up short, while the
  // header still advertises the original record count.
  testStorage().failReadAfter = BookOrbitStatsQueue::HEADER_BYTES + BookOrbitStatsQueue::RECORD_BYTES + 4;
  const bool trimmed = BookOrbitStatsQueue::removeOldest(CACHE, 1);
  testStorage().failReadAfter = SIZE_MAX;

  EXPECT_FALSE(trimmed) << "an incomplete copy must be reported as a failure";
  EXPECT_FALSE(Storage.exists((QUEUE_PATH + ".tmp").c_str())) << "the truncated temp must be discarded";
  // The live queue is untouched, so nothing was lost to the failed rewrite.
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 4u);
  EXPECT_EQ(drain().size(), 4u);
}

TEST_F(BookOrbitQueueTest, AnUnrestorableBackupIsNeverDestroyedByALaterOperation) {
  // The crash-recovery ship blocker. State: the live file is gone (crash between staging aside
  // and promoting) and the backup is the ONLY copy left -- but the restoring rename fails too.
  // If recovery merely gives up and lets the caller continue, the next append builds a fresh
  // queue and promotion deletes the backup to make room. That is total, silent loss of every
  // un-uploaded event, caused by the very mechanism meant to protect them.
  const BookOrbitStatEvent batch[] = {makeEvent(1000), makeEvent(2000), makeEvent(3000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, batch, 3));

  auto& files = testStorage().files;
  const std::vector<uint8_t> original = files[QUEUE_PATH];
  files[QUEUE_PATH + ".bak"] = original;
  files.erase(QUEUE_PATH);

  testStorage().failRenames = true;  // restore cannot complete

  // Every entry point must refuse rather than proceed.
  const BookOrbitStatEvent fresh[] = {makeEvent(9000)};
  EXPECT_FALSE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, fresh, 1)) << "append must fail closed";
  EXPECT_FALSE(BookOrbitStatsQueue::removeOldest(CACHE, 1)) << "trim must fail closed";
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 0u);
  BookOrbitStatEvent out[BookOrbitStatsQueue::MAX_EVENTS_PER_BATCH];
  size_t scanned = 0;
  EXPECT_EQ(BookOrbitStatsQueue::readOldest(CACHE, OWNER, out, sizeof(out) / sizeof(out[0]), scanned), 0u);

  // And the backup is still byte-for-byte intact, with nothing promoted over it.
  ASSERT_TRUE(Storage.exists((QUEUE_PATH + ".bak").c_str())) << "the only surviving copy was destroyed";
  EXPECT_EQ(files[QUEUE_PATH + ".bak"], original);
  EXPECT_FALSE(Storage.exists(QUEUE_PATH.c_str())) << "no fresh queue may be promoted while the backup is unresolved";

  // Once the card recovers, the next access restores the backup and the events come back.
  testStorage().failRenames = false;
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 3u);
  const auto read = drain();
  ASSERT_EQ(read.size(), 3u);
  EXPECT_EQ(read[0].startTime, 1000u);
  EXPECT_EQ(read[2].startTime, 3000u);
  EXPECT_FALSE(Storage.exists((QUEUE_PATH + ".bak").c_str())) << "and the backup is retired only after it is restored";
}

TEST_F(BookOrbitQueueTest, PromotionRefusesRatherThanClearingABackupItDidNotCreate) {
  // Directly pins the promotion precondition: a .bak present at promotion time can only be an
  // unresolved remnant, so promotion must refuse instead of clearing it. Here the live file
  // still exists, so an append would otherwise happily replace it and drop the backup.
  const BookOrbitStatEvent theirs[] = {makeEvent(1000), makeEvent(2000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OTHER_OWNER, theirs, 2));

  auto& files = testStorage().files;
  files[QUEUE_PATH + ".bak"] = {0xDE, 0xAD, 0xBE, 0xEF};
  const std::vector<uint8_t> backupBefore = files[QUEUE_PATH + ".bak"];
  testStorage().failRemoves = true;  // recovery cannot tidy the stale backup away

  const BookOrbitStatEvent mine[] = {makeEvent(9000)};
  EXPECT_FALSE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, mine, 1))
      << "an owner switch must not promote over an unresolved backup";
  testStorage().failRemoves = false;

  EXPECT_EQ(files[QUEUE_PATH + ".bak"], backupBefore) << "promotion cleared a backup it did not create";
  // The previous owner's queue is also untouched -- the failed switch changed nothing.
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OTHER_OWNER), 2u);
}

// ---- Durable writeback: byte counts are not proof --------------------------------------------

TEST_F(BookOrbitQueueTest, AFailedWritebackDuringAReplacementNeverPromotesOrDropsTheBackup) {
  // Every write in the rewrite can report its full length and the data can still be sitting in a
  // cache that never reaches the card. flush() returns void, so only close() -- whose bool is
  // SdFat's sync() result -- can tell. Promoting on the strength of byte counts alone would
  // replace a complete queue with an incomplete file and then delete the backup behind it.
  const BookOrbitStatEvent batch[] = {makeEvent(1000), makeEvent(2000), makeEvent(3000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, batch, 3));
  const std::vector<uint8_t> original = rawFile();

  testStorage().failFlush = true;
  EXPECT_FALSE(BookOrbitStatsQueue::removeOldest(CACHE, 1)) << "an unsynced replacement must not report success";
  testStorage().failFlush = false;

  EXPECT_EQ(rawFile(), original) << "the live queue was replaced by an unsynced temp";
  EXPECT_FALSE(Storage.exists((QUEUE_PATH + ".tmp").c_str())) << "the unsynced temp must be discarded";
  EXPECT_FALSE(Storage.exists((QUEUE_PATH + ".bak").c_str())) << "promotion must not have run at all";
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 3u) << "no event may be lost";
  EXPECT_EQ(drain().size(), 3u);

  // And once the card syncs again the trim completes normally.
  ASSERT_TRUE(BookOrbitStatsQueue::removeOldest(CACHE, 1));
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 2u);
}

TEST_F(BookOrbitQueueTest, AFailedWritebackCreatingTheFirstQueueReportsFailureAndLeavesNothingBehind) {
  testStorage().failFlush = true;
  const BookOrbitStatEvent batch[] = {makeEvent(1000), makeEvent(2000)};
  EXPECT_FALSE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, batch, 2))
      << "a queue that never synced must not be reported as created";
  testStorage().failFlush = false;

  EXPECT_FALSE(Storage.exists((QUEUE_PATH + ".tmp").c_str()));
  EXPECT_FALSE(Storage.exists((QUEUE_PATH + ".bak").c_str()));
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 0u);

  // The caller kept its RAM batch, so the retry lands everything.
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, batch, 2));
  EXPECT_EQ(drain().size(), 2u);
}

TEST_F(BookOrbitQueueTest, AFailedWritebackDuringAnOwnerSwitchKeepsThePreviousQueue) {
  const BookOrbitStatEvent theirs[] = {makeEvent(1000), makeEvent(2000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OTHER_OWNER, theirs, 2));
  const std::vector<uint8_t> original = rawFile();

  testStorage().failFlush = true;
  const BookOrbitStatEvent mine[] = {makeEvent(9000)};
  EXPECT_FALSE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, mine, 1));
  testStorage().failFlush = false;

  EXPECT_EQ(rawFile(), original) << "a failed switch must leave the previous account's queue untouched";
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OTHER_OWNER), 2u);
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 0u);
}

TEST_F(BookOrbitQueueTest, AFailedWritebackOnAnAppendReportsFailureSoTheBatchIsRetried) {
  // The in-place append path. Whatever reached the card may be a torn tail; what matters is that
  // the caller is told it failed, so it keeps its RAM batch instead of discarding it on the
  // strength of a write() length.
  const BookOrbitStatEvent first[] = {makeEvent(1000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, first, 1));

  testStorage().failFlush = true;
  const BookOrbitStatEvent second[] = {makeEvent(2000), makeEvent(3000)};
  EXPECT_FALSE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, second, 2))
      << "an unsynced append must never report success";
  testStorage().failFlush = false;

  // The original event is still there and the queue is still readable either way.
  const auto afterFailure = drain();
  EXPECT_GE(afterFailure.size(), 1u);
  EXPECT_EQ(afterFailure[0].startTime, 1000u) << "the previously durable event must survive";

  // The retry succeeds and everything is recoverable. Re-appending events that did reach the
  // card is harmless: the server dedups on (user, book, device, page, startTime), and those
  // stamps are written once at capture and never re-stamped.
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, second, 2));
  const auto afterRetry = drain();
  ASSERT_GE(afterRetry.size(), 3u);
  EXPECT_EQ(afterRetry[0].startTime, 1000u);
  bool sawSecond = false, sawThird = false;
  for (const auto& e : afterRetry) {
    if (e.startTime == 2000u) sawSecond = true;
    if (e.startTime == 3000u) sawThird = true;
  }
  EXPECT_TRUE(sawSecond && sawThird) << "the retried events must be present";
}

TEST_F(BookOrbitQueueTest, ATornTailFromAnUnsyncedAppendIsRepairedOnTheNextAttempt) {
  // Models the worst case the append path can leave: some bytes landed, not a whole record.
  const BookOrbitStatEvent first[] = {makeEvent(1000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, first, 1));
  for (int i = 0; i < 5; i++) rawFile().push_back(0xAB);  // partial record from the failed append

  const BookOrbitStatEvent next[] = {makeEvent(2000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, next, 1));

  EXPECT_EQ(rawFile().size(), BookOrbitStatsQueue::HEADER_BYTES + 2 * BookOrbitStatsQueue::RECORD_BYTES);
  const auto read = drain();
  ASSERT_EQ(read.size(), 2u);
  EXPECT_EQ(read[0].startTime, 1000u);
  EXPECT_EQ(read[1].startTime, 2000u);
}

// ---- Transient read failure must never be mistaken for "replaceable" -----------------------------

TEST_F(BookOrbitQueueTest, AValidQueueSurvivesATransientOpenFailure) {
  // A single bool for "could not read it" collapses two very different situations: the file is
  // not ours (safe to replace) and the file could not be opened this time (contents unknown).
  // Treating the second as the first means one SD hiccup replaces a queue full of unsent events
  // with a fresh one.
  const BookOrbitStatEvent batch[] = {makeEvent(1000), makeEvent(2000), makeEvent(3000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, batch, 3));
  const std::vector<uint8_t> original = rawFile();

  testStorage().failOpenRead = true;

  const BookOrbitStatEvent fresh[] = {makeEvent(9000)};
  EXPECT_FALSE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, fresh, 1)) << "append must defer, not replace";
  EXPECT_FALSE(BookOrbitStatsQueue::removeOldest(CACHE, 1)) << "a trim must not delete what it cannot read";
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 0u);
  BookOrbitStatEvent out[BookOrbitStatsQueue::MAX_EVENTS_PER_BATCH];
  size_t scanned = 0;
  EXPECT_EQ(BookOrbitStatsQueue::readOldest(CACHE, OWNER, out, sizeof(out) / sizeof(out[0]), scanned), 0u);
  EXPECT_EQ(scanned, 0u);

  EXPECT_EQ(rawFile(), original) << "the live queue was modified during a read failure";
  EXPECT_FALSE(Storage.exists((QUEUE_PATH + ".tmp").c_str()));
  EXPECT_FALSE(Storage.exists((QUEUE_PATH + ".bak").c_str()));

  // Once the card recovers, everything is still there and the deferred batch appends normally.
  testStorage().failOpenRead = false;
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 3u);
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, fresh, 1));
  const auto read = drain();
  ASSERT_EQ(read.size(), 4u);
  EXPECT_EQ(read[0].startTime, 1000u) << "nothing was lost to the transient failure";
  EXPECT_EQ(read[3].startTime, 9000u);
}

TEST_F(BookOrbitQueueTest, AValidQueueSurvivesAShortHeaderRead) {
  // Same hazard through the other door: the file is long enough to hold a header, but reading it
  // comes up short. That is an I/O failure and says nothing about the contents -- unlike a file
  // genuinely shorter than a header, which is a torn creation and IS replaceable.
  const BookOrbitStatEvent batch[] = {makeEvent(1000), makeEvent(2000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, batch, 2));
  const std::vector<uint8_t> original = rawFile();

  testStorage().failReadAfter = 2;  // header read returns fewer bytes than requested

  const BookOrbitStatEvent fresh[] = {makeEvent(9000)};
  EXPECT_FALSE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, fresh, 1));
  EXPECT_FALSE(BookOrbitStatsQueue::removeOldest(CACHE, 1));
  testStorage().failReadAfter = SIZE_MAX;

  EXPECT_EQ(rawFile(), original);
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 2u) << "the queue must be intact once reads work again";
}

TEST_F(BookOrbitQueueTest, AFileTooShortToHoldAHeaderIsStillReplaceable) {
  // The discrimination that makes the above safe without freezing the feature on genuine
  // garbage: a file shorter than a header has been read successfully and is known to be useless.
  rawFile() = {'B', 'O'};

  const BookOrbitStatEvent fresh[] = {makeEvent(9000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, fresh, 1)) << "a torn stub must not block the feature";
  const auto read = drain();
  ASSERT_EQ(read.size(), 1u);
  EXPECT_EQ(read[0].startTime, 9000u);
}

// ---- Book identity: move and rename lifecycle ---------------------------------------------------

TEST_F(BookOrbitQueueTest, TheQueueLivesOutsideTheRebuildableBookCache) {
  const BookOrbitStatEvent batch[] = {makeEvent(1000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, batch, 1));

  const std::string path = BookOrbitStatsQueue::filePath(CACHE);
  EXPECT_EQ(path.rfind("/.crosspoint/bo_stats/", 0), 0u)
      << "Clear Cache and every cache rebuild delete /.crosspoint/epub_*; unsent events must not live there";
  EXPECT_EQ(path.find("/epub_"), std::string::npos);
}

TEST_F(BookOrbitQueueTest, MovingOrRenamingTheBookKeepsTheQueueWhenItsDocumentIdentityHolds) {
  // The queue is keyed by the KOSync document hash, which is what the server matches on, so
  // ordinary library tidying does not strand un-uploaded events. Under filename matching a move
  // between folders leaves the hash unchanged; under binary matching a rename does too. Either
  // way, "same document identity" must mean "same queue".
  const BookOrbitStatEvent batch[] = {makeEvent(1000), makeEvent(2000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, batch, 2));

  // Nothing about the book's location appears in the key, so the queue is found again by
  // identity alone.
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 2u);
  EXPECT_EQ(BookOrbitStatsQueue::filePath(CACHE), QUEUE_PATH);
}

TEST_F(BookOrbitQueueTest, ADifferentDocumentIdentityIsADifferentQueue) {
  const BookOrbitStatEvent mine[] = {makeEvent(1000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, mine, 1));

  constexpr char OTHER_BOOK[] = "ffffffffffffffffffffffffffffffff";
  EXPECT_NE(BookOrbitStatsQueue::filePath(OTHER_BOOK), BookOrbitStatsQueue::filePath(CACHE));
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(OTHER_BOOK, OWNER), 0u) << "books must not share a queue";
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 1u);
}

TEST_F(BookOrbitQueueTest, AnUnexpectedIdentityStillProducesASafeDeterministicFileName) {
  const std::string path = BookOrbitStatsQueue::filePath("../../etc/passwd");
  EXPECT_EQ(path.find(".."), std::string::npos) << "nothing path-like may reach the filesystem";
  EXPECT_EQ(path, BookOrbitStatsQueue::filePath("../../etc/passwd")) << "and it must stay deterministic";
  EXPECT_EQ(BookOrbitStatsQueue::filePath(""), "/.crosspoint/bo_stats/unknown.bin");
}

// ---- Cross-owner append safety ------------------------------------------------------------------

TEST_F(BookOrbitQueueTest, AFailedOwnerSwitchNeverAppendsUnderTheOldOwnersHeader) {
  // The sharp edge: if retiring the previous account's queue fails but the append proceeds
  // anyway, the NEW account's reading lands inside a file stamped with the OLD account's owner
  // -- and is then uploadable under the wrong credentials. Every failure mode of the switch must
  // fail closed instead.
  const BookOrbitStatEvent theirs[] = {makeEvent(1000), makeEvent(2000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OTHER_OWNER, theirs, 2));
  const std::vector<uint8_t> original = rawFile();

  const BookOrbitStatEvent mine[] = {makeEvent(9000)};

  struct Failure {
    const char* name;
    bool TestStorageState::* flag;
  };
  for (const Failure& failure :
       {Failure{"temp open", &TestStorageState::failOpenWrite}, Failure{"temp write", &TestStorageState::failWrites},
        Failure{"rename", &TestStorageState::failRenames}, Failure{"remove", &TestStorageState::failRemoves}}) {
    testStorage().*(failure.flag) = true;
    const bool appended = BookOrbitStatsQueue::appendBatch(CACHE, OWNER, mine, 1);
    testStorage().*(failure.flag) = false;

    // Whether it reported failure or not, the invariant is the same: our events must never be
    // readable under the other account's header, and their data must not have been destroyed.
    BookOrbitStatEvent out[BookOrbitStatsQueue::MAX_EVENTS_PER_BATCH];
    size_t scanned = 0;
    const size_t theirsVisible =
        BookOrbitStatsQueue::readOldest(CACHE, OTHER_OWNER, out, sizeof(out) / sizeof(out[0]), scanned);
    for (size_t i = 0; i < theirsVisible; i++) {
      EXPECT_NE(out[i].startTime, 9000u) << failure.name << ": our event was filed under their owner";
    }
    if (!appended) {
      EXPECT_EQ(rawFile(), original) << failure.name << ": a failed switch must leave the old queue untouched";
    }
  }
}

TEST_F(BookOrbitQueueTest, ASuccessfulOwnerSwitchPromotesACompleteNewOwnerQueue) {
  const BookOrbitStatEvent theirs[] = {makeEvent(1000), makeEvent(2000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OTHER_OWNER, theirs, 2));

  const BookOrbitStatEvent mine[] = {makeEvent(9000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, mine, 1));

  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 1u);
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OTHER_OWNER), 0u);
  const auto read = drain();
  ASSERT_EQ(read.size(), 1u);
  EXPECT_EQ(read[0].startTime, 9000u);
  EXPECT_FALSE(Storage.exists((QUEUE_PATH + ".bak").c_str())) << "the retired queue must not linger";
  EXPECT_FALSE(Storage.exists((QUEUE_PATH + ".tmp").c_str()));
}

// ---- Owner migration (rewriteOwner) ------------------------------------------------------------
// Exercises the seam BookOrbitMigration.h uses to carry e51b383's opportunistically-detected
// queue across to an explicit BookOrbitCredentialStore owner without losing events -- see
// docs/bookorbit-feature-matrix.md. The credential-copy half of that migration is tested
// separately (test/bookorbit_migration/); this is the part with actual data-loss risk.

TEST_F(BookOrbitQueueTest, RewriteOwnerPreservesEveryQueuedEventUnderTheNewOwner) {
  // The scenario this exists for: a legacy queue captured under the OLD (opportunistic
  // KOReaderCredentialStore-derived) owner must remain fully readable -- and uploadable -- once
  // BookOrbitMigration.h repoints it at the NEW (explicit BookOrbitCredentialStore) owner.
  const BookOrbitStatEvent legacy[] = {makeEvent(1000, 10), makeEvent(2000, 20), makeEvent(3000, 30)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, legacy, 3));

  ASSERT_TRUE(BookOrbitStatsQueue::rewriteOwner(CACHE, OWNER, OTHER_OWNER));

  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 0u) << "no longer readable under the old owner";
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OTHER_OWNER), 3u);
  BookOrbitStatEvent out[BookOrbitStatsQueue::MAX_EVENTS_PER_BATCH];
  size_t scanned = 0;
  const size_t n = BookOrbitStatsQueue::readOldest(CACHE, OTHER_OWNER, out, sizeof(out) / sizeof(out[0]), scanned);
  ASSERT_EQ(n, 3u);
  EXPECT_EQ(out[0].startTime, 1000u);
  EXPECT_EQ(out[1].startTime, 2000u);
  EXPECT_EQ(out[2].startTime, 3000u);
  EXPECT_FALSE(Storage.exists((QUEUE_PATH + ".bak").c_str())) << "the pre-migration queue must not linger";
  EXPECT_FALSE(Storage.exists((QUEUE_PATH + ".tmp").c_str()));
}

TEST_F(BookOrbitQueueTest, RewriteOwnerIsANoOpWhenTheLiveQueueIsAlreadyTheNewOwner) {
  // Idempotent retry: BookOrbitMigration::runOnce() may run this again after an interrupted
  // attempt. A queue already carrying the target owner (from a completed prior attempt, or one
  // that was never the legacy owner to begin with) must be left exactly alone.
  const BookOrbitStatEvent events[] = {makeEvent(5000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OTHER_OWNER, events, 1));
  const std::vector<uint8_t> before = rawFile();

  EXPECT_TRUE(BookOrbitStatsQueue::rewriteOwner(CACHE, OWNER, OTHER_OWNER)) << "OWNER is not the live owner here";
  EXPECT_EQ(rawFile(), before);
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OTHER_OWNER), 1u);
}

TEST_F(BookOrbitQueueTest, RewriteOwnerIsANoOpWithNoLiveQueue) {
  EXPECT_TRUE(BookOrbitStatsQueue::rewriteOwner(CACHE, OWNER, OTHER_OWNER));
  EXPECT_FALSE(Storage.exists(QUEUE_PATH.c_str()));
}

TEST_F(BookOrbitQueueTest, RewriteOwnerToItselfIsANoOpEvenWhenItMatches) {
  const BookOrbitStatEvent events[] = {makeEvent(6000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, events, 1));
  const std::vector<uint8_t> before = rawFile();

  EXPECT_TRUE(BookOrbitStatsQueue::rewriteOwner(CACHE, OWNER, OWNER));
  EXPECT_EQ(rawFile(), before) << "an identical owner must not trigger a rewrite (or a rename count)";
}

TEST_F(BookOrbitQueueTest, AFailedRewriteLeavesTheLegacyOwnedQueueIntact) {
  // Fail-closed requirement: whatever step fails, the legacy queue -- the only copy that still
  // has a valid owner to read it back under -- must survive untouched.
  const BookOrbitStatEvent legacy[] = {makeEvent(1000), makeEvent(2000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, legacy, 2));
  const std::vector<uint8_t> original = rawFile();

  struct Failure {
    const char* name;
    bool TestStorageState::* flag;
  };
  for (const Failure& failure :
       {Failure{"temp open", &TestStorageState::failOpenWrite}, Failure{"temp write", &TestStorageState::failWrites},
        Failure{"rename", &TestStorageState::failRenames}}) {
    testStorage().*(failure.flag) = true;
    const bool ok = BookOrbitStatsQueue::rewriteOwner(CACHE, OWNER, OTHER_OWNER);
    testStorage().*(failure.flag) = false;

    EXPECT_FALSE(ok) << failure.name;
    EXPECT_EQ(rawFile(), original) << failure.name << ": legacy queue must survive a failed migration untouched";
    EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 2u) << failure.name;
  }
}

// ---- Partial pagination -------------------------------------------------------------------------

TEST_F(BookOrbitQueueTest, AFinalizedSectionDividesByItsRealPageCount) {
  // Where pageCount and estimatedTotalPages agree, the result must be exactly what dividing by
  // pageCount always produced -- this change must not move finalized-section numbers.
  float progress = 0.0f;
  ASSERT_TRUE(BookOrbitStatsQueue::chapterProgressAtEndOfPage(4, 10, 10, progress));
  EXPECT_FLOAT_EQ(progress, 0.5f);
  ASSERT_TRUE(BookOrbitStatsQueue::chapterProgressAtEndOfPage(9, 10, 10, progress));
  EXPECT_FLOAT_EQ(progress, 1.0f) << "the last page of a finished chapter is still 100% of it";
}

TEST_F(BookOrbitQueueTest, APartialSectionCannotReportAFinishedChapterFromItsWatermark) {
  // A giant spine still laying out: pageCount is the build watermark, and on its last built page
  // it equals currentPage + 1. Dividing by it would call the chapter finished and push whole-book
  // progress towards 100% while most of the chapter has not even been paginated.
  float progress = 0.0f;
  ASSERT_TRUE(BookOrbitStatsQueue::chapterProgressAtEndOfPage(19, 20, 500, progress));
  EXPECT_FLOAT_EQ(progress, 20.0f / 500.0f);
  EXPECT_LT(progress, 0.05f) << "a barely-started giant chapter must not read as nearly finished";
}

TEST_F(BookOrbitQueueTest, ChapterProgressStaysBoundedForStaleOrAbsurdInputs) {
  float progress = 0.0f;
  // Estimate lags behind the watermark: the larger figure wins, so the fraction stays <= 1.
  ASSERT_TRUE(BookOrbitStatsQueue::chapterProgressAtEndOfPage(30, 40, 10, progress));
  EXPECT_FLOAT_EQ(progress, 31.0f / 40.0f);
  // Both totals stale/zero: the page being finished still proves the chapter has that many.
  ASSERT_TRUE(BookOrbitStatsQueue::chapterProgressAtEndOfPage(7, 0, 0, progress));
  EXPECT_FLOAT_EQ(progress, 1.0f);
  ASSERT_TRUE(BookOrbitStatsQueue::chapterProgressAtEndOfPage(0, 1, 1, progress));
  EXPECT_FLOAT_EQ(progress, 1.0f);
  EXPECT_FALSE(BookOrbitStatsQueue::chapterProgressAtEndOfPage(-1, 10, 10, progress));
}

// ---- Corrupt, truncated and torn input --------------------------------------------------------

TEST_F(BookOrbitQueueTest, AbsentQueueReadsAsEmptyRatherThanFailing) {
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 0u);
  EXPECT_TRUE(drain().empty());
  EXPECT_TRUE(BookOrbitStatsQueue::removeOldest(CACHE, 5)) << "nothing to trim is not an error";
}

TEST_F(BookOrbitQueueTest, FileShorterThanTheHeaderIsSurvivable) {
  rawFile() = {'B', 'O'};
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 0u);
  EXPECT_TRUE(drain().empty());
}

TEST_F(BookOrbitQueueTest, HeaderOnlyFileHoldsNoRecords) {
  uint8_t header[BookOrbitStatsQueue::HEADER_BYTES];
  BookOrbitStatsQueue::encodeHeader(header, OWNER);
  rawFile().assign(header, header + sizeof(header));
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 0u);
  EXPECT_TRUE(drain().empty());
}

TEST_F(BookOrbitQueueTest, ForeignOrOldFormatHeaderIsDiscardedNotReinterpreted) {
  rawFile().assign(BookOrbitStatsQueue::HEADER_BYTES + 4, 0x5A);
  rawFile()[0] = 'X';  // foreign magic
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 0u);
  EXPECT_TRUE(drain().empty());

  // Appending must REPLACE it, not append onto bytes that would then be discarded wholesale --
  // otherwise the new events vanish along with the foreign file.
  const BookOrbitStatEvent fresh[] = {makeEvent(4242)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, fresh, 1));
  const auto read = drain();
  ASSERT_EQ(read.size(), 1u);
  EXPECT_EQ(read[0].startTime, 4242u);
}

TEST_F(BookOrbitQueueTest, TornTrailingPartialRecordIsIgnoredAndGoodRecordsSurvive) {
  const BookOrbitStatEvent batch[] = {makeEvent(1000), makeEvent(2000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, batch, 2));
  // Power loss mid-append: five bytes of a third record made it to the card.
  for (int i = 0; i < 5; i++) rawFile().push_back(0xAB);

  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 2u) << "a partial record is not a record";
  const auto read = drain();
  ASSERT_EQ(read.size(), 2u);
  EXPECT_EQ(read[0].startTime, 1000u);
  EXPECT_EQ(read[1].startTime, 2000u);
}

TEST_F(BookOrbitQueueTest, AppendingAfterATornTailRepairsAlignmentInsteadOfPoisoningTheQueue) {
  // Regression: appending with O_APPEND onto a file whose length is not header + N*RECORD_BYTES
  // starts the new record mid-slot. Reads walk a fixed 12-byte grid, so the partial bytes AND
  // every event appended after them decode as checksum failures -- one interrupted write would
  // silently destroy all subsequent reading history for that book.
  const BookOrbitStatEvent before[] = {makeEvent(1000), makeEvent(2000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, before, 2));
  for (int i = 0; i < 5; i++) rawFile().push_back(0xAB);  // power loss mid-append
  ASSERT_EQ(rawFile().size(), BookOrbitStatsQueue::HEADER_BYTES + 2 * BookOrbitStatsQueue::RECORD_BYTES + 5);

  const BookOrbitStatEvent after[] = {makeEvent(3000), makeEvent(4000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, after, 2));

  EXPECT_EQ(rawFile().size(), BookOrbitStatsQueue::HEADER_BYTES + 4 * BookOrbitStatsQueue::RECORD_BYTES)
      << "the torn bytes must be gone, leaving only whole records";
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 4u);
  const auto read = drain();
  ASSERT_EQ(read.size(), 4u) << "every record, old and new, must still decode";
  EXPECT_EQ(read[0].startTime, 1000u);
  EXPECT_EQ(read[1].startTime, 2000u);
  EXPECT_EQ(read[2].startTime, 3000u);
  EXPECT_EQ(read[3].startTime, 4000u);
}

TEST_F(BookOrbitQueueTest, ATornTailIsRepairedEvenWhenTheAppendAlsoOverflowsTheCap) {
  constexpr size_t CAP = BookOrbitStatsQueue::MAX_QUEUED_EVENTS;
  std::vector<BookOrbitStatEvent> fill;
  for (uint32_t i = 0; i < CAP; i++) fill.push_back(makeEvent(1000 + i));
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, fill.data(), fill.size()));
  for (int i = 0; i < 7; i++) rawFile().push_back(0xCD);

  const BookOrbitStatEvent fresh[] = {makeEvent(9000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, fresh, 1));

  EXPECT_EQ(rawFile().size(), BookOrbitStatsQueue::HEADER_BYTES + CAP * BookOrbitStatsQueue::RECORD_BYTES);
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), CAP);
  ASSERT_TRUE(BookOrbitStatsQueue::removeOldest(CACHE, CAP - 1));
  const auto tail = drain();
  ASSERT_EQ(tail.size(), 1u);
  EXPECT_EQ(tail[0].startTime, 9000u);
}

TEST_F(BookOrbitQueueTest, RepeatedFullBatchesAccumulateSoALongSessionIsNotTruncated) {
  // The reader buffers at most MAX_EVENTS_PER_BATCH events in RAM and flushes here when full, so
  // a session longer than one buffer must keep every event rather than stopping at the first 32.
  constexpr size_t BATCH = BookOrbitStatsQueue::MAX_EVENTS_PER_BATCH;
  for (uint32_t round = 0; round < 4; round++) {
    std::vector<BookOrbitStatEvent> batch;
    for (uint32_t i = 0; i < BATCH; i++) batch.push_back(makeEvent(1000 + round * BATCH + i));
    ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, batch.data(), batch.size()));
  }
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 4 * BATCH);

  const auto head = drain(1);
  ASSERT_EQ(head.size(), 1u);
  EXPECT_EQ(head[0].startTime, 1000u) << "the first page of the session must survive";
  ASSERT_TRUE(BookOrbitStatsQueue::removeOldest(CACHE, 4 * BATCH - 1));
  const auto tail = drain();
  ASSERT_EQ(tail.size(), 1u);
  EXPECT_EQ(tail[0].startTime, 1000u + 4 * BATCH - 1) << "and so must the last";
}

TEST_F(BookOrbitQueueTest, ACorruptRecordIsSkippedWhileItsNeighboursStillUpload) {
  const BookOrbitStatEvent batch[] = {makeEvent(1000), makeEvent(2000), makeEvent(3000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, batch, 3));
  // Garble the middle record in place.
  rawFile()[BookOrbitStatsQueue::HEADER_BYTES + BookOrbitStatsQueue::RECORD_BYTES + 1] ^= 0xFF;

  std::vector<BookOrbitStatEvent> out(BookOrbitStatsQueue::MAX_EVENTS_PER_BATCH);
  size_t scanned = 0;
  const size_t n = BookOrbitStatsQueue::readOldest(CACHE, OWNER, out.data(), out.size(), scanned);
  ASSERT_EQ(n, 2u) << "the corrupt record must be skipped, not returned";
  EXPECT_EQ(out[0].startTime, 1000u);
  EXPECT_EQ(out[1].startTime, 3000u);
  EXPECT_EQ(scanned, 3u) << "scanned must count the skipped record so removal covers it";

  // Retiring the batch by `scanned` clears the corrupt slot too; using `n` would strand it at
  // the head forever and shift every later removal onto the wrong records.
  ASSERT_TRUE(BookOrbitStatsQueue::removeOldest(CACHE, scanned));
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 0u);
}

TEST_F(BookOrbitQueueTest, ReadOldestScanningIsBoundedByTheRequestedBatchSize) {
  std::vector<BookOrbitStatEvent> events;
  for (uint32_t i = 0; i < 20; i++) events.push_back(makeEvent(1000 + i));
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, events.data(), events.size()));

  BookOrbitStatEvent out[4];
  size_t scanned = 0;
  const size_t n = BookOrbitStatsQueue::readOldest(CACHE, OWNER, out, 4, scanned);
  EXPECT_EQ(n, 4u);
  EXPECT_EQ(scanned, 4u) << "one corrupt record must never make this walk the whole file";
  EXPECT_EQ(out[0].startTime, 1000u);
}

// ---- Retry semantics --------------------------------------------------------------------------

TEST_F(BookOrbitQueueTest, RetryResendsByteIdenticalTimestampsSoServerDedupWorks) {
  // The server dedups on (user, book, device, page, startTime). A failed upload must leave the
  // queue exactly as it was, so the retry carries the SAME stamps rather than fresh ones.
  const BookOrbitStatEvent batch[] = {makeEvent(1893456000u, 4200, 61), makeEvent(1893456100u, 4300, 44)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, batch, 2));

  const auto firstAttempt = drain();
  const auto secondAttempt = drain();  // nothing removed in between: the upload "failed"
  ASSERT_EQ(firstAttempt.size(), 2u);
  ASSERT_EQ(secondAttempt.size(), 2u);
  for (size_t i = 0; i < 2; i++) {
    EXPECT_EQ(firstAttempt[i].startTime, secondAttempt[i].startTime);
    EXPECT_EQ(firstAttempt[i].page, secondAttempt[i].page);
    EXPECT_EQ(firstAttempt[i].durationSeconds, secondAttempt[i].durationSeconds);
  }
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 2u) << "a read must never consume the queue";
}

TEST_F(BookOrbitQueueTest, StalenessBoundRejectsAncientBacklogButKeepsRecentEvents) {
  const uint32_t now = 1893456000u;
  const BookOrbitStatEvent recent = makeEvent(now - 60u);
  const BookOrbitStatEvent ancient = makeEvent(now - BookOrbitStatsQueue::MAX_EVENT_AGE_SECONDS - 60u);

  EXPECT_TRUE(BookOrbitStatsQueue::isUploadable(recent, now));
  EXPECT_FALSE(BookOrbitStatsQueue::isUploadable(ancient, now));
  // No clock available: the age check is skipped rather than discarding real history.
  EXPECT_TRUE(BookOrbitStatsQueue::isUploadable(ancient, 0));
}

// ---- Storage failure ---------------------------------------------------------------------------

TEST_F(BookOrbitQueueTest, AppendReportsFailureWhenTheCardCannotBeWritten) {
  testStorage().failOpenWrite = true;
  const BookOrbitStatEvent batch[] = {makeEvent(1000)};
  EXPECT_FALSE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, batch, 1));
}

TEST_F(BookOrbitQueueTest, AFailedRewriteLeavesTheLiveQueueIntact) {
  const BookOrbitStatEvent batch[] = {makeEvent(1000), makeEvent(2000), makeEvent(3000)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(CACHE, OWNER, batch, 3));

  testStorage().failWrites = true;
  EXPECT_FALSE(BookOrbitStatsQueue::removeOldest(CACHE, 1));
  testStorage().failWrites = false;

  // The temp-file-then-rename discipline means a failed rewrite must not have touched the
  // original: all three events are still queued and still readable.
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(CACHE, OWNER), 3u);
  EXPECT_EQ(drain().size(), 3u);
  EXPECT_FALSE(Storage.exists((QUEUE_PATH + ".tmp").c_str())) << "a failed rewrite must clean up after itself";
}

}  // namespace
