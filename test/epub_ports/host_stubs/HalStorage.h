#pragma once

// Host-only in-memory replacement for lib/hal/HalStorage.h.  Dictionary.cpp
// uses only the small random-access file surface below; keeping the backing
// store injectable lets the tests exercise stale sidecars and SD failures
// without depending on an ESP32 or an actual card.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <Print.h>

using oflag_t = unsigned;
constexpr oflag_t O_RDONLY = 0x01;
constexpr oflag_t O_WRITE = 0x02;
constexpr oflag_t O_CREAT = 0x04;
constexpr oflag_t O_TRUNC = 0x08;

class HalFile : public Print {
 public:
  HalFile() = default;
  HalFile(HalFile&&) noexcept = default;
  HalFile& operator=(HalFile&&) noexcept = default;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  explicit operator bool() const { return backing_ != nullptr; }
  bool isDirectory() const { return backing_ && backing_->directory; }
  void rewindDirectory() { child_ = 0; }
  HalFile openNextFile() {
    if (!backing_ || child_ >= backing_->children.size()) return {};
    return HalFile(backing_->children[child_++], false);
  }
  bool getName(char* out, size_t size) const {
    if (!backing_ || size == 0) return false;
    const auto slash = backing_->name.find_last_of('/');
    const std::string name = backing_->name.substr(slash == std::string::npos ? 0 : slash + 1);
    std::strncpy(out, name.c_str(), size - 1);
    out[size - 1] = 0;
    return name.size() < size;
  }
  bool getModifyTime(uint16_t& date, uint16_t& time) const { date = time = 0; return false; }

  size_t fileSize() const { return backing_ ? backing_->bytes.size() : 0; }
  size_t size() const { return fileSize(); }
  bool isOpen() const { return backing_ != nullptr; }
  int available() const { return size() - position_; }
  bool flush() { return true; }
  bool seekCur(int offset) { return seekSet(position_ + offset); }
  size_t position() const { return position_; }

  bool seek(size_t offset) { return seekSet(offset); }
  bool seekSet(size_t offset) {
    if (!backing_ || backing_->failSeek || offset > backing_->bytes.size()) return false;
    position_ = offset;
    return true;
  }

  int read(void* destination, size_t count) {
    if (!backing_) return -1;
    if (position_ >= backing_->failReadAt) return -1;
    const size_t available = backing_->bytes.size() > position_ ? backing_->bytes.size() - position_ : 0;
    size_t amount = count < available ? count : available;
    if (position_ + amount > backing_->failReadAt) amount = backing_->failReadAt - position_;
    if (amount != 0) std::memcpy(destination, backing_->bytes.data() + position_, amount);
    position_ += amount;
    backing_->bytesRead += amount;
    return static_cast<int>(amount);
  }

  int read() {
    if (!backing_) return -1;
    if (position_ >= backing_->failReadAt || position_ >= backing_->bytes.size()) return -1;
    const int value = backing_->bytes[position_++];
    backing_->bytesRead++;
    return value;
  }

  size_t write(const void* source, size_t count) {
    if (!backing_ || !writable_) return 0;
    if (position_ >= backing_->failWriteAt) return 0;
    size_t amount = count;
    if (position_ + amount > backing_->failWriteAt) amount = backing_->failWriteAt - position_;
    if (position_ + amount > backing_->bytes.size()) backing_->bytes.resize(position_ + amount);
    if (amount != 0) std::memcpy(backing_->bytes.data() + position_, source, amount);
    position_ += amount;
    return amount;
  }

  size_t write(const uint8_t* source, size_t amount) override { return write(static_cast<const void*>(source), amount); }
  size_t write(uint8_t byte) override { return write(&byte, sizeof(byte)); }

  bool close() {
    const bool ok = !backing_ || !backing_->failClose;
    backing_.reset();
    position_ = 0;
    writable_ = false;
    return ok;
  }

 private:
  struct Backing {
    std::string name;
    bool directory = false;
    std::vector<std::shared_ptr<Backing>> children;
    std::vector<uint8_t> bytes;
    size_t failReadAt = std::numeric_limits<size_t>::max();
    size_t failWriteAt = std::numeric_limits<size_t>::max();
    size_t bytesRead = 0;
    bool failSeek = false;
    bool failClose = false;
  };

  friend class HalStorage;

  explicit HalFile(std::shared_ptr<Backing> backing, bool writable)
      : backing_(std::move(backing)), writable_(writable) {}

