#include "LibraryIndexBuilder.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Xtc.h>

#include <algorithm>
#include <iterator>

#include "util/BookMetadataResolver.h"
#include "util/LibraryIndexDelta.h"

namespace {
// Cheap "does this book already have a thumbnail at this size" check for a carriedOver entry --
// no load()/parse, since Epub/Xtc::getThumbBmpPath() only depends on the constructor-computed
// cache path. Mirrors the per-extension dispatch in BookMetadataResolver::resolve() and
// clearBookCache(); TXT/MD have no cover concept and are never "missing" one.
bool hasCachedCoverAtSize(const std::string& path, const int coverWidth, const int coverHeight) {
  if (FsHelpers::hasEpubExtension(path)) {
    const Epub epub(path, "/.crosspoint");
    return Storage.exists(epub.getThumbBmpPath(coverWidth, coverHeight).c_str());
  }
  if (FsHelpers::hasXtcExtension(path)) {
    const Xtc xtc(path, "/.crosspoint");
    return Storage.exists(xtc.getThumbBmpPath(coverWidth, coverHeight).c_str());
  }
  return true;
}
}  // namespace

void LibraryIndexBuilder::begin(const int coverWidth, const int coverHeight) {
  carriedOver.clear();
  pending.clear();
  resolved.clear();
  coverBackfill.clear();
  nextPendingIndex = 0;
  nextCoverBackfillIndex = 0;
  this->coverWidth = coverWidth;
  this->coverHeight = coverHeight;
  dirty = false;
  startMs = millis();

  const auto scanned = LibraryScanner::scanAllBooks("/", LibraryScanner::MAX_LIBRARY_BOOKS,
                                                    /*collectFingerprint=*/true);

  LibraryIndex::Index oldIndex;
  const bool hadOldIndex = LibraryIndex::load(indexPath, oldIndex);

  // Carry-over matching (including moving rather than copying each reusable entry's metadata)
  // lives in LibraryIndexDelta so it can be tested without SD storage -- see its header.
  LibraryIndexDelta::CarryOverMatcher matcher(oldIndex.entries);

  // Reserve ONLY what is guaranteed to be used. carriedOver deliberately gets no reservation at
  // all: the bound available here is an upper one, and in the all-changed case it would hold a
  // full-size empty buffer alive next to both the old index and the scan -- see
  // guaranteedPendingCount(). pending's bound is a lower one, so every slot it takes is a slot
  // that will be filled, and it saves the doubling copies for the cold-build case that needs
  // them most.
  pending.reserve(LibraryIndexDelta::guaranteedPendingCount(scanned.size(), matcher.oldEntryCount()));

  for (const auto& scannedEntry : scanned) {
    LibraryIndex::Entry entry;
    if (matcher.takeMatch(scannedEntry.path, scannedEntry.size, scannedEntry.fatDateTime, entry)) {
      carriedOver.push_back(std::move(entry));
      // Unchanged metadata never goes through step(), so a missing-cover check has to happen
      // here instead -- cheap (a stat, no parse) for the common case where it's already cached.
      if (coverWidth > 0 && coverHeight > 0 && !hasCachedCoverAtSize(scannedEntry.path, coverWidth, coverHeight)) {
        coverBackfill.push_back(carriedOver.size() - 1);
      }
    } else {
      pending.push_back(scannedEntry);
    }
  }

  // Something changed if there's parse work to do, or an old entry didn't survive (its book was
  // deleted/moved and is no longer in the scan) -- both need a commit() even though only the
  // first costs visible time. !hadOldIndex covers the very first build: carriedOver/oldIndex are
  // both empty then, so the size comparison alone wouldn't otherwise flag "needs a write."
  dirty = !pending.empty() || !hadOldIndex || carriedOver.size() != matcher.oldEntryCount();

  LOG_DBG("LIBIDX", "begin(): %zu carried over, %zu pending, %zu cover backfill, dirty=%d", carriedOver.size(),
          pending.size(), coverBackfill.size(), dirty);
}

