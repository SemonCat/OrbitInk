#pragma once

#include <Epub.h>
#include <FsHelpers.h>
#include <Logging.h>

#include "ProgressFile.h"
#include "ProgressRecord.h"

namespace EpubReaderUtils {

// Persists reader progress for an EPUB to its cache directory. An unchanged old-format record is
// kept in the 6-byte format so it remains eligible for the first valid server baseline.
inline bool saveProgress(const Epub& epub, int spineIndex, int pageNumber, int pageCount, uint64_t timestamp = 0,
                         bool preserveLegacy = false) {
  if (spineIndex < 0 || spineIndex > 0xFFFF || pageNumber < 0 || pageNumber > 0xFFFF || pageCount < 0 ||
      pageCount > 0xFFFF) {
    LOG_ERR("ERS", "Progress values out of range: spine=%d page=%d count=%d", spineIndex, pageNumber, pageCount);
    return false;
  }
  ProgressRecord::Value value{static_cast<uint16_t>(spineIndex), static_cast<uint16_t>(pageNumber),
                              static_cast<uint16_t>(pageCount), timestamp};
  uint8_t data[ProgressRecord::TIMESTAMPED_SIZE];
  const size_t dataSize = ProgressRecord::encode(value, data, preserveLegacy);
  if (!ProgressFile::writeAtomic(epub.getCachePath(), data, dataSize)) {
    return false;
  }
  LOG_DBG("ERS", "Progress saved: spine=%d page=%d", spineIndex, pageNumber);
  return true;
}

// Reads back progress.bin written by saveProgress(). Returns false if no progress
// is cached (spineIndex/pageNumber/pageCount are left unchanged). pageCount is
// only set when the 6-byte (v2) format is present; callers should treat a
// pageCount of 0 as "unknown" for older 4-byte saves.
inline bool loadProgress(const std::string& cachePath, int& spineIndex, int& pageNumber, int& pageCount,
                         uint64_t& timestamp, bool& legacy) {
  HalFile f;
  if (!Storage.openFileForRead("ERS", cachePath + "/progress.bin", f)) {
    return false;
  }
  uint8_t data[ProgressRecord::TIMESTAMPED_SIZE];
  const int dataSize = f.read(data, sizeof(data));
  ProgressRecord::Value value;
  if (!ProgressRecord::decode(data, static_cast<size_t>(dataSize), value)) return false;
  spineIndex = value.spineIndex;
  pageNumber = value.pageNumber;
  pageCount = value.pageCount;
  timestamp = value.timestamp;
  legacy = value.legacy;
  return true;
}

inline bool loadProgress(const std::string& cachePath, int& spineIndex, int& pageNumber, int& pageCount,
                         uint64_t& timestamp) {
  bool ignoredLegacy = false;
  return loadProgress(cachePath, spineIndex, pageNumber, pageCount, timestamp, ignoredLegacy);
}

inline bool loadProgress(const std::string& cachePath, int& spineIndex, int& pageNumber, int& pageCount) {
  uint64_t ignoredTimestamp = 0;
  return loadProgress(cachePath, spineIndex, pageNumber, pageCount, ignoredTimestamp);
}

// 0-100 completion percentage for an EPUB's saved reading progress, or -1.0f if path isn't an
// EPUB or its book.bin cache can't be loaded from disk as-is (buildIfMissing=false -- callers use
// this only for books that should already have been opened once, e.g. recent/library covers, so
// this deliberately doesn't pay for a full rebuild just to decorate a cover).
inline float recentBookProgressPercent(const std::string& path) {
  if (!FsHelpers::hasEpubExtension(path)) return -1.0f;

  Epub epub(path, "/.crosspoint");
  if (!epub.load(/*buildIfMissing=*/false, /*skipLoadingCss=*/true)) return -1.0f;

  int spineIndex = 0;
  int pageNumber = 0;
  int pageCount = 0;
  if (!loadProgress(epub.getCachePath(), spineIndex, pageNumber, pageCount)) return -1.0f;

  const float chapterProgress = pageCount > 0 ? static_cast<float>(pageNumber) / static_cast<float>(pageCount) : 0.0f;
  return epub.calculateProgress(spineIndex, chapterProgress) * 100.0f;
}

}  // namespace EpubReaderUtils
