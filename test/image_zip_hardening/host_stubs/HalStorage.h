#pragma once
// Host-test stand-in for lib/hal/HalStorage.h, sized to what ZipFile.cpp/.h and
// ImageToFramebufferDecoder.h actually need: HalFile::seek/seekCur/available/read/size/close, and
// HalStorage::openFileForRead. Not a faithful PIMPL/mutex reproduction -- see
// test/sdcardfont_transient_release/host_stubs/HalStorage.h for the sibling stub this one extends
// with seekCur/available/size and, the reason this stub exists at all, injectable read() failures
// (registerTestFile's failAtOffset) so a test can model a genuine SD read error mid-stream.
//
// Also provides a minimal Print base (ZipFile.h declares
// readFileToStream(..., Print&, ...); it must be a complete type for ZipFile.cpp to compile even
// though these tests only exercise readFileToMemory()).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class Print {
 public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t) { return 0; }
  virtual size_t write(const uint8_t* buffer, size_t size) {
    size_t n = 0;
    for (size_t i = 0; i < size; i++) {
      if (write(buffer[i]))
        n++;
      else
        break;
    }
    return n;
  }
};

struct TestFileBacking {
  std::vector<uint8_t> bytes;
  // Byte offset (within this file's own bytes) at which read() starts reporting an I/O error (-1)
  // instead of real data. SIZE_MAX (default) means "never fail."
  size_t failAtOffset = SIZE_MAX;
};

class HalFile {
 public:
  HalFile() = default;

  explicit operator bool() const { return backing_ != nullptr; }

  size_t size() { return backing_ ? backing_->bytes.size() : 0; }

  bool seek(size_t pos) {
    if (!backing_ || pos > backing_->bytes.size()) return false;
    pos_ = pos;
    return true;
  }

  bool seekCur(int64_t offset) {
    if (!backing_) return false;
    const int64_t newPos = static_cast<int64_t>(pos_) + offset;
    if (newPos < 0 || static_cast<size_t>(newPos) > backing_->bytes.size()) return false;
    pos_ = static_cast<size_t>(newPos);
    return true;
  }

  int available() const {
    if (!backing_ || pos_ >= backing_->bytes.size()) return 0;
    return static_cast<int>(backing_->bytes.size() - pos_);
  }

  size_t position() const { return pos_; }

  // Test-only failure injection: once the read cursor reaches failAtOffset, report an I/O error
  // (-1) instead of real bytes -- exactly what ZipFile.cpp's zipFillCallback() must handle
  // without casting straight to size_t (the bug this test target exists to catch).
  int read(void* buf, size_t count) {
    if (!backing_) return -1;
    if (pos_ >= backing_->failAtOffset) return -1;
    size_t avail = backing_->bytes.size() > pos_ ? backing_->bytes.size() - pos_ : 0;
    size_t n = count < avail ? count : avail;
    if (pos_ + n > backing_->failAtOffset) n = backing_->failAtOffset - pos_;
    if (n > 0) memcpy(buf, backing_->bytes.data() + pos_, n);
    pos_ += n;
    return static_cast<int>(n);
  }

  bool close() {
    backing_.reset();
    pos_ = 0;
    return true;
  }

  // Test-only: bind this handle to an in-memory file's bytes.
  void bindForTest(std::shared_ptr<TestFileBacking> backing) {
    backing_ = std::move(backing);
    pos_ = 0;
  }

 private:
  std::shared_ptr<TestFileBacking> backing_;
  size_t pos_ = 0;
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
  bool openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
    return openFileForRead(moduleName, path.c_str(), file);
  }

  // Test-only: register an in-memory file the stub will serve for `path`. `failAtOffset` (default
  // SIZE_MAX = never) models an SD read error once the read cursor reaches that byte.
  static void registerTestFile(const std::string& path, std::vector<uint8_t> bytes, size_t failAtOffset = SIZE_MAX) {
    auto backing = std::make_shared<TestFileBacking>();
    backing->bytes = std::move(bytes);
    backing->failAtOffset = failAtOffset;
    getInstance().files_[path] = std::move(backing);
  }

  // Test-only: clear all registered files between tests.
  static void resetTestFiles() { getInstance().files_.clear(); }

 private:
  std::unordered_map<std::string, std::shared_ptr<TestFileBacking>> files_;
};

#define Storage HalStorage::getInstance()
