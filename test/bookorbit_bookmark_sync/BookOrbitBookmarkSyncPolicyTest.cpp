// Tests for the REAL src/util/BookOrbitBookmarkSyncPolicy.cpp -- the pure decision logic behind
// BookOrbit two-way bookmark sync (identity minting, outgoing batch construction, incoming
// apply/ack). No I/O anywhere in these functions, so no HalStorage/HalClock/network stub is
// needed; every test constructs plain BookmarkEntry/BookOrbit* structs directly.

#include <BookOrbitBookmarkSyncPolicy.h>
#include <gtest/gtest.h>

namespace {

using namespace BookOrbitBookmarkSync;

constexpr uint32_t SOME_EPOCH_SECONDS = 1893456000u;  // 2030-01-01T00:00:00Z, arbitrary but fixed

BookmarkEntry makeBookmark(const std::string& xpath, const std::string& boDatetime = "", bool boSynced = false,
                           const std::string& boOwner = "") {
  BookmarkEntry b;
  b.xpath = xpath;
  b.percentage = 0.5f;
  b.boDatetime = boDatetime;
  b.boSynced = boSynced;
  b.boOwner = boOwner;
  return b;
}

constexpr char OWNER_A[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr char OWNER_B[] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

// ---- loadFailureRequiresAbort ------------------------------------------------------------------

TEST(LoadFailureRequiresAbortTest, FailedLoadWithoutConfirmedAbsenceRequiresAbort) {
  EXPECT_TRUE(loadFailureRequiresAbort(/*loadOk=*/false, /*absenceConfirmed=*/false));
}

TEST(LoadFailureRequiresAbortTest, NoFileAtAllIsALegitimateEmptySetNotAnAbort) {
  EXPECT_FALSE(loadFailureRequiresAbort(/*loadOk=*/false, /*absenceConfirmed=*/true));
}

TEST(LoadFailureRequiresAbortTest, ASuccessfulLoadNeverAbortsRegardlessOfAbsenceProof) {
  EXPECT_FALSE(loadFailureRequiresAbort(/*loadOk=*/true, /*absenceConfirmed=*/true));
  EXPECT_FALSE(loadFailureRequiresAbort(/*loadOk=*/true, /*absenceConfirmed=*/false));
}

// ---- mintMissingDatetimes ---------------------------------------------------------------------

TEST(MintMissingDatetimesTest, MintsEveryUnmintedEntryWithTheSameSharedNow) {
  std::vector<BookmarkEntry> bookmarks = {makeBookmark("/a"), makeBookmark("/b")};
  bool anyUnminted = true;
  const bool mintedAny = mintMissingDatetimes(bookmarks, /*clockAvailable=*/true, SOME_EPOCH_SECONDS, anyUnminted);
  EXPECT_TRUE(mintedAny);
  EXPECT_FALSE(anyUnminted);
  ASSERT_FALSE(bookmarks[0].boDatetime.empty());
  EXPECT_EQ(bookmarks[0].boDatetime, bookmarks[1].boDatetime) << "one shared clock read per pass";
  EXPECT_FALSE(bookmarks[0].boSynced) << "a freshly minted identity has never been offered yet";
}

TEST(MintMissingDatetimesTest, NeverTouchesAnAlreadyMintedEntry) {
  std::vector<BookmarkEntry> bookmarks = {makeBookmark("/a", "2020-01-01 00:00:00", true)};
  bool anyUnminted = false;
  const bool mintedAny = mintMissingDatetimes(bookmarks, true, SOME_EPOCH_SECONDS, anyUnminted);
  EXPECT_FALSE(mintedAny);
  EXPECT_EQ(bookmarks[0].boDatetime, "2020-01-01 00:00:00") << "an identity must never change once minted";
  EXPECT_TRUE(bookmarks[0].boSynced) << "minting must not disturb an unrelated field";
}

TEST(MintMissingDatetimesTest, NoClockMintsNothingAndReportsEveryUnmintedEntry) {
  std::vector<BookmarkEntry> bookmarks = {makeBookmark("/a"), makeBookmark("/b", "2020-01-01 00:00:00")};
  bool anyUnminted = false;
  const bool mintedAny = mintMissingDatetimes(bookmarks, /*clockAvailable=*/false, SOME_EPOCH_SECONDS, anyUnminted);
  EXPECT_FALSE(mintedAny);
  EXPECT_TRUE(anyUnminted);
  EXPECT_TRUE(bookmarks[0].boDatetime.empty());
  EXPECT_EQ(bookmarks[1].boDatetime, "2020-01-01 00:00:00") << "already-minted entries are untouched either way";
}

TEST(MintMissingDatetimesTest, EmptyListMintsNothingAndReportsComplete) {
  std::vector<BookmarkEntry> bookmarks;
  bool anyUnminted = true;
  EXPECT_FALSE(mintMissingDatetimes(bookmarks, true, SOME_EPOCH_SECONDS, anyUnminted));
  EXPECT_FALSE(anyUnminted);
}

// ---- buildOutgoing ------------------------------------------------------------------------

TEST(BuildOutgoingTest, KeysCoverEveryMintedEntryRegardlessOfSyncedState) {
  std::vector<BookmarkEntry> bookmarks = {
      makeBookmark("/a", "2020-01-01 00:00:00", /*boSynced=*/true),
      makeBookmark("/b", "2020-01-01 00:00:01", /*boSynced=*/false),
  };
  std::vector<BookOrbitBookmarkKeyRow> keys;
  bool keysComplete = false;
  std::vector<BookOrbitBookmark> changes;
  std::vector<size_t> changeIndices;
  buildOutgoing(bookmarks, /*anyUnminted=*/false, 500, 16, /*currentOwnerHex=*/"", keys, keysComplete, changes,
                changeIndices);

  EXPECT_TRUE(keysComplete);
  ASSERT_EQ(keys.size(), 2u) << "both minted entries contribute a key regardless of sync state";
  EXPECT_STREQ(keys[0].dt, "2020-01-01 00:00:00");
}

TEST(BuildOutgoingTest, ChangesCoverOnlyUnsyncedMintedEntries) {
  std::vector<BookmarkEntry> bookmarks = {
      makeBookmark("/already-synced", "2020-01-01 00:00:00", /*boSynced=*/true),
      makeBookmark("/needs-upload", "2020-01-01 00:00:01", /*boSynced=*/false),
      makeBookmark("/not-minted-yet"),  // boDatetime empty -- excluded from both
  };
  std::vector<BookOrbitBookmarkKeyRow> keys;
  bool keysComplete = false;
  std::vector<BookOrbitBookmark> changes;
  std::vector<size_t> changeIndices;
  buildOutgoing(bookmarks, /*anyUnminted=*/true, 500, 16, /*currentOwnerHex=*/"", keys, keysComplete, changes,
                changeIndices);

  ASSERT_EQ(changes.size(), 1u) << "an already-synced identity is never resent";
  EXPECT_EQ(changes[0].pos, "/needs-upload");
  ASSERT_EQ(changeIndices.size(), 1u);
  EXPECT_EQ(changeIndices[0], 1u) << "must name the bookmarks[] slot that produced this change";
  EXPECT_EQ(keys.size(), 2u) << "the unminted entry is excluded from keys too";
  EXPECT_FALSE(keysComplete) << "anyUnminted=true must always propagate";
}

TEST(BuildOutgoingTest, KeyCapTruncatesAndMarksIncomplete) {
  std::vector<BookmarkEntry> bookmarks;
  for (int i = 0; i < 5; i++) {
    bookmarks.push_back(makeBookmark("/p" + std::to_string(i), "2020-01-01 00:00:0" + std::to_string(i)));
  }
  std::vector<BookOrbitBookmarkKeyRow> keys;
  bool keysComplete = true;
  std::vector<BookOrbitBookmark> changes;
  std::vector<size_t> changeIndices;
  buildOutgoing(bookmarks, /*anyUnminted=*/false, /*maxKeys=*/3, 16, /*currentOwnerHex=*/"", keys, keysComplete,
                changes, changeIndices);

  EXPECT_EQ(keys.size(), 3u) << "must never exceed the caller's cap";
  EXPECT_FALSE(keysComplete) << "a truncated set must never be reported as complete";
}

TEST(BuildOutgoingTest, ChangeCapSpreadsAcrossFurtherSyncRounds) {
  std::vector<BookmarkEntry> bookmarks;
  for (int i = 0; i < 5; i++) {
    bookmarks.push_back(makeBookmark("/p" + std::to_string(i), "2020-01-01 00:00:0" + std::to_string(i)));
  }
  std::vector<BookOrbitBookmarkKeyRow> keys;
  bool keysComplete = false;
  std::vector<BookOrbitBookmark> changes;
  std::vector<size_t> changeIndices;
  buildOutgoing(bookmarks, false, 500, /*maxChanges=*/2, /*currentOwnerHex=*/"", keys, keysComplete, changes,
                changeIndices);

  EXPECT_EQ(changes.size(), 2u) << "must never exceed the caller's cap";
  EXPECT_EQ(keys.size(), 5u) << "the key set itself is not limited by the change cap";
  EXPECT_TRUE(keysComplete);
}

TEST(BuildOutgoingTest, AMintedEntryWithAnEmptyXpathIsExcludedAndMarksIncomplete) {
  // A defensive edge case: boDatetime set but xpath empty (e.g. corrupted data) must not silently
  // vanish from the key set as if it were a clean "not yet minted" exclusion.
  std::vector<BookmarkEntry> bookmarks = {makeBookmark("", "2020-01-01 00:00:00")};
  std::vector<BookOrbitBookmarkKeyRow> keys;
  bool keysComplete = true;
  std::vector<BookOrbitBookmark> changes;
  std::vector<size_t> changeIndices;
  buildOutgoing(bookmarks, false, 500, 16, /*currentOwnerHex=*/"", keys, keysComplete, changes, changeIndices);

  EXPECT_TRUE(keys.empty());
  EXPECT_TRUE(changes.empty()) << "an empty xpath can never be sent";
  EXPECT_FALSE(keysComplete);
}

TEST(BuildOutgoingTest, PagenoIsOneBasedFromComputedChapterProgressWhenAvailable) {
  BookmarkEntry b = makeBookmark("/a", "2020-01-01 00:00:00");
  b.computedChapterPageCount = 10;
  b.computedChapterProgress = 3;
  std::vector<BookmarkEntry> bookmarks = {b};
  std::vector<BookOrbitBookmarkKeyRow> keys;
  bool keysComplete = false;
  std::vector<BookOrbitBookmark> changes;
  std::vector<size_t> changeIndices;
  buildOutgoing(bookmarks, false, 500, 16, /*currentOwnerHex=*/"", keys, keysComplete, changes, changeIndices);

  ASSERT_EQ(changes.size(), 1u);
  EXPECT_EQ(changes[0].pageno, 4);
}

TEST(BuildOutgoingTest, PagenoIsZeroWhenChapterPageCountUnknown) {
  std::vector<BookmarkEntry> bookmarks = {makeBookmark("/a", "2020-01-01 00:00:00")};
  std::vector<BookOrbitBookmarkKeyRow> keys;
  bool keysComplete = false;
  std::vector<BookOrbitBookmark> changes;
  std::vector<size_t> changeIndices;
  buildOutgoing(bookmarks, false, 500, 16, /*currentOwnerHex=*/"", keys, keysComplete, changes, changeIndices);

  ASSERT_EQ(changes.size(), 1u);
  EXPECT_EQ(changes[0].pageno, 0);
}

TEST(BuildOutgoingTest, AnEntrySyncedToADifferentOwnerIsResentToTheCurrentOne) {
  // Regression: boSynced=true alone used to be enough to skip resending -- switching BookOrbit
  // account never clears it, so a bookmark already synced to a PREVIOUS account would be silently
  // skipped forever once the device points at a new one.
  std::vector<BookmarkEntry> bookmarks = {makeBookmark("/a", "2020-01-01 00:00:00", /*boSynced=*/true, OWNER_A)};
  std::vector<BookOrbitBookmarkKeyRow> keys;
  bool keysComplete = false;
  std::vector<BookOrbitBookmark> changes;
  std::vector<size_t> changeIndices;
  buildOutgoing(bookmarks, false, 500, 16, OWNER_B, keys, keysComplete, changes, changeIndices);

  ASSERT_EQ(changes.size(), 1u) << "must be resent -- it was never confirmed synced to OWNER_B";
  EXPECT_EQ(changeIndices[0], 0u);
}

TEST(BuildOutgoingTest, AnEntryAlreadySyncedToTheCurrentOwnerIsNotResent) {
  std::vector<BookmarkEntry> bookmarks = {makeBookmark("/a", "2020-01-01 00:00:00", /*boSynced=*/true, OWNER_A)};
  std::vector<BookOrbitBookmarkKeyRow> keys;
  bool keysComplete = false;
  std::vector<BookOrbitBookmark> changes;
  std::vector<size_t> changeIndices;
  buildOutgoing(bookmarks, false, 500, 16, OWNER_A, keys, keysComplete, changes, changeIndices);

  EXPECT_TRUE(changes.empty());
}

// ---- applyIncoming ------------------------------------------------------------------------

BookOrbitIncomingBookmark makeIncomingAdd(uint32_t serverId, const std::string& pos, const std::string& title = "") {
  BookOrbitIncomingBookmark in;
  in.serverId = serverId;
  in.pos = pos;
  in.title = title;
  in.deleted = false;
  return in;
}

BookOrbitIncomingBookmark makeIncomingDelete(uint32_t serverId, const std::string& key) {
  BookOrbitIncomingBookmark in;
  in.serverId = serverId;
  in.key = key;
  in.deleted = true;
  return in;
}

TEST(ApplyIncomingTest, ANewAddCreatesALocalBookmarkAndAcksWithTheLocalIdentity) {
  std::vector<BookmarkEntry> bookmarks;
  std::vector<BookOrbitIncomingBookmark> incoming = {makeIncomingAdd(42, "/body/DocFragment[1]", "Chapter One")};
  std::vector<BookOrbitBookmarkAck> applied;
  std::vector<uint32_t> deletedIds;
  bool changed = false;
  applyIncoming(bookmarks, SOME_EPOCH_SECONDS, /*currentOwnerHex=*/"", incoming, applied, deletedIds, changed);

  EXPECT_TRUE(changed);
  ASSERT_EQ(bookmarks.size(), 1u);
  EXPECT_EQ(bookmarks[0].xpath, "/body/DocFragment[1]");
  EXPECT_TRUE(bookmarks[0].boSynced) << "an incoming entry is already known to the server";
  EXPECT_FALSE(bookmarks[0].boDatetime.empty()) << "the DEVICE must mint the identity, not the server";

  ASSERT_EQ(applied.size(), 1u);
  EXPECT_EQ(applied[0].serverId, 42u);
  EXPECT_FALSE(applied[0].failed);
  EXPECT_EQ(applied[0].datetime, bookmarks[0].boDatetime) << "ack must echo the identity just minted, not any other";
  EXPECT_EQ(applied[0].pos, "/body/DocFragment[1]");
  EXPECT_FALSE(applied[0].key.empty());
  EXPECT_TRUE(deletedIds.empty());
}

TEST(ApplyIncomingTest, AnAddWithNoUsableClockIsLeftOutOfBothTheListAndTheAcknowledgment) {
  std::vector<BookmarkEntry> bookmarks;
  std::vector<BookOrbitIncomingBookmark> incoming = {makeIncomingAdd(42, "/x")};
  std::vector<BookOrbitBookmarkAck> applied;
  std::vector<uint32_t> deletedIds;
  bool changed = false;
  applyIncoming(bookmarks, /*nowEpochSecondsOrZero=*/0, /*currentOwnerHex=*/"", incoming, applied, deletedIds, changed);

  EXPECT_FALSE(changed);
  EXPECT_TRUE(bookmarks.empty());
  EXPECT_TRUE(applied.empty()) << "neither applied nor failed -- must be re-offered once a clock exists";
}

TEST(ApplyIncomingTest, AnAddWithAnEmptyPositionFailsImmediatelyEvenWithNoClock) {
  std::vector<BookmarkEntry> bookmarks;
  std::vector<BookOrbitIncomingBookmark> incoming = {makeIncomingAdd(42, "")};
  std::vector<BookOrbitBookmarkAck> applied;
  std::vector<uint32_t> deletedIds;
  bool changed = false;
  applyIncoming(bookmarks, /*nowEpochSecondsOrZero=*/0, /*currentOwnerHex=*/"", incoming, applied, deletedIds, changed);

  EXPECT_FALSE(changed);
  EXPECT_TRUE(bookmarks.empty());
  ASSERT_EQ(applied.size(), 1u);
  EXPECT_TRUE(applied[0].failed) << "an unplaceable position is a permanent failure, not a retry case";
}

TEST(ApplyIncomingTest, ReapplyingTheSameAddIsIdempotentAndDoesNotDuplicate) {
  // Simulates a lost ack: the server re-offers the same add on a later round after it was
  // already applied here.
  std::vector<BookmarkEntry> bookmarks;
  std::vector<BookOrbitIncomingBookmark> firstRound = {makeIncomingAdd(42, "/x")};
  std::vector<BookOrbitBookmarkAck> applied1;
  std::vector<uint32_t> deletedIds1;
  bool changed1 = false;
  applyIncoming(bookmarks, SOME_EPOCH_SECONDS, /*currentOwnerHex=*/"", firstRound, applied1, deletedIds1, changed1);
  ASSERT_EQ(bookmarks.size(), 1u);
  const std::string firstKey = applied1[0].key;

  std::vector<BookOrbitIncomingBookmark> secondRound = {makeIncomingAdd(42, "/x")};
  secondRound[0].key = firstKey;  // the server re-sends the SAME add, still pending its own ack
  std::vector<BookOrbitBookmarkAck> applied2;
  std::vector<uint32_t> deletedIds2;
  bool changed2 = false;
  applyIncoming(bookmarks, SOME_EPOCH_SECONDS + 60, /*currentOwnerHex=*/"", secondRound, applied2, deletedIds2,
                changed2);

  EXPECT_FALSE(changed2) << "an already-present identity must not be duplicated";
  EXPECT_EQ(bookmarks.size(), 1u) << "still exactly one local bookmark";
  ASSERT_EQ(applied2.size(), 1u) << "still re-acked, just not re-applied";
  EXPECT_EQ(applied2[0].key, firstKey);
}

TEST(ApplyIncomingTest, ALostAckReofferWithNoKeyStillMatchesByPositionRatherThanDuplicating) {
  // The realistic lost-ack scenario (unlike ReapplyingTheSameAddIsIdempotentAndDoesNotDuplicate
  // above, which hands the second round a key the server could not actually have supplied): per
  // BookOrbitBookmarks.h's own class comment, the server has no identity key for a bookmark it
  // created until THIS device's ack lands, so a lost ack causes the server to re-offer the exact
  // same add with `key` still EMPTY -- never a key that would simply fail to match.
  std::vector<BookmarkEntry> bookmarks;
  std::vector<BookOrbitBookmarkAck> applied1;
  std::vector<uint32_t> deletedIds1;
  bool changed1 = false;
  applyIncoming(bookmarks, SOME_EPOCH_SECONDS, /*currentOwnerHex=*/"", {makeIncomingAdd(42, "/x")}, applied1,
                deletedIds1, changed1);
  ASSERT_EQ(bookmarks.size(), 1u);
  const std::string firstDatetime = bookmarks[0].boDatetime;

  // Ack never landed; the server re-offers the identical pending add, still with no key.
  std::vector<BookOrbitBookmarkAck> applied2;
  std::vector<uint32_t> deletedIds2;
  bool changed2 = false;
  applyIncoming(bookmarks, SOME_EPOCH_SECONDS + 60, /*currentOwnerHex=*/"", {makeIncomingAdd(42, "/x")}, applied2,
                deletedIds2, changed2);

  EXPECT_FALSE(changed2) << "must recognize the position it already applied, not mint a new identity";
  ASSERT_EQ(bookmarks.size(), 1u) << "must not duplicate the bookmark";
  EXPECT_EQ(bookmarks[0].boDatetime, firstDatetime) << "the original identity must be preserved, not re-minted";
  ASSERT_EQ(applied2.size(), 1u) << "still re-acked, using the original identity";
  EXPECT_EQ(applied2[0].pos, "/x");
}

TEST(ApplyIncomingTest, ADeleteRemovesTheMatchingLocalBookmarkByKey) {
  std::vector<BookmarkEntry> bookmarks = {makeBookmark("/a", "2020-01-01 00:00:00")};
  char key[BookOrbitBookmarkKeyRow::DIGEST_SIZE];
  ASSERT_TRUE(bookOrbitBookmarkKey("2020-01-01 00:00:00", "/a", key));

  std::vector<BookOrbitIncomingBookmark> incoming = {makeIncomingDelete(7, key)};
  std::vector<BookOrbitBookmarkAck> applied;
  std::vector<uint32_t> deletedIds;
  bool changed = false;
  applyIncoming(bookmarks, SOME_EPOCH_SECONDS, /*currentOwnerHex=*/"", incoming, applied, deletedIds, changed);

  EXPECT_TRUE(changed);
  EXPECT_TRUE(bookmarks.empty());
  ASSERT_EQ(deletedIds.size(), 1u);
  EXPECT_EQ(deletedIds[0], 7u);
  EXPECT_TRUE(applied.empty());
}

TEST(ApplyIncomingTest, ReapplyingTheSameDeleteAfterItAlreadyHappenedStillAcksWithoutResurrection) {
  // The core "no resurrected deletions" property: a delete the device already applied (but never
  // acked) must be re-acked cleanly on retry, and must NEVER bring the bookmark back.
  std::vector<BookmarkEntry> bookmarks = {makeBookmark("/a", "2020-01-01 00:00:00")};
  char key[BookOrbitBookmarkKeyRow::DIGEST_SIZE];
  ASSERT_TRUE(bookOrbitBookmarkKey("2020-01-01 00:00:00", "/a", key));

  std::vector<BookOrbitBookmarkAck> applied1;
  std::vector<uint32_t> deletedIds1;
  bool changed1 = false;
  applyIncoming(bookmarks, SOME_EPOCH_SECONDS, /*currentOwnerHex=*/"", {makeIncomingDelete(7, key)}, applied1,
                deletedIds1, changed1);
  ASSERT_TRUE(bookmarks.empty());
  ASSERT_EQ(deletedIds1.size(), 1u);

  // Ack never landed; the server re-offers the identical pending deletion.
  std::vector<BookOrbitBookmarkAck> applied2;
  std::vector<uint32_t> deletedIds2;
  bool changed2 = false;
  applyIncoming(bookmarks, SOME_EPOCH_SECONDS + 60, /*currentOwnerHex=*/"", {makeIncomingDelete(7, key)}, applied2,
                deletedIds2, changed2);

  EXPECT_FALSE(changed2) << "nothing local left to change -- the bookmark must stay gone";
  EXPECT_TRUE(bookmarks.empty()) << "must never resurrect an already-applied deletion";
  ASSERT_EQ(deletedIds2.size(), 1u) << "still re-acked so the server can retire it";
  EXPECT_EQ(deletedIds2[0], 7u);
}

TEST(ApplyIncomingTest, MixedAddsAndDeletesInOneRoundAreAllHandled) {
  std::vector<BookmarkEntry> bookmarks = {makeBookmark("/keep-me", "2020-01-01 00:00:00"),
                                          makeBookmark("/delete-me", "2020-01-01 00:00:01")};
  char deleteKey[BookOrbitBookmarkKeyRow::DIGEST_SIZE];
  ASSERT_TRUE(bookOrbitBookmarkKey("2020-01-01 00:00:01", "/delete-me", deleteKey));

  std::vector<BookOrbitIncomingBookmark> incoming = {makeIncomingAdd(100, "/new-from-web"),
                                                     makeIncomingDelete(101, deleteKey)};
  std::vector<BookOrbitBookmarkAck> applied;
  std::vector<uint32_t> deletedIds;
  bool changed = false;
  applyIncoming(bookmarks, SOME_EPOCH_SECONDS, /*currentOwnerHex=*/"", incoming, applied, deletedIds, changed);

  EXPECT_TRUE(changed);
  ASSERT_EQ(bookmarks.size(), 2u) << "kept + newly added, delete-me removed";
  bool sawKeep = false, sawNew = false;
  for (const auto& b : bookmarks) {
    if (b.xpath == "/keep-me") sawKeep = true;
    if (b.xpath == "/new-from-web") sawNew = true;
  }
  EXPECT_TRUE(sawKeep);
  EXPECT_TRUE(sawNew);
  EXPECT_EQ(applied.size(), 1u);
  EXPECT_EQ(deletedIds.size(), 1u);
}

TEST(ApplyIncomingTest, AnIncomingDeleteFromTheCurrentOwnerNeverRemovesAForeignOwnedEntry) {
  // Regression: a delete's key match used to ignore ownership entirely. A coincidental key
  // collision (astronomically unlikely for a real digest, but cheap to guard) between the current
  // account and a PREVIOUS one's bookmark must never let the current account delete it.
  std::vector<BookmarkEntry> bookmarks = {makeBookmark("/a", "2020-01-01 00:00:00", /*boSynced=*/true, OWNER_A)};
  char key[BookOrbitBookmarkKeyRow::DIGEST_SIZE];
  ASSERT_TRUE(bookOrbitBookmarkKey("2020-01-01 00:00:00", "/a", key));

  std::vector<BookOrbitBookmarkAck> applied;
  std::vector<uint32_t> deletedIds;
  bool changed = false;
  applyIncoming(bookmarks, SOME_EPOCH_SECONDS, OWNER_B, {makeIncomingDelete(7, key)}, applied, deletedIds, changed);

  EXPECT_FALSE(changed) << "must not touch an entry owned by a different account";
  ASSERT_EQ(bookmarks.size(), 1u);
  EXPECT_EQ(bookmarks[0].boOwner, OWNER_A);
  ASSERT_EQ(deletedIds.size(), 1u) << "still acked -- the server's delete is not itself wrong, just inapplicable here";
}

TEST(ApplyIncomingTest, AnIncomingAddFromTheCurrentOwnerNeverReownsAForeignOwnedEntryAtTheSamePosition) {
  // Regression: the position-fallback match (the lost-ack-reoffer fix) used to ignore ownership
  // too. Switching account and then receiving an add that happens to land on the same xpath as a
  // PREVIOUS account's bookmark must create a genuinely new entry, not silently re-own the old one.
  std::vector<BookmarkEntry> bookmarks = {makeBookmark("/x", "2020-01-01 00:00:00", /*boSynced=*/true, OWNER_A)};

  std::vector<BookOrbitBookmarkAck> applied;
  std::vector<uint32_t> deletedIds;
  bool changed = false;
  applyIncoming(bookmarks, SOME_EPOCH_SECONDS, OWNER_B, {makeIncomingAdd(42, "/x")}, applied, deletedIds, changed);

  EXPECT_TRUE(changed);
  ASSERT_EQ(bookmarks.size(), 2u) << "the OWNER_A entry must be left alone; a distinct new one is created for OWNER_B";
  EXPECT_EQ(bookmarks[0].boOwner, OWNER_A);
  EXPECT_EQ(bookmarks[1].boOwner, OWNER_B);
  ASSERT_EQ(applied.size(), 1u);
  EXPECT_FALSE(applied[0].failed);
}

TEST(ApplyIncomingTest, ReapplyingAnAddAdoptsAPreOwnerFieldEntryIntoTheConfirmingAccount) {
  // A bookmark synced before this field existed (boOwner empty) is freely adoptable by whichever
  // account confirms it first -- this is not a foreign-owned entry, just an unlabeled one.
  std::vector<BookmarkEntry> bookmarks = {makeBookmark("/x", "2020-01-01 00:00:00", /*boSynced=*/true, /*boOwner=*/"")};

  std::vector<BookOrbitBookmarkAck> applied;
  std::vector<uint32_t> deletedIds;
  bool changed = false;
  applyIncoming(bookmarks, SOME_EPOCH_SECONDS, OWNER_A, {makeIncomingAdd(42, "/x")}, applied, deletedIds, changed);

  EXPECT_TRUE(changed) << "adopting the entry's owner is itself a change worth persisting";
  ASSERT_EQ(bookmarks.size(), 1u) << "matched the existing entry rather than creating a duplicate";
  EXPECT_EQ(bookmarks[0].boOwner, OWNER_A);
}

TEST(ApplyIncomingTest, EmptyIncomingListChangesNothing) {
  std::vector<BookmarkEntry> bookmarks = {makeBookmark("/a", "2020-01-01 00:00:00")};
  std::vector<BookOrbitBookmarkAck> applied;
  std::vector<uint32_t> deletedIds;
  bool changed = true;
  applyIncoming(bookmarks, SOME_EPOCH_SECONDS, /*currentOwnerHex=*/"", {}, applied, deletedIds, changed);
  EXPECT_FALSE(changed);
  EXPECT_EQ(bookmarks.size(), 1u);
  EXPECT_TRUE(applied.empty());
  EXPECT_TRUE(deletedIds.empty());
}

// ---- BookOrbitBookmarks.h/.cpp: datetime formatting and identity key --------------------------

TEST(BookOrbitBookmarksTest, FormatDatetimeProducesTheExpectedNineteenCharacterString) {
  char buf[20];
  ASSERT_TRUE(bookOrbitFormatBookmarkDatetime(SOME_EPOCH_SECONDS, buf));
  EXPECT_EQ(std::string(buf), "2030-01-01 00:00:00");
  EXPECT_EQ(strlen(buf), 19u);
}

TEST(BookOrbitBookmarksTest, FormatDatetimeRoundTripsThroughDifferentTimesOfDay) {
  char buf[20];
  ASSERT_TRUE(bookOrbitFormatBookmarkDatetime(SOME_EPOCH_SECONDS + 3661, buf));  // +1h1m1s
  EXPECT_EQ(std::string(buf), "2030-01-01 01:01:01");
}

TEST(BookOrbitBookmarksTest, KeyIsDeterministicForTheSameInputs) {
  char k1[BookOrbitBookmarkKeyRow::DIGEST_SIZE];
  char k2[BookOrbitBookmarkKeyRow::DIGEST_SIZE];
  ASSERT_TRUE(bookOrbitBookmarkKey("2020-01-01 00:00:00", "/a/b/c", k1));
  ASSERT_TRUE(bookOrbitBookmarkKey("2020-01-01 00:00:00", "/a/b/c", k2));
  EXPECT_STREQ(k1, k2);
}

TEST(BookOrbitBookmarksTest, KeyDiffersWhenEitherInputDiffers) {
  char base[BookOrbitBookmarkKeyRow::DIGEST_SIZE];
  char diffDatetime[BookOrbitBookmarkKeyRow::DIGEST_SIZE];
  char diffPos[BookOrbitBookmarkKeyRow::DIGEST_SIZE];
  ASSERT_TRUE(bookOrbitBookmarkKey("2020-01-01 00:00:00", "/a", base));
  ASSERT_TRUE(bookOrbitBookmarkKey("2020-01-01 00:00:01", "/a", diffDatetime));
  ASSERT_TRUE(bookOrbitBookmarkKey("2020-01-01 00:00:00", "/b", diffPos));
  EXPECT_STRNE(base, diffDatetime);
  EXPECT_STRNE(base, diffPos);
}

TEST(BookOrbitBookmarksTest, KeyRejectsEmptyInputs) {
  char k[BookOrbitBookmarkKeyRow::DIGEST_SIZE];
  EXPECT_FALSE(bookOrbitBookmarkKey("", "/a", k));
  EXPECT_FALSE(bookOrbitBookmarkKey("2020-01-01 00:00:00", "", k));
  EXPECT_FALSE(bookOrbitBookmarkKey(nullptr, "/a", k));
  EXPECT_FALSE(bookOrbitBookmarkKey("2020-01-01 00:00:00", nullptr, k));
}

}  // namespace
