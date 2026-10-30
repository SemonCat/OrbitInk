// Owner-scoped BookOrbit library index (src/util/BookOrbitLibraryStore) and the shared replace
// state machine (src/util/AtomicFileReplace.h), over the in-memory HalStorage stub.
#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>

#include "AtomicFileReplace.h"
#include "BookOrbitLibraryStore.h"
#include "HalStorage.h"

namespace {

using BookOrbitLibrary::Entry;
using BookOrbitLibrary::Store;

void digest(const uint8_t* data, const size_t len, uint8_t out[32]) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    h ^= data[i];
    h *= 16777619u;
  }
  for (int i = 0; i < 32; i++) {
    h ^= static_cast<uint32_t>(i + 1) * 2654435761u;
    h *= 16777619u;
    out[i] = static_cast<uint8_t>((h >> 13) & 0xFF);
  }
}

BookOrbitOwnerId owner(const char* user) { return BookOrbitOwnerId::derive("https://bo.example", user, &digest); }

Entry entry(const std::string& bookId, const std::string& title = "書名") {
  Entry e;
  e.bookId = bookId;
  e.fileId = "f" + bookId;
  e.fileHash = "0123456789abcdef0123456789abcdef";
  e.path = "/BookOrbit/" + bookId + ".epub";
  e.title = title;
  e.authors = "作者";
  e.sizeBytes = 1234;
  e.downloadedAt = 1760000000u;
  return e;
}

class LibraryStoreTest : public ::testing::Test {
 protected:
  void SetUp() override { testStorage().reset(); }
  std::unique_ptr<Store> store(const char* user = "alice") { return std::make_unique<Store>(owner(user), &digest); }
};

TEST(LibraryCodecTest, RoundTripsEveryField) {
  uint8_t rec[BookOrbitLibrary::RECORD_BYTES];
  const Entry in = entry("42");
  ASSERT_TRUE(BookOrbitLibrary::encode(in, rec));
  Entry out;
  ASSERT_TRUE(BookOrbitLibrary::decode(rec, out));
  EXPECT_EQ(out.bookId, in.bookId);
  EXPECT_EQ(out.fileId, in.fileId);
  EXPECT_EQ(out.fileHash, in.fileHash);
  EXPECT_EQ(out.path, in.path);
  EXPECT_EQ(out.title, in.title);
  EXPECT_EQ(out.authors, in.authors);
  EXPECT_EQ(out.sizeBytes, in.sizeBytes);
  EXPECT_EQ(out.downloadedAt, in.downloadedAt);
}

TEST(LibraryCodecTest, IdentifiersThatDoNotFitAreRejectedNeverTruncated) {
  uint8_t rec[BookOrbitLibrary::RECORD_BYTES];
  Entry e = entry("1");
  e.path = "/" + std::string(BookOrbitLibrary::PATH_BYTES, 'x');
  EXPECT_FALSE(BookOrbitLibrary::encode(e, rec));
  e = entry(std::string(BookOrbitLibrary::BOOK_ID_BYTES, '9'));
  EXPECT_FALSE(BookOrbitLibrary::encode(e, rec));
  e = entry("1");
  e.fileHash = "NOT-HEX";
  EXPECT_FALSE(BookOrbitLibrary::encode(e, rec));
}

TEST(LibraryCodecTest, DisplayTextIsTruncatedOnAUtf8Boundary) {
  uint8_t rec[BookOrbitLibrary::RECORD_BYTES];
  std::string longTitle;
  for (int i = 0; i < 60; i++) longTitle += "書";  // 180 bytes > TITLE_BYTES
  ASSERT_TRUE(BookOrbitLibrary::encode(entry("1", longTitle), rec));
  Entry out;
  ASSERT_TRUE(BookOrbitLibrary::decode(rec, out));
  EXPECT_LT(out.title.size(), BookOrbitLibrary::TITLE_BYTES);
  EXPECT_EQ(out.title.size() % 3, 0u) << "no severed multi-byte sequence";
}

TEST(LibraryCodecTest, AFlippedByteFailsTheChecksum) {
  uint8_t rec[BookOrbitLibrary::RECORD_BYTES];
  ASSERT_TRUE(BookOrbitLibrary::encode(entry("1"), rec));
  rec[BookOrbitLibrary::OFF_PATH + 3] ^= 0x01;
  Entry out;
  EXPECT_FALSE(BookOrbitLibrary::decode(rec, out));
}

