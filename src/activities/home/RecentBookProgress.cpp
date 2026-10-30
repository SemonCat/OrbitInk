// Adapted from CrossInk (uxjulia/CrossInk 9914146, src/activities/home/RecentBookProgress.cpp, MIT).
// Kept: the idea and arithmetic of a tiny per-book percent cache in the book's cache directory,
// computed from book.bin spine sizes (BookMetadataCache) instead of Epub::load(). Changed: the
// cache is keyed by the exact progress.bin bytes it was derived from (this firmware's reader does
// not write a percent cache), uses this firmware's ProgressRecord, and the TXT/XTC paths are gone
// (Home here only decorates EPUBs). Chapter fraction uses pageNumber/pageCount, matching this
// firmware's EpubReaderUtils::recentBookProgressPercent(), not CrossInk's (pageNumber+1).
#include "RecentBookProgress.h"

#include <Epub/BookMetadataCache.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ProgressRecord.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>

namespace {
constexpr uint8_t MAGIC[4] = {'B', 'O', 'P', 'C'};
constexpr uint8_t VERSION = 1;
constexpr size_t KEY_CAPACITY = ProgressRecord::TIMESTAMPED_SIZE;
// magic + version + keyLen + key + basisPoints(LE16) + checksum
constexpr size_t RECORD_SIZE = sizeof(MAGIC) + 1 + 1 + KEY_CAPACITY + 2 + 1;
constexpr char CACHE_FILE[] = "/home_percent.bin";

uint8_t checksum(const uint8_t* data, const size_t len) {
  uint8_t sum = 0xA5;
  for (size_t i = 0; i < len; i++) sum = static_cast<uint8_t>((sum << 1 | sum >> 7) ^ data[i]);
  return sum;
}

std::string cachePathFor(const std::string& path) {
  // Same derivation as Epub's constructor (lib/Epub/Epub.h), without constructing an Epub.
  return "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(path));
}

size_t readFile(const std::string& path, uint8_t* out, const size_t cap) {
  HalFile f;
  if (!Storage.openFileForRead("RBPR", path, f)) return 0;
  const int n = f.read(out, cap);
  return n > 0 ? static_cast<size_t>(n) : 0;
}

bool readCached(const std::string& cachePath, const uint8_t* key, const size_t keyLen, uint16_t& basisPoints) {
  uint8_t rec[RECORD_SIZE];
  if (readFile(cachePath + CACHE_FILE, rec, sizeof(rec)) != sizeof(rec)) return false;
  if (memcmp(rec, MAGIC, sizeof(MAGIC)) != 0 || rec[4] != VERSION) return false;
  if (checksum(rec, RECORD_SIZE - 1) != rec[RECORD_SIZE - 1]) return false;
  if (rec[5] != keyLen || memcmp(rec + 6, key, keyLen) != 0) return false;
  basisPoints = static_cast<uint16_t>(rec[6 + KEY_CAPACITY] | (rec[7 + KEY_CAPACITY] << 8));
  return basisPoints <= 10000;
}

void writeCached(const std::string& cachePath, const uint8_t* key, const size_t keyLen, const uint16_t basisPoints) {
  uint8_t rec[RECORD_SIZE] = {};
  memcpy(rec, MAGIC, sizeof(MAGIC));
  rec[4] = VERSION;
  rec[5] = static_cast<uint8_t>(keyLen);
  memcpy(rec + 6, key, keyLen);
  rec[6 + KEY_CAPACITY] = static_cast<uint8_t>(basisPoints & 0xFF);
  rec[7 + KEY_CAPACITY] = static_cast<uint8_t>(basisPoints >> 8);
  rec[RECORD_SIZE - 1] = checksum(rec, RECORD_SIZE - 1);

  // Advisory cache: a torn write fails its checksum and is simply recomputed next time, so a
  // plain temp+rename (never truncating the live file in place) is enough.
  const std::string finalPath = cachePath + CACHE_FILE;
  const std::string tmpPath = finalPath + ".tmp";
  {
    HalFile f;
    if (!Storage.openFileForWrite("RBPR", tmpPath, f)) return;
    if (f.write(rec, sizeof(rec)) != sizeof(rec)) {
      LOG_ERR("RBPR", "Short write on percent cache");
      return;
    }
    f.flush();
  }
  Storage.remove(finalPath.c_str());
  if (!Storage.rename(tmpPath.c_str(), finalPath.c_str())) LOG_ERR("RBPR", "Percent cache rename failed");
}

// Spine-size weighted percentage from book.bin only (no EPUB container access).
float computePercent(const std::string& cachePath, const ProgressRecord::Value& value) {
  BookMetadataCache metadata(cachePath);
  if (!metadata.load()) return -1.0f;
  const int spineCount = metadata.getSpineCount();
  if (spineCount <= 0 || value.spineIndex >= spineCount) return -1.0f;
  const uint32_t bookSize = metadata.getSpineEntry(spineCount - 1).cumulativeSize;
  if (bookSize == 0) return -1.0f;
  const uint32_t prev = value.spineIndex >= 1 ? metadata.getSpineEntry(value.spineIndex - 1).cumulativeSize : 0;
  const uint32_t current = metadata.getSpineEntry(value.spineIndex).cumulativeSize - prev;
  const float chapter =
      value.pageCount > 0 ? static_cast<float>(value.pageNumber) / static_cast<float>(value.pageCount) : 0.0f;
  const float total = static_cast<float>(prev) + chapter * static_cast<float>(current);
  return std::clamp(total / static_cast<float>(bookSize) * 100.0f, 0.0f, 100.0f);
}
}  // namespace

namespace RecentBookProgress {

float loadPercent(const std::string& path) {
  if (!FsHelpers::hasEpubExtension(path)) return -1.0f;
  const std::string cachePath = cachePathFor(path);

  uint8_t key[KEY_CAPACITY] = {};
  const size_t keyLen = readFile(cachePath + "/progress.bin", key, sizeof(key));
  ProgressRecord::Value value;
  if (keyLen == 0 || !ProgressRecord::decode(key, keyLen, value)) return -1.0f;

  uint16_t basisPoints = 0;
  if (readCached(cachePath, key, keyLen, basisPoints)) return static_cast<float>(basisPoints) / 100.0f;

  const float percent = computePercent(cachePath, value);
  if (percent >= 0.0f) writeCached(cachePath, key, keyLen, static_cast<uint16_t>(percent * 100.0f + 0.5f));
  return percent;
}

bool hasPercent(const float progress) { return progress >= 0.0f && progress <= 100.0f; }

void formatPercent(const float progress, char* buf, const size_t bufSize) {
  if (bufSize == 0) return;
  if (!hasPercent(progress)) {
    buf[0] = '\0';
    return;
  }
  snprintf(buf, bufSize, "%d%%", static_cast<int>(progress + 0.5f));
}

}  // namespace RecentBookProgress
