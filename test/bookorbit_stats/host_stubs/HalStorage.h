#pragma once
// Host-test stand-in for lib/hal/HalStorage.h, sized to what BookOrbitStatsQueue.cpp needs.
//
// Unlike the read-only stubs in test/image_zip_hardening/ and
// test/sdcardfont_transient_release/, this one is READ-WRITE: the queue's whole contract is
// about durability -- append, drop-oldest rewrite via temp-file-then-rename, and surviving
// corrupt or torn files -- so a stub that cannot write could only test half of it. Files live
// in an in-memory map, which additionally lets a test hand-craft a byte-exact corrupt or
// truncated file that no real SD sequence could be relied upon to produce.
//
// Not a faithful reproduction of the real PIMPL/mutex implementation; it models the observable
// behaviour of the handful of calls the queue makes.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

using oflag_t = int;
constexpr oflag_t O_RDONLY = 0x01;
constexpr oflag_t O_WRONLY = 0x02;
constexpr oflag_t O_CREAT = 0x04;
constexpr oflag_t O_APPEND = 0x08;

// Minimal Print base: HalFile derives from Print in the real header.
class Print {
 public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t) { return 0; }
  virtual size_t write(const uint8_t*, size_t) { return 0; }
};

// The whole in-memory filesystem, plus injectable failure switches so error paths are testable.
struct TestStorageState {
  std::map<std::string, std::vector<uint8_t>> files;
  bool failWrites = false;     // every write() reports a short write
  bool failOpenWrite = false;  // openFileForWrite/open-for-write returns a closed handle
  bool failOpenRead = false;   // openFileForRead fails even though the file exists
  // Paths whose tri-state probe must report an I/O error rather than present/absent. This models
  // SdFat's boolean exists() ambiguity without making the rest of the fake filesystem unreadable.
  std::set<std::string> failPathProbes;
  // Models a failed final writeback: close() -- whose bool is the underlying SdFat sync()
  // result -- reports failure even though the bytes are visible in this in-memory map. That
  // asymmetry is deliberate: it is exactly the state a real card leaves when the data never
  // reached durable storage, and it is what a caller relying on void flush() cannot detect.
  bool failFlush = false;
  bool failRenames = false;  // every rename() fails
  bool failRemoves = false;  // every remove() fails
  // Fail only the Nth rename (1-based), letting a test stop the replacement protocol at an exact
  // stage -- 1 = could not stage the live queue aside, 2 = could not promote the replacement.
  int failRenameAt = 0;
  int renameCount = 0;
  // Byte offset past which read() reports a SHORT read (returns fewer bytes, then 0), modelling
  // a truncated file.
  size_t failReadAfter = SIZE_MAX;
  // Byte offset at or past which read() reports an I/O ERROR (-1). Distinct from the above: a
  // short read is "less data than asked for", an error is "the card failed", and the two must
  // not be conflated by anything under test.
  size_t failReadErrorAfter = SIZE_MAX;
  std::vector<std::string> directoriesEnsured;
  // Paths explicitly known as directories, whether or not they hold any files yet -- lets a
  // directory-scan caller (see BookOrbitMigration.cpp) see an existing-but-empty directory rather
  // than treating an as-yet-unpopulated one as absent. ensureDirectoryExists() also adds here.
  std::set<std::string> directoriesKnown;
  // Models a transient failure to OPEN a directory that is known to exist (a real SD hiccup, not
  // absence) -- see BookOrbitMigration.cpp's own absent-vs-unreadable distinction. exists() still
  // reports the directory present (matching real SdFat, whose exists() checks any directory
  // entry, file or directory); only the directory-open() overload fails.
  bool failOpenDir = false;
  // Fail directory enumeration after this many successfully returned entries. The directory
  // handle keeps an error bit so callers can distinguish this from clean end-of-directory.
  size_t failDirectoryReadAfter = SIZE_MAX;

  void reset() {
    files.clear();
    failWrites = false;
    failOpenWrite = false;
    failOpenRead = false;
    failPathProbes.clear();
    failFlush = false;
    failRenames = false;
    failRemoves = false;
    failRenameAt = 0;
    renameCount = 0;
    failReadAfter = SIZE_MAX;
    failReadErrorAfter = SIZE_MAX;
    directoriesEnsured.clear();
    directoriesKnown.clear();
    failOpenDir = false;
    failDirectoryReadAfter = SIZE_MAX;
  }
};

