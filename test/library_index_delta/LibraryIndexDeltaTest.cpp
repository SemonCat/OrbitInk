#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "LibraryIndexDelta.h"

using LibraryIndexDelta::CarryOverMatcher;

namespace {

LibraryIndex::Entry makeEntry(const std::string& path, const uint32_t size, const uint32_t fatDateTime,
                              const std::string& title, const std::string& author) {
  LibraryIndex::Entry entry;
  entry.path = path;
  entry.size = size;
  entry.fatDateTime = fatDateTime;
  entry.title = title;
  entry.author = author;
  entry.series = title + " series";
  entry.seriesIndex = 2.0f;
  return entry;
}

}  // namespace

// The regression that matters: metadata is moved out of the old entries to halve the heap peak,
// but the matcher binary-searches the SAME entries by path. Moving the path too would leave
// emptied keys behind, destroy the sort order, and make every LATER lookup miss -- every book
// after the first would silently be re-parsed. Several books, deliberately given a scan order
// that differs from sorted-path order.
TEST(LibraryIndexDelta, AllUnchangedBooksCarryOverAfterEarlierMoves) {
  std::vector<LibraryIndex::Entry> oldEntries = {
      makeEntry("/books/alpha.epub", 100, 7, "Alpha", "Author A"),
      makeEntry("/books/bravo.epub", 200, 8, "Bravo", "Author B"),
      makeEntry("/books/charlie.epub", 300, 9, "Charlie", "Author C"),
      makeEntry("/books/delta.epub", 400, 10, "Delta", "Author D"),
  };

  CarryOverMatcher matcher(oldEntries);
  ASSERT_EQ(matcher.oldEntryCount(), 4u);

  struct Scanned {
    std::string path;
    uint32_t size;
    uint32_t fatDateTime;
  };
  const std::vector<Scanned> scanned = {
      {"/books/charlie.epub", 300, 9},
      {"/books/alpha.epub", 100, 7},
      {"/books/delta.epub", 400, 10},
      {"/books/bravo.epub", 200, 8},
  };

  std::vector<LibraryIndex::Entry> carriedOver;
  for (const auto& s : scanned) {
    LibraryIndex::Entry entry;
    ASSERT_TRUE(matcher.takeMatch(s.path, s.size, s.fatDateTime, entry)) << "missed " << s.path;
    carriedOver.push_back(std::move(entry));
  }

  ASSERT_EQ(carriedOver.size(), 4u);
  EXPECT_EQ(carriedOver[0].path, "/books/charlie.epub");
  EXPECT_EQ(carriedOver[0].title, "Charlie");
  EXPECT_EQ(carriedOver[1].path, "/books/alpha.epub");
  EXPECT_EQ(carriedOver[1].title, "Alpha");
  EXPECT_EQ(carriedOver[2].path, "/books/delta.epub");
  EXPECT_EQ(carriedOver[2].title, "Delta");
  EXPECT_EQ(carriedOver[3].path, "/books/bravo.epub");
  EXPECT_EQ(carriedOver[3].title, "Bravo");
}

TEST(LibraryIndexDelta, CarriesOverEveryMetadataField) {
  std::vector<LibraryIndex::Entry> oldEntries = {makeEntry("/a.epub", 11, 22, "Title", "Writer")};
  CarryOverMatcher matcher(oldEntries);

  LibraryIndex::Entry entry;
  ASSERT_TRUE(matcher.takeMatch("/a.epub", 11, 22, entry));
  EXPECT_EQ(entry.path, "/a.epub");
  EXPECT_EQ(entry.size, 11u);
  EXPECT_EQ(entry.fatDateTime, 22u);
  EXPECT_EQ(entry.title, "Title");
  EXPECT_EQ(entry.author, "Writer");
  EXPECT_EQ(entry.series, "Title series");
  EXPECT_FLOAT_EQ(entry.seriesIndex, 2.0f);
}

// The sort key must survive the move, or the structure the next lookup depends on is corrupt.
TEST(LibraryIndexDelta, SourcePathIsPreservedAfterAMatch) {
  std::vector<LibraryIndex::Entry> oldEntries = {makeEntry("/a.epub", 1, 2, "T", "A")};
  CarryOverMatcher matcher(oldEntries);

  LibraryIndex::Entry entry;
  ASSERT_TRUE(matcher.takeMatch("/a.epub", 1, 2, entry));
  EXPECT_EQ(oldEntries[0].path, "/a.epub");
}

// A changed file (same path, different fingerprint) must NOT carry over -- it has to be
// re-parsed, or the index would keep serving stale metadata.
TEST(LibraryIndexDelta, ChangedFingerprintDoesNotCarryOver) {
  std::vector<LibraryIndex::Entry> oldEntries = {makeEntry("/a.epub", 100, 7, "Old", "A")};
  CarryOverMatcher matcher(oldEntries);

  LibraryIndex::Entry bySize;
  EXPECT_FALSE(matcher.takeMatch("/a.epub", 101, 7, bySize));
  EXPECT_TRUE(bySize.path.empty()) << "out must be left untouched on a miss";

  LibraryIndex::Entry byDate;
  EXPECT_FALSE(matcher.takeMatch("/a.epub", 100, 8, byDate));

  // The rejections must not have consumed the entry: the real match still works.
  LibraryIndex::Entry good;
  EXPECT_TRUE(matcher.takeMatch("/a.epub", 100, 7, good));
  EXPECT_EQ(good.title, "Old");
}