TEST(LibraryPathTest, DownloadPathsAreOwnerScopedStableAndBounded) {
  std::string title;
  for (int i = 0; i < 80; i++) title += "長";
  const std::string a = BookOrbitLibrary::downloadPath(owner("alice"), "7", "70", title, &digest);
  const std::string a2 = BookOrbitLibrary::downloadPath(owner("alice"), "7", "70", title, &digest);
  const std::string b = BookOrbitLibrary::downloadPath(owner("bob"), "7", "70", title, &digest);
  const std::string other = BookOrbitLibrary::downloadPath(owner("alice"), "7", "71", title, &digest);
  EXPECT_EQ(a, a2) << "stable";
  EXPECT_NE(a, b) << "another account never shares a path";
  EXPECT_NE(a, other) << "another file/edition never shares a path";
  EXPECT_LT(a.size(), BookOrbitLibrary::PATH_BYTES);
  EXPECT_EQ(a.rfind("/BookOrbit/", 0), 0u);
  EXPECT_EQ(a.substr(a.size() - 5), ".epub");
  // Framing: ("1","23") and ("12","3") must differ.
  EXPECT_NE(BookOrbitLibrary::downloadPath(owner("a"), "1", "23", "t", &digest),
            BookOrbitLibrary::downloadPath(owner("a"), "12", "3", "t", &digest));
}

TEST_F(LibraryStoreTest, UpsertFindAndNewestFirstPaging) {
  auto s = store();
  for (int i = 1; i <= 5; i++) ASSERT_TRUE(s->upsert(entry(std::to_string(i))));
  Entry e;
  ASSERT_TRUE(s->find("3", e));
  EXPECT_EQ(e.path, "/BookOrbit/3.epub");
  ASSERT_TRUE(s->findByPath("/BookOrbit/4.epub", e));
  EXPECT_EQ(e.bookId, "4");
  Entry page[2];
  ASSERT_EQ(s->page(0, page, 2), 2u);
  EXPECT_EQ(page[0].bookId, "5");
  EXPECT_EQ(page[1].bookId, "4");
  ASSERT_EQ(s->page(4, page, 2), 1u);
  EXPECT_EQ(page[0].bookId, "1");
}

TEST_F(LibraryStoreTest, UpsertReplacesTheBooksEntryAndMovesItToNewest) {
  auto s = store();
  ASSERT_TRUE(s->upsert(entry("1")));
  ASSERT_TRUE(s->upsert(entry("2")));
  Entry updated = entry("1");
  updated.path = "/BookOrbit/1-new.epub";
  ASSERT_TRUE(s->upsert(updated));
  EXPECT_EQ(s->count(), 2u);
  Entry page[1];
  ASSERT_EQ(s->page(0, page, 1), 1u);
  EXPECT_EQ(page[0].path, "/BookOrbit/1-new.epub");
}

TEST_F(LibraryStoreTest, AnotherAccountSeesNothingAndCannotAlias) {
  ASSERT_TRUE(store("alice")->upsert(entry("1")));
  auto bob = store("bob");
  Entry e;
  EXPECT_FALSE(bob->find("1", e)) << "same bookId under another account must not alias";
  EXPECT_EQ(bob->count(), 0u);
  ASSERT_TRUE(bob->upsert(entry("1", "Bob's copy")));
  ASSERT_TRUE(store("alice")->find("1", e));
  EXPECT_EQ(e.title, "書名") << "bob's write did not touch alice's index";
  EXPECT_NE(store("alice")->indexPath(), bob->indexPath());
}

TEST_F(LibraryStoreTest, AForeignHeaderInTheOwnersPathReadsEmptyAndIsNotDeleted) {
  auto s = store();
  ASSERT_TRUE(s->upsert(entry("1")));
  auto& bytes = testStorage().files[s->indexPath()];
  bytes[5] ^= 0xFF;  // corrupt the recorded owner
  EXPECT_EQ(s->count(), 0u);
  EXPECT_TRUE(testStorage().files.count(s->indexPath())) << "never deleted by a read";
}

TEST_F(LibraryStoreTest, NoCredentialsMeansNothingIsReadOrWritten) {
  Store s(BookOrbitOwnerId{}, &digest);
  EXPECT_FALSE(s.enabled());
  EXPECT_FALSE(s.upsert(entry("1")));
  EXPECT_TRUE(testStorage().files.empty());
}