  std::shared_ptr<Backing> backing_;
  size_t position_ = 0;
  bool writable_ = false;
  size_t child_ = 0;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }

  bool mkdir(const char*, bool = true) { ++mkdirCalls_; return true; }
  bool removeDir(const char* path) {
    ++removeDirCalls_;
    const std::string prefix = std::string(path) + "/";
    for (auto it = files_.begin(); it != files_.end();) {
      if (it->first == path || it->first.rfind(prefix, 0) == 0) it = files_.erase(it);
      else ++it;
    }
    return true;
  }
  bool exists(const std::string& path) const { return exists(path.c_str()); }
  bool exists(const char* path) const { return path != nullptr && files_.find(path) != files_.end(); }

  bool openFileForRead(const char* /*moduleName*/, const char* path, HalFile& file) {
    const std::string key = path ? path : "";
    readOpenAttempts_[key]++;
    const auto it = files_.find(key);
    if (it == files_.end()) return false;
    file = HalFile(it->second, false);
    return true;
  }

  bool openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
    return openFileForRead(moduleName, path.c_str(), file);
  }

  bool openFileForWrite(const char* /*moduleName*/, const char* path, HalFile& file) {
    const std::string key = path ? path : "";
    writeOpenAttempts_[key]++;
    if (writeOpenFailures_.find(key) != writeOpenFailures_.end()) return false;
    auto backing = std::make_shared<HalFile::Backing>();
    if (const auto it = writeFailureOffsets_.find(key); it != writeFailureOffsets_.end()) {
      backing->failWriteAt = it->second;
    }
    backing->failClose = closeFailures_.find(key) != closeFailures_.end();
    files_[key] = backing;
    file = HalFile(std::move(backing), true);
    return true;
  }

  bool openFileForWrite(const char* moduleName, const std::string& path, HalFile& file) {
    return openFileForWrite(moduleName, path.c_str(), file);
  }

  HalFile open(const char* path, oflag_t flags = O_RDONLY) {
    HalFile file;
    if ((flags & O_WRITE) != 0) {
      openFileForWrite("TEST", path, file);
    } else {
      openFileForRead("TEST", path, file);
    }
    return file;
  }

  bool remove(const char* path) { return files_.erase(path ? path : "") != 0; }

  bool rename(const char* oldPath, const char* newPath) {
    const std::string oldKey = oldPath ? oldPath : "";
    const std::string newKey = newPath ? newPath : "";
    const auto it = files_.find(oldKey);
    if (it == files_.end() || files_.find(newKey) != files_.end()) return false;
    files_.emplace(newKey, std::move(it->second));
    files_.erase(it);
    return true;
  }

  // Test controls and observations.
  static void resetTestFiles() {
    auto& storage = getInstance();
    storage.files_.clear();
    storage.readOpenAttempts_.clear();
    storage.writeOpenAttempts_.clear();
    storage.writeOpenFailures_.clear();
    storage.writeFailureOffsets_.clear();
    storage.closeFailures_.clear();
    storage.mkdirCalls_ = storage.removeDirCalls_ = 0;
  }

  static void registerTestFile(const std::string& path, std::vector<uint8_t> bytes) {
    auto& storage = getInstance();
    auto backing = std::make_shared<HalFile::Backing>();
    backing->name = path;
    backing->bytes = std::move(bytes);
    storage.files_[path] = std::move(backing);
  }
  static void registerTestDirectory(const std::string& path, const std::vector<std::string>& children) {
    auto& storage = getInstance();
    auto backing = std::make_shared<HalFile::Backing>();
    backing->name = path;
    backing->directory = true;
    for (const auto& child : children) backing->children.push_back(storage.files_.at(child));
    storage.files_[path] = std::move(backing);
  }
  static size_t mkdirCalls() { return getInstance().mkdirCalls_; }
  static size_t removeDirCalls() { return getInstance().removeDirCalls_; }

  static bool hasTestFile(const std::string& path) { return getInstance().exists(path.c_str()); }

  static std::vector<uint8_t> readTestFile(const std::string& path) {
    const auto it = getInstance().files_.find(path);
    return it == getInstance().files_.end() ? std::vector<uint8_t>{} : it->second->bytes;
  }

  static size_t readOpenAttempts(const std::string& path) {
    const auto it = getInstance().readOpenAttempts_.find(path);
    return it == getInstance().readOpenAttempts_.end() ? 0 : it->second;
  }

  static size_t writeOpenAttempts(const std::string& path) {
    const auto it = getInstance().writeOpenAttempts_.find(path);
    return it == getInstance().writeOpenAttempts_.end() ? 0 : it->second;
  }

  static size_t bytesRead(const std::string& path) {
    const auto it = getInstance().files_.find(path);
    return it == getInstance().files_.end() ? 0 : it->second->bytesRead;
  }

  static void setReadFailure(const std::string& path, size_t offset) {
    if (const auto it = getInstance().files_.find(path); it != getInstance().files_.end())
      it->second->failReadAt = offset;
  }

  static void setWriteFailure(const std::string& path, size_t offset) {
    getInstance().writeFailureOffsets_[path] = offset;
    if (const auto it = getInstance().files_.find(path); it != getInstance().files_.end())
      it->second->failWriteAt = offset;
  }

  static void setSeekFailure(const std::string& path, bool fail) {
    if (const auto it = getInstance().files_.find(path); it != getInstance().files_.end()) it->second->failSeek = fail;
  }

  static void setCloseFailure(const std::string& path, bool fail) {
    if (fail) {
      getInstance().closeFailures_.insert(path);
    } else {
      getInstance().closeFailures_.erase(path);
    }
    if (const auto it = getInstance().files_.find(path); it != getInstance().files_.end()) it->second->failClose = fail;
  }

  static void failWriteOpen(const std::string& path) { getInstance().writeOpenFailures_.insert(path); }

  static void allowWriteOpen(const std::string& path) { getInstance().writeOpenFailures_.erase(path); }

 private:
  size_t mkdirCalls_ = 0;
  size_t removeDirCalls_ = 0;
  std::unordered_map<std::string, std::shared_ptr<HalFile::Backing>> files_;
  std::unordered_map<std::string, size_t> readOpenAttempts_;
  std::unordered_map<std::string, size_t> writeOpenAttempts_;
  std::unordered_set<std::string> writeOpenFailures_;
  std::unordered_map<std::string, size_t> writeFailureOffsets_;
  std::unordered_set<std::string> closeFailures_;
};

#define Storage HalStorage::getInstance()