inline TestStorageState& testStorage() {
  static TestStorageState state;
  return state;
}

class HalFile : public Print {
 public:
  HalFile() = default;
  HalFile(HalFile&&) = default;
  HalFile& operator=(HalFile&&) = default;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  // Constructed open by HalStorage below.
  HalFile(std::string path, bool writable, bool append) : path_(std::move(path)), open_(true), writable_(writable) {
    auto& files = testStorage().files;
    if (writable && !append) files[path_].clear();
    if (files.find(path_) == files.end()) files[path_] = {};
    pos_ = append ? files[path_].size() : 0;
  }

  // Directory-handle constructor: `children` are the direct child basenames (already computed by
  // HalStorage::open()'s directory scan, in the order the in-memory map iterates them).
  HalFile(std::string path, std::vector<std::string> children)
      : path_(std::move(path)), open_(true), isDirectory_(true), dirChildren_(std::move(children)) {}

  bool isOpen() const { return open_; }
  explicit operator bool() const { return open_; }
  bool isDirectory() const { return isDirectory_; }
  void rewindDirectory() { dirPos_ = 0; }

  // Only ever meaningful on a directory handle. Each returned entry knows only its own name
  // (see getName()) -- BookOrbitMigration.cpp's scan never reads through it, it re-opens the
  // full path via Storage.open()/openFileForRead() for the actual file contents.
  HalFile openNextFile() {
    if (!isDirectory_) return HalFile{};
    if (dirPos_ >= testStorage().failDirectoryReadAfter) {
      error_ = 1;
      return HalFile{};
    }
    if (dirPos_ >= dirChildren_.size()) return HalFile{};
    HalFile child;
    child.open_ = true;
    child.path_ = dirChildren_[dirPos_++];
    return child;
  }

  uint8_t getError() const { return error_; }

  size_t getName(char* buf, const size_t len) {
    if (!buf || len == 0) return 0;
    const size_t n = std::min(path_.size(), len - 1);
    memcpy(buf, path_.data(), n);
    buf[n] = '\0';
    return n;
  }

  size_t size() { return fileSize(); }
  size_t fileSize() {
    if (!open_) return 0;
    return testStorage().files[path_].size();
  }

  bool seekSet(size_t offset) {
    if (!open_ || offset > fileSize()) return false;
    pos_ = offset;
    return true;
  }
  bool seek(size_t offset) { return seekSet(offset); }

  int read(void* buf, size_t count) {
    if (!open_) return -1;
    if (pos_ >= testStorage().failReadErrorAfter) return -1;
    const auto& bytes = testStorage().files[path_];
    if (pos_ >= bytes.size()) return 0;
    size_t available = bytes.size() - pos_;
    if (testStorage().failReadAfter != SIZE_MAX && pos_ < testStorage().failReadAfter) {
      available = std::min(available, testStorage().failReadAfter - pos_);
    } else if (testStorage().failReadAfter != SIZE_MAX) {
      return 0;
    }
    const size_t n = std::min(count, available);
    memcpy(buf, bytes.data() + pos_, n);
    pos_ += n;
    return static_cast<int>(n);
  }

  size_t write(const void* buf, size_t count) {
    if (!open_ || !writable_ || testStorage().failWrites) return 0;
    auto& bytes = testStorage().files[path_];
    if (pos_ + count > bytes.size()) bytes.resize(pos_ + count);
    memcpy(bytes.data() + pos_, buf, count);
    pos_ += count;
    return count;
  }
  size_t write(const uint8_t* buf, size_t count) override { return write(static_cast<const void*>(buf), count); }
  size_t write(uint8_t b) override { return write(&b, 1); }

  // Deliberately a no-op returning nothing, matching HalFile: it is not a durability signal.
  void flush() {}
  bool close() {
    const bool synced = !testStorage().failFlush;
    open_ = false;
    return synced;
  }

 private:
  std::string path_;
  size_t pos_ = 0;
  bool open_ = false;
  bool writable_ = false;
  bool isDirectory_ = false;
  std::vector<std::string> dirChildren_;
  size_t dirPos_ = 0;
  uint8_t error_ = 0;
};