bool LibraryIndexBuilder::step() {
  if (!hasWork()) {
    return false;
  }

  // Sized here rather than in begin(): step() appends exactly one entry per pending book, so the
  // final size is known either way, but begin() still has the loaded old index and the whole
  // scan resident. Allocating this there would raise the peak by a full result set at the worst
  // possible moment; by the first step() both of those are long gone.
  if (nextPendingIndex == 0) {
    resolved.reserve(pending.size());
  }

  const LibraryScanner::Entry& scannedEntry = pending[nextPendingIndex];
  // Folds cover generation into the same resolve() call already paying to open/parse this book's
  // metadata -- coverWidth/coverHeight are 0 (BookMetadataResolver::resolve()'s default, meaning
  // "skip cover work") unless begin() was given a real size.
  const BookMetadataResolver::Result result = BookMetadataResolver::resolve(scannedEntry.path, coverWidth, coverHeight);

  LibraryIndex::Entry entry;
  entry.path = scannedEntry.path;
  entry.size = scannedEntry.size;
  entry.fatDateTime = scannedEntry.fatDateTime;
  entry.title = result.title;
  entry.author = result.author;
  entry.series = result.series;
  entry.seriesIndex = result.seriesIndex;
  resolved.push_back(std::move(entry));

  nextPendingIndex++;
  return hasWork();
}

bool LibraryIndexBuilder::stepCover() {
  if (!hasCoverWork()) {
    return false;
  }

  const LibraryIndex::Entry& entry = carriedOver[coverBackfill[nextCoverBackfillIndex]];
  // Discards title/author/series -- carriedOver's are already correct and untouched by this call;
  // it exists only to generate the thumbnail found missing in begin(). Reaching the cover still
  // requires opening/parsing the book (the cover href comes from the same metadata), so this pays
  // a real parse cost, but only for the entries begin() actually flagged as missing a cover.
  BookMetadataResolver::resolve(entry.path, coverWidth, coverHeight);

  nextCoverBackfillIndex++;
  return hasCoverWork();
}

bool LibraryIndexBuilder::commit() {
  if (!dirty) {
    return true;  // Already matches what's on disk -- nothing to write.
  }

  // Move rather than copy into the write buffer. Copying built a third full set of every
  // entry's strings alongside carriedOver and resolved -- the heap peak of the whole rebuild,
  // reached at the exact moment a large SD write is also about to run.
  LibraryIndex::Index index;
  index.entries = std::move(carriedOver);
  index.entries.reserve(index.entries.size() + resolved.size());
  std::move(resolved.begin(), resolved.end(), std::back_inserter(index.entries));
  index.lastBuildDurationMs = millis() - startMs;

  const bool ok = LibraryIndex::save(indexPath, index);
  LOG_DBG("LIBIDX", "commit(): %zu entries, %lums, ok=%d", index.entries.size(),
          static_cast<unsigned long>(index.lastBuildDurationMs), ok);

  // Take the entries back, unconditionally: `index` is about to die, and moving out of the two
  // member vectors would otherwise leave the builder unable to answer totalEntries() (read by
  // the completion screen AFTER commit()) or to be committed again after a failed save.
  // Everything now lives in carriedOver, with resolved emptied, so the count still totals
  // correctly and coverBackfill's indices -- which only ever point into carriedOver's original
  // leading run, whose order and positions are unchanged -- stay valid for the backfill phase
  // that runs after this.
  carriedOver = std::move(index.entries);
  resolved.clear();
  return ok;
}

uint32_t LibraryIndexBuilder::elapsedMs() const { return millis() - startMs; }

void LibraryIndexBuilder::cancel() {
  carriedOver.clear();
  pending.clear();
  resolved.clear();
  coverBackfill.clear();
  nextPendingIndex = 0;
  nextCoverBackfillIndex = 0;
  dirty = false;
  // The on-disk index (if any) is never touched here -- only commit() writes. Any covers already
  // generated by a prior stepCover() call this run are real files on disk and are left in place --
  // there's nothing to roll back there either.
}