TEST_F(LibraryStoreTest, CapacityEvictsTheOldestFromTheIndexOnly) {
  auto s = store();
  for (size_t i = 0; i < BookOrbitLibrary::MAX_ENTRIES + 2; i++) ASSERT_TRUE(s->upsert(entry(std::to_string(i))));
  EXPECT_EQ(s->count(), BookOrbitLibrary::MAX_ENTRIES);
  Entry e;
  EXPECT_FALSE(s->find("0", e));
  EXPECT_FALSE(s->find("1", e));
  EXPECT_TRUE(s->find("2", e));
  EXPECT_TRUE(s->find(std::to_string(BookOrbitLibrary::MAX_ENTRIES + 1), e));
}

TEST_F(LibraryStoreTest, AFailedPromotionKeepsTheLastCommittedIndex) {
  auto s = store();
  ASSERT_TRUE(s->upsert(entry("1")));
  testStorage().failRenameAt = testStorage().renameCount + 2;  // stage-aside ok, promote fails
  EXPECT_FALSE(s->upsert(entry("2")));
  Entry e;
  EXPECT_TRUE(s->find("1", e)) << "live restored from backup";
  EXPECT_FALSE(s->find("2", e));
  EXPECT_FALSE(testStorage().files.count(AtomicFileReplace::tmpPath(s->indexPath())));
}

TEST_F(LibraryStoreTest, AFailedCloseNeverPromotesAPartialTemp) {
  auto s = store();
  ASSERT_TRUE(s->upsert(entry("1")));
  testStorage().failFlush = true;
  EXPECT_FALSE(s->upsert(entry("2")));
  testStorage().failFlush = false;
  Entry e;
  EXPECT_TRUE(s->find("1", e));
  EXPECT_FALSE(s->find("2", e));
}

TEST_F(LibraryStoreTest, RecoveryAfterACrashBetweenPromoteAndBackupRemovalKeepsTheNewIndex) {
  auto s = store();
  ASSERT_TRUE(s->upsert(entry("1")));
  ASSERT_TRUE(s->upsert(entry("2")));
  // Model: power lost after tmp -> live but before .bak was removed. Live is committed.
  auto& files = testStorage().files;
  files[AtomicFileReplace::bakPath(s->indexPath())] = {1, 2, 3};
  Entry e;
  EXPECT_TRUE(s->find("2", e)) << "a stale .bak must never roll back a committed index";
  EXPECT_FALSE(files.count(AtomicFileReplace::bakPath(s->indexPath())));
}

TEST_F(LibraryStoreTest, RecoveryRestoresTheBackupWhenLiveIsMissing) {
  auto s = store();
  ASSERT_TRUE(s->upsert(entry("1")));
  auto& files = testStorage().files;
  // Model: crashed after live -> .bak, before .tmp -> live; the .tmp is unverified.
  files[AtomicFileReplace::bakPath(s->indexPath())] = files[s->indexPath()];
  files.erase(s->indexPath());
  files[AtomicFileReplace::tmpPath(s->indexPath())] = {9, 9, 9};
  Entry e;
  EXPECT_TRUE(s->find("1", e));
  EXPECT_FALSE(files.count(AtomicFileReplace::tmpPath(s->indexPath())));
}

TEST_F(LibraryStoreTest, ALoneTempIsDiscardedNeverPromoted) {
  auto s = store();
  ASSERT_TRUE(s->upsert(entry("1")));
  auto& files = testStorage().files;
  files[AtomicFileReplace::tmpPath(s->indexPath())] = files[s->indexPath()];
  files.erase(s->indexPath());
  Entry e;
  EXPECT_FALSE(s->find("1", e)) << "an unverified temp is not a committed index";
  EXPECT_FALSE(files.count(AtomicFileReplace::tmpPath(s->indexPath())));
}

TEST_F(LibraryStoreTest, ACorruptRecordIsSkippedAndTheRestRemainReadable) {
  auto s = store();
  for (int i = 1; i <= 3; i++) ASSERT_TRUE(s->upsert(entry(std::to_string(i))));
  auto& bytes = testStorage().files[s->indexPath()];
  bytes[BookOrbitLibrary::HEADER_BYTES + BookOrbitLibrary::RECORD_BYTES + 10] ^= 0x40;  // record #2
  EXPECT_EQ(s->count(), 2u);
  Entry e;
  EXPECT_TRUE(s->find("1", e));
  EXPECT_TRUE(s->find("3", e));
  ASSERT_TRUE(s->upsert(entry("4")));
  EXPECT_EQ(s->count(), 3u) << "rewrite drops only the undecodable record";
}

}  // namespace