class HalStorage {
 public:
  enum class PathStatus : uint8_t { Present, Absent, Error };

  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }

  // The in-memory map has no real directories; recording the call is enough for the queue's
  // "make sure the stats directory exists" step, and lets open(path) below recognize an existing
  // but still-empty directory (see BookOrbitMigration.cpp's scan).
  bool ensureDirectoryExists(const char* path) {
    testStorage().directoriesEnsured.push_back(path);
    testStorage().directoriesKnown.insert(path);
    return true;
  }

  // Matches real SdFat's exists(), which reports any directory entry -- file OR directory -- not
  // just files: a plain files-map lookup alone would report every directory absent (nothing is
  // ever stored under its own path as a "file"), which BookOrbitMigration.cpp's absent-vs-
  // unreadable distinction depends on getting right.
  bool exists(const char* path) {
    if (testStorage().files.count(path) != 0) return true;
    return isKnownDirectory(path);
  }

  PathStatus probePath(const char* path) {
    if (testStorage().failPathProbes.count(path) != 0) return PathStatus::Error;
    return exists(path) ? PathStatus::Present : PathStatus::Absent;
  }

  bool remove(const char* path) {
    if (testStorage().failRemoves) return false;
    return testStorage().files.erase(path) != 0;
  }

  bool rename(const char* oldPath, const char* newPath) {
    auto& state = testStorage();
    state.renameCount++;
    if (state.failRenames) return false;
    if (state.failRenameAt != 0 && state.renameCount == state.failRenameAt) return false;
    auto& files = state.files;
    const auto it = files.find(oldPath);
    if (it == files.end()) return false;
    files[newPath] = it->second;
    files.erase(it);
    return true;
  }

  HalFile open(const char* path, oflag_t oflag) {
    const bool writable = (oflag & O_WRONLY) != 0;
    if (writable && testStorage().failOpenWrite) return HalFile{};
    if (!writable && !exists(path)) return HalFile{};
    if (writable && !exists(path) && (oflag & O_CREAT) == 0) return HalFile{};
    return HalFile{path, writable, (oflag & O_APPEND) != 0};
  }

  // Directory-open overload, for a directory-scan caller (BookOrbitMigration.cpp). Synthesizes
  // directory semantics over the flat in-memory file map: `path` is a known directory if it was
  // ensureDirectoryExists()'d, or if any stored file's key has it as an immediate parent -- the
  // common case, since nothing in this stub ever calls ensureDirectoryExists() before a queue
  // file already exists. Returns a closed HalFile (falsy, isDirectory() == false) for anything
  // else, matching SdFat's own open() on a nonexistent path -- OR when failOpenDir is set, to
  // model a transient failure to open a directory that genuinely exists (exists() still reports
  // it present; only this open() call fails, exactly as a real SD read glitch would).
  HalFile open(const char* path) {
    if (testStorage().failOpenDir) return HalFile{};
    const std::string dir = path;
    const std::vector<std::string> children = childrenOf(dir);
    if (children.empty() && testStorage().directoriesKnown.count(dir) == 0) return HalFile{};
    return HalFile{dir, children};
  }

  bool openFileForRead(const char*, const std::string& path, HalFile& file) {
    if (testStorage().failOpenRead) return false;
    if (!exists(path.c_str())) return false;
    file = HalFile{path, false, false};
    return true;
  }

  bool openFileForWrite(const char*, const std::string& path, HalFile& file) {
    if (testStorage().failOpenWrite) return false;
    file = HalFile{path, true, false};
    return true;
  }

 private:
  static std::vector<std::string> childrenOf(const std::string& dir) {
    const std::string prefix = dir + "/";
    std::vector<std::string> children;
    for (const auto& [key, bytes] : testStorage().files) {
      (void)bytes;
      if (key.size() <= prefix.size() || key.compare(0, prefix.size(), prefix) != 0) continue;
      const std::string rest = key.substr(prefix.size());
      if (rest.find('/') != std::string::npos) continue;  // not a direct child
      children.push_back(rest);
    }
    return children;
  }

  static bool isKnownDirectory(const std::string& path) {
    if (testStorage().directoriesKnown.count(path) != 0) return true;
    return !childrenOf(path).empty();
  }
};

#define Storage HalStorage::getInstance()