TEST(LibraryIndexDelta, UnknownPathDoesNotCarryOver) {
  std::vector<LibraryIndex::Entry> oldEntries = {makeEntry("/b.epub", 1, 2, "B", "A")};
  CarryOverMatcher matcher(oldEntries);

  LibraryIndex::Entry before;
  EXPECT_FALSE(matcher.takeMatch("/a.epub", 1, 2, before));  // sorts before every old entry
  LibraryIndex::Entry after;
  EXPECT_FALSE(matcher.takeMatch("/z.epub", 1, 2, after));  // sorts past the end
}

TEST(LibraryIndexDelta, EmptyOldIndexCarriesNothingOver) {
  std::vector<LibraryIndex::Entry> oldEntries;
  CarryOverMatcher matcher(oldEntries);
  EXPECT_EQ(matcher.oldEntryCount(), 0u);

  LibraryIndex::Entry entry;
  EXPECT_FALSE(matcher.takeMatch("/a.epub", 1, 2, entry));
}

// A repeated path (a scan quirk, not normal) must stay harmless rather than hand back the
// already-emptied metadata as if it were real.
TEST(LibraryIndexDelta, RepeatedPathDoesNotResurrectMovedMetadata) {
  std::vector<LibraryIndex::Entry> oldEntries = {makeEntry("/a.epub", 1, 2, "T", "A")};
  CarryOverMatcher matcher(oldEntries);

  LibraryIndex::Entry first;
  ASSERT_TRUE(matcher.takeMatch("/a.epub", 1, 2, first));
  EXPECT_EQ(first.title, "T");

  // Still matches on the (untouched) fingerprint, but yields the moved-from metadata rather than
  // a second real copy -- so the caller sees empty strings, never another book's data.
  LibraryIndex::Entry second;
  ASSERT_TRUE(matcher.takeMatch("/a.epub", 1, 2, second));
  EXPECT_EQ(second.path, "/a.epub");
  EXPECT_TRUE(second.title.empty());
}

// --- Reserve policy (heap peak) --------------------------------------------------------------
//
// LibraryIndexBuilder::begin() runs with the loaded old index AND the fresh directory scan both
// resident, so anything it reserves is added on top of the two largest things in memory. These
// pin the rule that keeps that safe: only ever reserve capacity that is guaranteed to be filled.

// The regression this exists for. Every book's fingerprint changed (a library recopied from a
// PC), so nothing carries over -- and reserving the available upper bound, min(scanned, old),
// would have parked a full-size buffer that never receives a single entry. At ~144 bytes per
// entry on device that is ~14KB for 100 books, and past the whole heap at
// LibraryScanner::MAX_LIBRARY_BOOKS. There is no carried-over reservation to get this wrong,
// and the pending bound correctly asks for nothing here.
TEST(LibraryIndexDelta, AllChangedLibraryPreallocatesNothingUpFront) {
  EXPECT_EQ(LibraryIndexDelta::guaranteedPendingCount(2000, 2000), 0u);
  EXPECT_EQ(LibraryIndexDelta::guaranteedPendingCount(100, 100), 0u);

  // The consequence on the vectors begin() actually fills: neither is sized ahead of knowing.
  std::vector<LibraryIndex::Entry> carriedOver;  // deliberately never reserved
  std::vector<LibraryIndex::Entry> pending;
  pending.reserve(LibraryIndexDelta::guaranteedPendingCount(2000, 2000));
  EXPECT_EQ(carriedOver.capacity(), 0u);
  EXPECT_EQ(pending.capacity(), 0u);
}

// A cold build (no usable index) is the case the pending reservation is actually for: every
// scanned book must be parsed, and that is known up front.
TEST(LibraryIndexDelta, ColdBuildReservesExactlyThePendingWork) {
  EXPECT_EQ(LibraryIndexDelta::guaranteedPendingCount(2000, 0), 2000u);
  EXPECT_EQ(LibraryIndexDelta::guaranteedPendingCount(7, 0), 7u);
}

// A shrunken library (books deleted since the last build) must not be talked into reserving for
// books that are not there.
TEST(LibraryIndexDelta, FewerScannedThanIndexedReservesNothing) {
  EXPECT_EQ(LibraryIndexDelta::guaranteedPendingCount(10, 2000), 0u);
  EXPECT_EQ(LibraryIndexDelta::guaranteedPendingCount(0, 0), 0u);
}

TEST(LibraryIndexDelta, PartiallyChangedLibraryReservesOnlyTheSurplus) {
  // 120 scanned against 100 indexed: at least 20 are new, however the other 100 compare.
  EXPECT_EQ(LibraryIndexDelta::guaranteedPendingCount(120, 100), 20u);
}

// The property the whole policy rests on: the reservation is a LOWER bound on the pending list's
// final size, for every possible carry-over outcome. A slot reserved is a slot filled -- so the
// reservation can never be the dead weight that an upper bound would be.
TEST(LibraryIndexDelta, ReservationIsAlwaysALowerBoundOnRealPendingWork) {
  for (size_t scanned = 0; scanned <= 40; ++scanned) {
    for (size_t old = 0; old <= 40; ++old) {
      const size_t reserved = LibraryIndexDelta::guaranteedPendingCount(scanned, old);
      // carriedOver can be anything from nothing matching up to every comparable pair matching.
      const size_t maxCarriedOver = scanned < old ? scanned : old;
      for (size_t carried = 0; carried <= maxCarriedOver; ++carried) {
        const size_t actualPending = scanned - carried;
        ASSERT_LE(reserved, actualPending) << "scanned=" << scanned << " old=" << old << " carried=" << carried;
      }
    }
  }
}
