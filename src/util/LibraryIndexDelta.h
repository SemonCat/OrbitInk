#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "LibraryIndex.h"

// The carry-over half of LibraryIndexBuilder::begin()'s delta: given the entries of a previously
// saved index, decide per scanned book whether its metadata can be reused verbatim, and hand it
// over WITHOUT copying the strings.
//
// Split out from LibraryIndexBuilder for two reasons: it is the part with a real invariant to get
// wrong (see takeMatch), and it depends on nothing but LibraryIndex::Entry, so it can be tested
// on the host -- LibraryIndexBuilder itself reaches SD storage and EPUB parsing.
namespace LibraryIndexDelta {

// Sorted-by-path view over the old index's entries, for binary-search lookup. Mirrors the
// sorted+binary-search convention ContentOpfParser/BookMetadataCache already use at a similar
// scale, rather than pulling in a hash map.
//
// Holds pointers INTO the caller's vector: that vector must outlive the matcher and must not be
// resized while it is in use.
class CarryOverMatcher {
 public:
  explicit CarryOverMatcher(std::vector<LibraryIndex::Entry>& oldEntries);

  // If an old entry matches (path, size, fatDateTime) exactly, fills `out` with it and returns
  // true; otherwise leaves `out` untouched and returns false.
  //
  // The matched entry's title/author/series are MOVED into `out`, not copied: the old index is a
  // short-lived local in the caller, and holding a second full copy of every unchanged book's
  // metadata is the heap peak of a rebuild on a 380KB device.
  //
  // `path` is deliberately copied rather than moved. It is this matcher's sort key, and emptying
  // it would break the ordering every later lookup binary-searches over -- silently turning
  // carried-over books into re-parsed ones, which looks like a slow rebuild rather than a bug.
  // Leaving it intact also keeps a repeated path (which the library scan does not produce, but
  // which must not be able to corrupt anything) contained: the second match yields the
  // moved-from entry's empty metadata, never another book's.
  bool takeMatch(const std::string& path, uint32_t size, uint32_t fatDateTime, LibraryIndex::Entry& out);

  // Number of old entries under consideration -- used to tell "an old entry was dropped" (its
  // book was deleted or moved) from "everything carried over".
  size_t oldEntryCount() const { return byPath.size(); }

 private:
  std::vector<LibraryIndex::Entry*> byPath;
};

// How many scanned books CANNOT be carried over, whatever the fingerprint comparisons turn out
// to say: with at most `oldEntryCount` books available to match against, at least this many of
// the `scannedCount` scanned ones must be (re)parsed. A LOWER bound, so reserving it for the
// pending list never allocates a slot that goes unused.
//
// There is deliberately NO counterpart for the carried-over list. The only bound available
// before the comparisons run is min(scannedCount, oldEntryCount), which is an UPPER bound --
// and reserving an upper bound is exactly the wrong trade here. In the all-changed case (every
// book's size or timestamp moved, e.g. a library recopied from a PC) nothing carries over at
// all, so that reservation is a full-size buffer that stays empty for the whole of begin(),
// held alive alongside the loaded old index AND the fresh scan. At ~144 bytes per entry on
// device that is ~14KB for 100 books and past the entire heap at LibraryScanner::
// MAX_LIBRARY_BOOKS -- a heap regression on precisely the device this code exists to protect,
// paid to avoid some vector growth. The carried-over list is left to grow on its own.
constexpr size_t guaranteedPendingCount(const size_t scannedCount, const size_t oldEntryCount) {
  return scannedCount > oldEntryCount ? scannedCount - oldEntryCount : 0;
}

}  // namespace LibraryIndexDelta
