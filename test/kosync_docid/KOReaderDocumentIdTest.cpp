// Read-loop tests for the REAL lib/KOReaderSync/KOReaderDocumentId.cpp.
//
// The hazard here is memory safety, not hashing. HalFile::read() returns int and signals an I/O
// error with -1; the loop used to store that in a size_t, where it becomes SIZE_MAX, sails past
// a `> 0` test, and hands md5.add() a four-billion-byte length to walk off the end of a 1 KB
// stack buffer. The MD5 itself is stubbed -- what these assert is the LENGTH every add() is
// given, and that a failing read produces no hash at all.
//
// This path predates the BookOrbit work, but that feature now calls it on every book open when
// binary matching is configured, which is why it is covered here.

#include <gtest/gtest.h>

#include <string>

#include "HalStorage.h"
#include "KOReaderDocumentId.h"
#include "MD5Builder.h"

namespace {

constexpr char BOOK[] = "/books/novel.epub";

class KOReaderDocumentIdTest : public ::testing::Test {
 protected:
  void SetUp() override {
    testStorage().reset();
    Md5Calls::clear();
  }

  // The offset sequence is 0, then 1024 << 2i; an offset at or past the file size is skipped.
  void giveFileOfSize(const size_t bytes) { testStorage().files[BOOK] = std::vector<uint8_t>(bytes, 0xAB); }
};

TEST_F(KOReaderDocumentIdTest, ANegativeReadProducesNoHashAndNeverHandsOutAHugeLength) {
  giveFileOfSize(4096);
  testStorage().failReadErrorAfter = 0;  // every read reports an I/O error (-1)

  const std::string hash = KOReaderDocumentId::calculate(BOOK);

  EXPECT_TRUE(hash.empty()) << "a read error must fail closed, not yield a plausible wrong id";
  for (const size_t len : Md5Calls::lengths()) {
    EXPECT_LE(len, 1024u) << "md5.add() was handed " << len << " bytes -- a negative read leaked through as size_t";
  }
}

TEST_F(KOReaderDocumentIdTest, AnOrdinaryFileStillHashesTheExpectedChunkLengths) {
  // Offsets 0 and 1024 fall inside a 4096-byte file; both read a full 1024-byte chunk.
  giveFileOfSize(4096);

  const std::string hash = KOReaderDocumentId::calculate(BOOK);

  EXPECT_FALSE(hash.empty());
  ASSERT_FALSE(Md5Calls::lengths().empty());
  for (const size_t len : Md5Calls::lengths()) {
    EXPECT_GT(len, 0u);
    EXPECT_LE(len, 1024u);
  }
}

TEST_F(KOReaderDocumentIdTest, APartialPositiveReadStillContributesWhatItReturned) {
  // A short but non-negative read is normal near the end of a file and must keep working
  // exactly as before -- the fix must not turn "fewer bytes than asked for" into a failure.
  giveFileOfSize(700);  // only offset 0 is inside, and it can supply just 700 bytes

  const std::string hash = KOReaderDocumentId::calculate(BOOK);

  EXPECT_FALSE(hash.empty());
  ASSERT_EQ(Md5Calls::lengths().size(), 1u);
  EXPECT_EQ(Md5Calls::lengths()[0], 700u) << "a short but positive read must still contribute";
}

TEST_F(KOReaderDocumentIdTest, AnEmptyFileHashesNothingButStillReturnsAnId) {
  giveFileOfSize(0);  // every offset is at or past the end, so all are skipped

  const std::string hash = KOReaderDocumentId::calculate(BOOK);

  EXPECT_FALSE(hash.empty()) << "no data to hash is not an error; the file is simply tiny";
  EXPECT_TRUE(Md5Calls::lengths().empty());
}

TEST_F(KOReaderDocumentIdTest, AnUnopenableFileYieldsAnEmptyHash) {
  giveFileOfSize(4096);
  testStorage().failOpenRead = true;
  EXPECT_TRUE(KOReaderDocumentId::calculate(BOOK).empty());
}

TEST_F(KOReaderDocumentIdTest, AMidwayReadErrorAbandonsTheHashRatherThanTruncatingIt) {
  // The first chunk succeeds, the second fails. Hashing only the first would produce a
  // well-formed but WRONG document id, silently detaching this book from its server record.
  giveFileOfSize(4096);
  testStorage().failReadErrorAfter = 1024;  // offset 0's chunk succeeds; the read at offset 1024 errors

  EXPECT_TRUE(KOReaderDocumentId::calculate(BOOK).empty());
}

// ---- Progress identity follows the setting; stats identity never does -------------------------

TEST_F(KOReaderDocumentIdTest, TheTwoIdentitySchemesGenuinelyDiffer) {
  // Everything below rests on this: if filename and binary hashing produced the same value there
  // would be nothing to get wrong, and no test here would mean anything.
  giveFileOfSize(4096);
  EXPECT_NE(KOReaderDocumentId::calculateFromFilename(BOOK), KOReaderDocumentId::calculate(BOOK));
}

TEST_F(KOReaderDocumentIdTest, ProgressIdentityStillHonoursTheConfiguredMatchMethod) {
  // Unchanged behaviour, pinned so the stats fix cannot quietly drag progress along with it.
  // Filename matching is a real, server-supported mode for progress; it is only stats that
  // BookOrbit cannot resolve it for.
  giveFileOfSize(4096);
  EXPECT_EQ(KOReaderDocumentId::calculateForProgress(BOOK, /*useFilenameMatching=*/true),
            KOReaderDocumentId::calculateFromFilename(BOOK));
  EXPECT_EQ(KOReaderDocumentId::calculateForProgress(BOOK, /*useFilenameMatching=*/false),
            KOReaderDocumentId::calculate(BOOK));
}

TEST_F(KOReaderDocumentIdTest, StatsIdentityIsAlwaysTheBinaryPartialMd5) {
  // The release blocker. BookOrbit resolves a page-stats hash only intrinsically, via hash
  // history, or via a manual link that already exists -- and its page-stats endpoint never
  // records an unmatched hash anywhere a user could create that link from (only match-check
  // does, which this firmware never calls). A filename hash would come back unmatched forever.
  giveFileOfSize(4096);
  EXPECT_EQ(KOReaderDocumentId::calculateForStats(BOOK), KOReaderDocumentId::calculate(BOOK));
  EXPECT_NE(KOReaderDocumentId::calculateForStats(BOOK), KOReaderDocumentId::calculateFromFilename(BOOK));
}

TEST_F(KOReaderDocumentIdTest, StatsIdentityIsIndependentOfEveryProgressMatchMode) {
  // Filename, Binary and Smart all drive the PROGRESS selector; none of them may reach the stats
  // one. Smart additionally probes the alternate-method hash, which is why the stats identity is
  // derived separately rather than read off whichever hash progress settled on.
  giveFileOfSize(4096);
  const std::string stats = KOReaderDocumentId::calculateForStats(BOOK);

  for (const bool filenameMode : {true, false}) {
    const std::string progress = KOReaderDocumentId::calculateForProgress(BOOK, filenameMode);
    EXPECT_EQ(KOReaderDocumentId::calculateForStats(BOOK), stats) << "stats moved with the match method";
    if (filenameMode) {
      EXPECT_NE(progress, stats) << "filename-mode progress and stats must be different values";
    } else {
      EXPECT_EQ(progress, stats) << "in binary mode they coincide, which is fine";
    }
  }
}

TEST_F(KOReaderDocumentIdTest, TheStatsIdentityIsStableForTheSameFile) {
  giveFileOfSize(4096);
  EXPECT_EQ(KOReaderDocumentId::calculateForStats(BOOK), KOReaderDocumentId::calculateForStats(BOOK));
}

TEST_F(KOReaderDocumentIdTest, AFailedStatsIdentityIsEmptyRatherThanAFilenameFallback) {
  // Falling back to the filename hash when the file cannot be read would reintroduce the bug
  // through the error path; callers treat an empty id as "no stats for this book".
  giveFileOfSize(4096);
  testStorage().failReadErrorAfter = 0;
  EXPECT_TRUE(KOReaderDocumentId::calculateForStats(BOOK).empty());
}

}  // namespace
