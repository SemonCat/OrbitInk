#include "BookmarkFile.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PersistableStore.h>

#include "BookmarkUtil.h"

namespace {

// Resolves a save() left mid-flight by a crash/reset, using the exact same temp+backup+promote
// state machine (and the same two-case proof) as BookOrbitStatsQueue::promoteReplacement()/
// recoverInterruptedReplacement() and BookOrbitCatalogClient::downloadFile(): `.bak` is only ever
// created by staging the live file aside at the instant it stops existing, so:
//  - .bak present, live ABSENT: the promote (tmp -> live) never ran. Restore the backup.
//  - .bak present, live PRESENT: the promote already succeeded and only its trailing
//    Storage.remove(bak) cleanup step did not run or finish. Safe to just clear the backup --
//    there is no other history that produces this pairing.
//  - .bak absent: nothing to recover; a leftover .tmp is an unpromoted, never-verified write and
//    is simply discarded.
//
// This is what closes the gap in cycle-1's own loadFailureRequiresAbort(false,
// /*absenceConfirmed=*/true): before this fix, HalStorage::writeFile()'s underlying
// SDCardManager::writeFile() removed the existing file and then wrote the new one IN PLACE (no
// staging), so a crash mid-write could leave no file at all -- indistinguishable from "this book
// has never had bookmarks" rather than "the bookmark file was just destroyed".
bool recoverInterruptedSave(const std::string& path) {
  const std::string tmpPath = path + ".tmp";
  const std::string bakPath = path + ".bak";
  const HalStorage::PathStatus liveStatus = Storage.probePath(path.c_str());
  const HalStorage::PathStatus backupStatus = Storage.probePath(bakPath.c_str());
  if (liveStatus == HalStorage::PathStatus::Error || backupStatus == HalStorage::PathStatus::Error) {
    LOG_ERR("BKM", "Could not determine interrupted-save state; refusing to modify bookmark files");
    return false;
  }
  if (backupStatus == HalStorage::PathStatus::Absent) {
    Storage.remove(tmpPath.c_str());
    return true;
  }
  if (liveStatus == HalStorage::PathStatus::Absent) {
    if (Storage.rename(bakPath.c_str(), path.c_str())) {
      LOG_INF("BKM", "Recovered a bookmark file left hidden by an interrupted save");
    } else {
      LOG_ERR("BKM", "Could not recover a hidden bookmark file at %s", bakPath.c_str());
      Storage.remove(tmpPath.c_str());
      return false;
    }
    Storage.remove(tmpPath.c_str());
    return true;
  }
  if (!Storage.remove(bakPath.c_str())) {
    LOG_ERR("BKM", "Could not clear a stale bookmark backup at %s", bakPath.c_str());
    return false;
  }
  LOG_INF("BKM", "Cleared a bookmark file backup left over from an interrupted save cleanup");
  Storage.remove(tmpPath.c_str());
  return true;
}

}  // namespace

bool BookmarkFile::load(const std::string& bookPath, std::vector<BookmarkEntry>& bookmarks) {
  bookmarks.clear();

  // Read/write go through PersistableStoreBase so the JSON parser and
  // serializer stay instantiated once, in PersistableStore.cpp.
  const std::string path = BookmarkUtil::getBookmarkPath(bookPath);
  if (!recoverInterruptedSave(path)) return false;

  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(path.c_str(), doc)) {
    return false;
  }

  JsonArray arr = doc["bookmarks"].as<JsonArray>();
  bookmarks.reserve(arr.size());
  for (JsonObject obj : arr) {
    bookmarks.emplace_back();
    auto& bookmark = bookmarks.back();
    bookmark.xpath = obj["xpath"] | "";
    bookmark.percentage = obj["percentage"] | static_cast<float>(0);
    bookmark.summary = obj["summary"] | "";
    bookmark.computedSpineIndex = obj["si"] | static_cast<uint16_t>(0);
    bookmark.computedChapterPageCount = obj["pc"] | static_cast<uint16_t>(0);
    bookmark.computedChapterProgress = obj["pp"] | static_cast<uint16_t>(0);
    // Additive fields: absent on any bookmark file written before BookOrbit bookmark sync
    // existed, which is exactly the correct default -- such a bookmark has never been offered to
    // a server under any identity.
    bookmark.boDatetime = obj["bo_dt"] | "";
    bookmark.boSynced = obj["bo_synced"] | false;
    bookmark.boOwner = obj["bo_owner"] | "";
  }

  LOG_DBG("BKM", "Loaded %zu bookmarks from file", bookmarks.size());
  return true;
}

