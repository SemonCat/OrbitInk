#pragma once
// Host-test stand-in for lib/hal/HalStorage.h, sized to what BookmarkFile.cpp and
// PersistableStore.cpp actually need for BookmarkFile's own atomic save/load: writeFile/readFile
// plus a checked HalFile write/close path (the whole-document APIs BookmarkFile.cpp reaches
// through PersistableStoreBase), and probePath/rename/remove/mkdir for the temp+backup+promote
// staging BookmarkFile::save()/recoverInterruptedSave() do directly.
//
// In-memory, read-write, with injectable failures so the atomicity paths (a write that never
// completes, a rename that fails partway through the promote sequence) are actually testable --
// same rationale as test/bookorbit_stats/host_stubs/HalStorage.h, which this stub's shape mirrors
// for the exists/rename/remove trio.

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>

#include "Arduino.h"

struct TestBookmarkStorageState {
  std::map<std::string, std::string> files;
  bool failWriteFile = false;  // writeFile() reports failure without touching the map
  bool failClose = false;      // close() reports that the just-written bytes were not durable
  bool failRenames = false;
  bool failRemoves = false;
  std::set<std::string> failPathProbes;
  size_t renameCalls = 0;
  size_t failRenameOnCall = 0;

  void reset() {
    files.clear();
    failWriteFile = false;
    failClose = false;
    failRenames = false;
    failRemoves = false;
    failPathProbes.clear();
    renameCalls = 0;
    failRenameOnCall = 0;
  }
};

inline TestBookmarkStorageState& testBookmarkStorage() {
  static TestBookmarkStorageState state;
  return state;
}

class HalFile : public Print {
 public:
  HalFile() = default;
  explicit HalFile(std::string path) : path_(std::move(path)), open_(true) {
    testBookmarkStorage().files[path_].clear();
  }
  HalFile(HalFile&&) = default;
  HalFile& operator=(HalFile&&) = default;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* buffer, size_t size) override {
    if (!open_ || testBookmarkStorage().failWriteFile) return 0;
    testBookmarkStorage().files[path_].append(reinterpret_cast<const char*>(buffer), size);
    return size;
  }
  bool close() {
    open_ = false;
    return !testBookmarkStorage().failClose;
  }

 private:
  std::string path_;
  bool open_ = false;
};

class HalStorage {
 public:
  enum class PathStatus : uint8_t { Present, Absent, Error };

  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }

  bool mkdir(const char*, bool = true) { return true; }

  bool exists(const char* path) { return testBookmarkStorage().files.count(path) != 0; }

  PathStatus probePath(const char* path) {
    if (testBookmarkStorage().failPathProbes.count(path) != 0) return PathStatus::Error;
    return exists(path) ? PathStatus::Present : PathStatus::Absent;
  }

  bool openFileForWrite(const char*, const char* path, HalFile& file) {
    file = HalFile(path);
    return true;
  }

  bool writeFile(const char* path, const String& content) {
    if (testBookmarkStorage().failWriteFile) return false;
    testBookmarkStorage().files[path] = std::string(content.c_str());
    return true;
  }

  String readFile(const char* path) {
    const auto it = testBookmarkStorage().files.find(path);
    if (it == testBookmarkStorage().files.end()) return String();
    return String(it->second);
  }

  bool remove(const char* path) {
    if (testBookmarkStorage().failRemoves) return false;
    return testBookmarkStorage().files.erase(path) != 0;
  }

  bool rename(const char* oldPath, const char* newPath) {
    auto& state = testBookmarkStorage();
    state.renameCalls++;
    if (state.failRenames || (state.failRenameOnCall != 0 && state.renameCalls == state.failRenameOnCall)) {
      return false;
    }
    auto& files = state.files;
    const auto it = files.find(oldPath);
    // Matches SdFat's FsVolume::rename contract: the destination must not already exist.
    if (it == files.end() || files.count(newPath) != 0) return false;
    files[newPath] = it->second;
    files.erase(it);
    return true;
  }
};

#define Storage HalStorage::getInstance()
