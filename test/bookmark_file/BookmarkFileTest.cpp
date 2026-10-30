// Tests for src/util/BookmarkFile.cpp's atomic save()/recovery-on-load(), against an in-memory
// read-write HalStorage stub (test/bookmark_file/host_stubs/HalStorage.h). The interruption
// windows and their resolution mirror BookOrbitStatsQueue::promoteReplacement()/
// recoverInterruptedReplacement() and BookOrbitCatalogClient::downloadFile()'s own
// temp+backup+promote state machine -- see BookmarkFile.cpp's own comments for the shared
// reasoning (which pairing of {dest, .bak, .tmp} is unambiguous and why).

#include <gtest/gtest.h>

#include "BookmarkFile.h"
#include "HalStorage.h"

namespace {

constexpr char BOOK_PATH[] = "/Books/dune.epub";

std::vector<BookmarkEntry> oneBookmark(const std::string& xpath, float percentage) {
  BookmarkEntry entry;
  entry.xpath = xpath;
  entry.percentage = percentage;
  return {entry};
}

class BookmarkFileTest : public ::testing::Test {
 protected:
  void SetUp() override { testBookmarkStorage().reset(); }
};

TEST_F(BookmarkFileTest, SaveThenLoadRoundTripsTheBookmarkFields) {
  const auto bookmarks = oneBookmark("/body/DocFragment[3]", 0.25f);
  ASSERT_TRUE(BookmarkFile::save(BOOK_PATH, bookmarks));

  std::vector<BookmarkEntry> loaded;
  ASSERT_TRUE(BookmarkFile::load(BOOK_PATH, loaded));
  ASSERT_EQ(loaded.size(), 1u);
  EXPECT_EQ(loaded[0].xpath, "/body/DocFragment[3]");
  EXPECT_FLOAT_EQ(loaded[0].percentage, 0.25f);
}

TEST_F(BookmarkFileTest, SaveIsAtomicNoTempOrBackupSurvivesASuccessfulSave) {
  ASSERT_TRUE(BookmarkFile::save(BOOK_PATH, oneBookmark("/a", 0.1f)));
  ASSERT_TRUE(BookmarkFile::save(BOOK_PATH, oneBookmark("/b", 0.2f)));  // a second save, replacing the first

  const std::string base = "/.crosspoint/bookmarks/Books_dune.json";
  EXPECT_TRUE(Storage.exists(base.c_str()));
  EXPECT_FALSE(Storage.exists((base + ".tmp").c_str()));
  EXPECT_FALSE(Storage.exists((base + ".bak").c_str()));
}

TEST_F(BookmarkFileTest, ATempWriteFailureLeavesThePreviousBookmarksUntouched) {
  ASSERT_TRUE(BookmarkFile::save(BOOK_PATH, oneBookmark("/original", 0.5f)));

  testBookmarkStorage().failWriteFile = true;
  EXPECT_FALSE(BookmarkFile::save(BOOK_PATH, oneBookmark("/new", 0.9f)));
  testBookmarkStorage().failWriteFile = false;

  std::vector<BookmarkEntry> loaded;
  ASSERT_TRUE(BookmarkFile::load(BOOK_PATH, loaded));
  ASSERT_EQ(loaded.size(), 1u);
  EXPECT_EQ(loaded[0].xpath, "/original") << "a failed temp write must never touch the live file";
}

TEST_F(BookmarkFileTest, ATempCloseFailureLeavesThePreviousBookmarksUntouched) {
  ASSERT_TRUE(BookmarkFile::save(BOOK_PATH, oneBookmark("/original", 0.5f)));

  testBookmarkStorage().failClose = true;
  EXPECT_FALSE(BookmarkFile::save(BOOK_PATH, oneBookmark("/new", 0.9f)));
  testBookmarkStorage().failClose = false;

  std::vector<BookmarkEntry> loaded;
  ASSERT_TRUE(BookmarkFile::load(BOOK_PATH, loaded));
  ASSERT_EQ(loaded.size(), 1u);
  EXPECT_EQ(loaded[0].xpath, "/original") << "an unsynced temp file must never replace the durable live file";
}

TEST_F(BookmarkFileTest, APromoteFailureRestoresTheBackedUpPreviousFile) {
  ASSERT_TRUE(BookmarkFile::save(BOOK_PATH, oneBookmark("/original", 0.5f)));

  // The replacement does two renames: live -> .bak, then .tmp -> live. Fail only the second so
  // this actually exercises the promote-failure restoration branch rather than the earlier
  // stage-aside failure branch.
  testBookmarkStorage().failRenameOnCall = testBookmarkStorage().renameCalls + 2;
  EXPECT_FALSE(BookmarkFile::save(BOOK_PATH, oneBookmark("/new", 0.9f)));
  testBookmarkStorage().failRenameOnCall = 0;

  std::vector<BookmarkEntry> loaded;
  ASSERT_TRUE(BookmarkFile::load(BOOK_PATH, loaded));
  ASSERT_EQ(loaded.size(), 1u);
  EXPECT_EQ(loaded[0].xpath, "/original") << "restored from .bak when the promote rename itself failed";
}

TEST_F(BookmarkFileTest, SaveRecoversAStaleBackupBeforeReplacingTheLiveFile) {
  // A reset after .tmp -> live but before cleanup leaves both live and .bak. SdFat refuses a
  // rename when the destination already exists, so save() must reconcile this state before it
  // attempts the next live -> .bak staging rename; otherwise every later save wedges forever.
  const std::string base = "/.crosspoint/bookmarks/Books_dune.json";
  testBookmarkStorage().files[base] = R"({"bookmarks":[{"xpath":"/live","percentage":0.3}]})";
  testBookmarkStorage().files[base + ".bak"] = R"({"bookmarks":[{"xpath":"/stale","percentage":0.1}]})";

  ASSERT_TRUE(BookmarkFile::save(BOOK_PATH, oneBookmark("/replacement", 0.8f)));

  std::vector<BookmarkEntry> loaded;
  ASSERT_TRUE(BookmarkFile::load(BOOK_PATH, loaded));
  ASSERT_EQ(loaded.size(), 1u);
  EXPECT_EQ(loaded[0].xpath, "/replacement");
  EXPECT_FALSE(Storage.exists((base + ".bak").c_str()));
}

TEST_F(BookmarkFileTest, SaveRefusesWhenAStaleBackupCannotBeCleared) {
  const std::string base = "/.crosspoint/bookmarks/Books_dune.json";
  testBookmarkStorage().files[base] = R"({"bookmarks":[{"xpath":"/live","percentage":0.3}]})";
  testBookmarkStorage().files[base + ".bak"] = R"({"bookmarks":[{"xpath":"/stale","percentage":0.1}]})";
  testBookmarkStorage().failRemoves = true;

  EXPECT_FALSE(BookmarkFile::save(BOOK_PATH, oneBookmark("/replacement", 0.8f)));
  EXPECT_TRUE(Storage.exists(base.c_str()));
  EXPECT_TRUE(Storage.exists((base + ".bak").c_str()));
  EXPECT_FALSE(Storage.exists((base + ".tmp").c_str()));
}

TEST_F(BookmarkFileTest, SaveFailsClosedWhenBackupPresenceIsInconclusive) {
  const std::string base = "/.crosspoint/bookmarks/Books_dune.json";
  testBookmarkStorage().files[base + ".bak"] = R"({"bookmarks":[{"xpath":"/original","percentage":0.5}]})";
  testBookmarkStorage().failPathProbes.insert(base + ".bak");

  EXPECT_FALSE(BookmarkFile::save(BOOK_PATH, oneBookmark("/replacement", 0.8f)));
  EXPECT_FALSE(Storage.exists(base.c_str()));
  EXPECT_TRUE(Storage.exists((base + ".bak").c_str())) << "the only confirmed-complete copy must stay recoverable";
  EXPECT_FALSE(Storage.exists((base + ".tmp").c_str()));
}

TEST_F(BookmarkFileTest, LoadRecoversAFileLeftHiddenByAResetBetweenTheTwoPromoteRenames) {
  // Unambiguous: dest ABSENT, .bak present. Only a reset landing between the "stage dest aside"
  // and "promote tmp into place" renames can produce this -- always safe to restore.
  const std::string base = "/.crosspoint/bookmarks/Books_dune.json";
  testBookmarkStorage().files[base + ".bak"] = R"({"bookmarks":[{"xpath":"/original","percentage":0.5}]})";

  std::vector<BookmarkEntry> loaded;
  ASSERT_TRUE(BookmarkFile::load(BOOK_PATH, loaded));
  ASSERT_EQ(loaded.size(), 1u);
  EXPECT_EQ(loaded[0].xpath, "/original") << "the hidden original must become visible again";
  EXPECT_FALSE(Storage.exists((base + ".bak").c_str()));
}

TEST_F(BookmarkFileTest, LoadFailsClosedWhenAHiddenBackupCannotBeRestored) {
  const std::string base = "/.crosspoint/bookmarks/Books_dune.json";
  testBookmarkStorage().files[base + ".bak"] = R"({"bookmarks":[{"xpath":"/original","percentage":0.5}]})";
  testBookmarkStorage().failRenames = true;

  std::vector<BookmarkEntry> loaded;
  EXPECT_FALSE(BookmarkFile::load(BOOK_PATH, loaded));
  EXPECT_TRUE(loaded.empty());
  EXPECT_TRUE(Storage.exists((base + ".bak").c_str())) << "the only complete copy must remain recoverable";
}

TEST_F(BookmarkFileTest, LoadFailsClosedWhenBackupPresenceIsInconclusive) {
  const std::string base = "/.crosspoint/bookmarks/Books_dune.json";
  testBookmarkStorage().files[base + ".bak"] = R"({"bookmarks":[{"xpath":"/original","percentage":0.5}]})";
  testBookmarkStorage().failPathProbes.insert(base + ".bak");

  std::vector<BookmarkEntry> loaded;
  EXPECT_FALSE(BookmarkFile::load(BOOK_PATH, loaded));
  EXPECT_TRUE(loaded.empty());
  EXPECT_FALSE(Storage.exists(base.c_str()));
  EXPECT_TRUE(Storage.exists((base + ".bak").c_str())) << "an inconclusive probe must not move or delete data";
}

TEST_F(BookmarkFileTest, LoadClearsAStaleBackupLeftByAnInterruptedPromotionCleanupWhenDestAlsoExists) {
  // Unambiguous, the same way BookOrbitCatalogClient::downloadFile()'s "dest exists AND .bak
  // exists" state is: .bak can only exist alongside a present dest if the promote already
  // succeeded and only its trailing cleanup step did not finish. Safe to just clear the backup and
  // keep dest -- never restore .bak over the newer file.
  const std::string base = "/.crosspoint/bookmarks/Books_dune.json";
  testBookmarkStorage().files[base] = R"({"bookmarks":[{"xpath":"/new","percentage":0.9}]})";
  testBookmarkStorage().files[base + ".bak"] = R"({"bookmarks":[{"xpath":"/stale","percentage":0.1}]})";

  std::vector<BookmarkEntry> loaded;
  ASSERT_TRUE(BookmarkFile::load(BOOK_PATH, loaded));
  ASSERT_EQ(loaded.size(), 1u);
  EXPECT_EQ(loaded[0].xpath, "/new") << "dest is already the correct, freshly-promoted content";
  EXPECT_FALSE(Storage.exists((base + ".bak").c_str())) << "the stale backup must be cleared, not restored";
}

TEST_F(BookmarkFileTest, ALeftoverUnpromotedTempIsDiscardedWithoutAffectingTheLiveFile) {
  const std::string base = "/.crosspoint/bookmarks/Books_dune.json";
  testBookmarkStorage().files[base] = R"({"bookmarks":[{"xpath":"/live","percentage":0.3}]})";
  testBookmarkStorage().files[base + ".tmp"] = R"({"bookmarks":[{"xpath":"/never-verified","percentage":0.7}]})";

  std::vector<BookmarkEntry> loaded;
  ASSERT_TRUE(BookmarkFile::load(BOOK_PATH, loaded));
  ASSERT_EQ(loaded.size(), 1u);
  EXPECT_EQ(loaded[0].xpath, "/live");
  EXPECT_FALSE(Storage.exists((base + ".tmp").c_str()));
}

}  // namespace
