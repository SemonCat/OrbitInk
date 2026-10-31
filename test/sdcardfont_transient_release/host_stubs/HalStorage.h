#pragma once
// Host-test stand-in for lib/hal/HalStorage.h. SdCardFont.cpp only ever calls
// Storage.openFileForRead(module, path, HalFile&) and, on the returned
// HalFile, seekSet()/read()/close(). This stub backs those calls with an
// in-memory byte buffer registered by the test via registerTestFile(),
// instead of the real SdFat-backed implementation (which needs an actual SD
// card and the ESP32 Arduino framework, neither available on host).
//
// This is intentionally NOT a faithful reproduction of the real HalFile's
// PIMPL/mutex machinery -- it only needs to be source-compatible with the
// narrow subset of the interface SdCardFont.cpp actually uses.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class HalFile {
 public:
  HalFile() = default;

  bool seekSet(size_t offset) {
    if (!data_) return false;
    if (offset > data_->size()) return false;
    pos_ = offset;
    return true;
  }

  int read(void* buf, size_t count) {
    if (!data_) return -1;
    const size_t avail = data_->size() > pos_ ? data_->size() - pos_ : 0;
    const size_t n = count < avail ? count : avail;
    if (n > 0) memcpy(buf, data_->data() + pos_, n);
    pos_ += n;
    readCount_++;
    return static_cast<int>(n);
  }

  size_t fileSize() const { return data_ ? data_->size() : 0; }

  bool close() {
    data_.reset();
    pos_ = 0;
    return true;
  }

  // Test-only: bind this handle to an in-memory file's bytes.
  void bindForTest(std::shared_ptr<std::vector<uint8_t>> data) {
    data_ = std::move(data);
    pos_ = 0;
  }

  static void resetReadCount() { readCount_ = 0; }
  static size_t readCount() { return readCount_; }

 private:
  std::shared_ptr<std::vector<uint8_t>> data_;
  size_t pos_ = 0;
  inline static size_t readCount_ = 0;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }

  bool openFileForRead(const char* /*moduleName*/, const char* path, HalFile& file) {
    const auto it = files_.find(path);
    if (it == files_.end()) return false;
    file.bindForTest(it->second);
    return true;
  }

  // Test-only: register an in-memory file the stub will serve for `path`.
  static void registerTestFile(const std::string& path, std::vector<uint8_t> bytes) {
    getInstance().files_[path] = std::make_shared<std::vector<uint8_t>>(std::move(bytes));
  }

  // Test-only: clear all registered files between tests.
  static void resetTestFiles() {
    getInstance().files_.clear();
    HalFile::resetReadCount();
  }

  static size_t readCount() { return HalFile::readCount(); }
  static void resetReadCount() { HalFile::resetReadCount(); }

 private:
  std::unordered_map<std::string, std::shared_ptr<std::vector<uint8_t>>> files_;
};

#define Storage HalStorage::getInstance()