bool BookmarkFile::save(const std::string& bookPath, const std::vector<BookmarkEntry>& bookmarks) {
  JsonDocument doc;
  JsonArray arr = doc["bookmarks"].to<JsonArray>();
  LOG_DBG("BKM", "Saving %zu bookmarks to file", bookmarks.size());
  for (const auto& bookmark : bookmarks) {
    JsonObject obj = arr.add<JsonObject>();
    obj["xpath"] = bookmark.xpath;
    obj["percentage"] = bookmark.percentage;
    obj["summary"] = bookmark.summary;
    obj["si"] = bookmark.computedSpineIndex;
    obj["pc"] = bookmark.computedChapterPageCount;
    obj["pp"] = bookmark.computedChapterProgress;
    // Omit entirely rather than write an empty string/false: keeps a bookmark file untouched by
    // this feature identical to one written before it existed, for a device with no BookOrbit
    // credentials configured.
    if (!bookmark.boDatetime.empty()) obj["bo_dt"] = bookmark.boDatetime;
    if (bookmark.boSynced) obj["bo_synced"] = true;
    if (!bookmark.boOwner.empty()) obj["bo_owner"] = bookmark.boOwner;
  }

  // The checked document writer ensures /.crosspoint; the bookmarks subdirectory is ours.
  Storage.mkdir(BookmarkUtil::getBookmarksDir().c_str());
  const std::string path = BookmarkUtil::getBookmarkPath(bookPath);
  const std::string tmpPath = path + ".tmp";
  const std::string bakPath = path + ".bak";

  // save() is also a recovery entry point. The interactive reader can retain bookmarks in
  // memory across the reset/re-entry path and save without a fresh load immediately beforehand;
  // leaving a stale .bak in place would make SdFat reject the live -> .bak rename because its
  // destination must not already exist. If recovery itself cannot clear/restore the backup,
  // refuse this save rather than overwrite or strand the only known-complete copy.
  if (!recoverInterruptedSave(path)) return false;

  // Write to a temp path first, then promote it into place with the live file staged aside as
  // .bak (same ordering as BookOrbitStatsQueue::promoteReplacement() and
  // BookOrbitCatalogClient::downloadFile(), and for the same reason: HalStorage's underlying
  // SDCardManager::writeFile() removes the existing file and writes the new one IN PLACE at the
  // same path, with no staging of its own -- see recoverInterruptedSave()'s doc comment above for
  // what that made possible). The checked writer also requires close()/sync success before the
  // first rename: at every instant at least one of path/.bak is therefore a KNOWN-DURABLE file, so
  // a rename failure or power loss here never leaves this book with no readable bookmark file at
  // all -- only, at worst, a stale one recoverInterruptedSave() resolves on the next load().
  if (!PersistableStoreBase::writeDocToFileChecked(tmpPath.c_str(), doc)) {
    LOG_ERR("BKM", "Could not write temp bookmark file");
    Storage.remove(tmpPath.c_str());
    return false;
  }
  const HalStorage::PathStatus liveStatus = Storage.probePath(path.c_str());
  if (liveStatus == HalStorage::PathStatus::Error) {
    LOG_ERR("BKM", "Could not determine whether the live bookmark file exists before promotion");
    Storage.remove(tmpPath.c_str());
    return false;
  }
  const bool stagedExisting = liveStatus == HalStorage::PathStatus::Present;
  if (stagedExisting && !Storage.rename(path.c_str(), bakPath.c_str())) {
    LOG_ERR("BKM", "Could not stage existing bookmark file aside before promoting");
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (!Storage.rename(tmpPath.c_str(), path.c_str())) {
    LOG_ERR("BKM", "Could not promote bookmark file; restoring previous file if any");
    if (stagedExisting && !Storage.rename(bakPath.c_str(), path.c_str())) {
      LOG_ERR("BKM", "Could not restore staged-aside bookmark file; it remains at %s", bakPath.c_str());
    }
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (stagedExisting) Storage.remove(bakPath.c_str());
  return true;
}
