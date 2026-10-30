#include "ProgressSyncPosition.h"

#include <ArduinoJson.h>
#include <Epub/SectionMetadata.h>
#include <HalStorage.h>

#include <cmath>

#include "AtomicFileReplace.h"

namespace ProgressSyncPosition {
namespace {
constexpr int POSITION_CACHE_VERSION = 3;  // Previous versions could retain unenriched headless positions.
}

std::optional<float> chapterProgress(const CrossPointPosition& position, int spineCount) {
  if (position.spineIndex < 0 || position.spineIndex >= spineCount || position.pageNumber < 0 ||
      position.totalPages < 0) return std::nullopt;
  if (position.totalPages == 0) {
    return position.pageNumber == 0 ? std::optional<float>{0.0f} : std::nullopt;
  }
  if (position.pageNumber >= position.totalPages) return std::nullopt;
  return static_cast<float>(position.pageNumber) / position.totalPages;
}

SavedProgressPosition resolve(const std::shared_ptr<Epub>& epub, CrossPointPosition position,
                               const std::string& binaryDocument) {
  const std::string path = epub->getCachePath() + "/sync_position.json";
  uint32_t layout = 2166136261U;
  HalFile sectionFile;
  if (Storage.openFileForRead("SyncPos",
                              epub->getCachePath() + "/sections/" + std::to_string(position.spineIndex) + ".bin",
                              sectionFile)) {
    uint8_t header[64];
    const int count = sectionFile.read(header, sizeof(header));
    if (count > 0) {
      for (int i = 0; i < count; ++i) layout = (layout ^ header[i]) * 16777619U;
    }
  }
  const std::string key = binaryDocument + ":" + std::to_string(position.spineIndex) + ":" +
                          std::to_string(position.pageNumber) + ":" + std::to_string(position.totalPages) + ":" +
                          std::to_string(layout);
  bool cacheable = binaryDocument.size() == 32;
  if (cacheable && AtomicFileReplace::recover("SyncPos", path)) {
    HalFile cached;
    if (Storage.openFileForRead("SyncPos", path, cached) && cached.size() <= 4096) {
      JsonDocument doc;
      if (!deserializeJson(doc, cached) && doc["version"] == POSITION_CACHE_VERSION && doc["key"].is<std::string>() &&
          doc["key"].as<std::string>() == key && doc["xpath"].is<std::string>() && doc["percentage"].is<float>()) {
        SavedProgressPosition result{doc["xpath"].as<std::string>(), doc["percentage"].as<float>()};
        if (!result.xpath.empty() && result.xpath.size() <= 1024 && std::isfinite(result.percentage) &&
            result.percentage >= 0 && result.percentage <= 1) return result;
      }
    }
  }
  if (sectionFile && position.pageNumber > 0) {
    const auto cachedPages = sectionmetadata::pageCount(sectionFile);
    if (!cachedPages) {
      cacheable = false;
    } else if (*cachedPages == position.totalPages) {
      if (const auto paragraph = sectionmetadata::paragraphIndexForPage(sectionFile, position.pageNumber - 1)) {
        position.paragraphIndex = *paragraph;
        position.hasParagraphIndex = *paragraph > 0;
      } else {
        cacheable = false;  // Retry incomplete/transient LUT reads rather than persisting a coarse anchor.
      }
    }
  }
  auto result = ProgressMapper::toSavedProgress(epub, position);
  if (cacheable && !result.xpath.empty() && result.xpath.size() <= 1024 &&
      AtomicFileReplace::recover("SyncPos", path)) {
    JsonDocument doc;
    doc["version"] = POSITION_CACHE_VERSION;
    doc["key"] = key;
    doc["xpath"] = result.xpath;
    doc["percentage"] = result.percentage;
    HalFile file;
    if (Storage.openFileForWrite("SyncPos", AtomicFileReplace::tmpPath(path), file)) {
      const size_t expected = measureJson(doc);
      const bool written = serializeJson(doc, file) == expected;
      const bool closed = file.close();
      if (written && closed && AtomicFileReplace::verifyTmp("SyncPos", path, expected)) {
        AtomicFileReplace::promote("SyncPos", path);
      }
    }
  }
  return result;
}
}  // namespace ProgressSyncPosition
