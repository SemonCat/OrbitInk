#pragma once

// Owned simulator adapter: superset shadow of crosspoint-simulator's HalStorage/HalFile.
//
// WHY THIS FILE EXISTS
// The simulator replaces the firmware HAL wholesale (`lib_ignore = hal`), so on the host
// `#include <HalStorage.h>` resolves here instead of lib/hal/HalStorage.h. CrossFade's storage HAL
// carries three members the pinned simulator does not, and firmware that boots on the host calls
// all of them:
//   HalStorage::PathStatus + HalStorage::probePath()  -- src/util/BookmarkFile.cpp,
//       src/util/BookOrbitBookmarkSync.cpp, src/util/BookOrbitMigration.cpp,
//       lib/KOReaderSync/BookOrbitCatalogClient.cpp
//   HalFile::getError()                               -- src/util/BookOrbitMigration.cpp
//   HalFile::getModifyTime()                          -- src/util/LibraryScanner.cpp
// Members cannot be added to a class from outside it, and the simulator checkout under
// .pio/libdeps must not be patched -- so this header declares both classes as the pinned simulator
// declares them, plus those members, which sim/CrossFadeSimHal/HalStorageCompat.cpp defines using
// only the simulator's own public API.
//
// MAINTENANCE CONTRACT
// Everything above each CROSSFADE ADDITIONS marker mirrors crosspoint-simulator@dacbbbc
// src/HalStorage.h declaration for declaration -- same members, same order, same inline bodies --
// but reformatted to this repo's clang-format style, so compare declarations rather than bytes.
// The simulator's own HalStorage.cpp includes its copy via a quoted include and keeps compiling
// against the original; this shadow is what the firmware sees. The two definitions must agree on
// data layout and on every inline body, because the objects themselves are constructed in the
// simulator's translation units. The exact dependency pin in platformio.ini is what keeps that
// true. When bumping the pin, re-diff this file against the new src/HalStorage.h before anything
// else.
//
// See sim/README.md.

#include <Print.h>
#include <Stream.h>
#include <common/FsApiConstants.h>  // for oflag_t
#include <freertos/semphr.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "WString.h"

class HalFile;

enum class UsbDriveState : uint8_t {
  Unsupported,
  WaitingForHost,
  Connected,
  Ejected,
  Disconnected,
  IoError,
};

class HalStorage {
 public:
  HalStorage();
  bool begin();
  bool ready() const;
  bool getSpace(uint64_t& totalBytes, uint64_t& freeBytes);
  void prepareForDeepSleep() {};
  bool beginUsbDrive();
  bool disconnectUsbDriveHost();
  void endUsbDrive();
  UsbDriveState usbDriveState() const;
  std::vector<String> listFiles(const char* path = "/", int maxFiles = 200);
  String readFile(const char* path);
  bool readFileToStream(const char* path, Print& out, size_t chunkSize = 256);
  size_t readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes = 0);
  bool writeFile(const char* path, const String& content);
  bool ensureDirectoryExists(const char* path);

  HalFile open(const char* path, const oflag_t oflag = O_RDONLY);
  bool mkdir(const char* path, const bool pFlag = true);
  bool exists(const char* path);
  bool remove(const char* path);
  bool rename(const char* oldPath, const char* newPath);
  bool rmdir(const char* path);

  bool openFileForRead(const char* moduleName, const char* path, HalFile& file);
  bool openFileForRead(const char* moduleName, const std::string& path, HalFile& file);
  bool openFileForRead(const char* moduleName, const String& path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const char* path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const std::string& path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const String& path, HalFile& file);
  bool removeDir(const char* path);

  static HalStorage& getInstance() { return instance; }

  class StorageLock;  // private class, used internally

  // ---- CROSSFADE ADDITIONS (not upstream; defined in HalStorageCompat.cpp) ----

  // Mirrors lib/hal/HalStorage.h. On device this distinguishes a confirmed-missing path from an
  // I/O failure, because SdFat's exists() reports false for both and destructive
  // recovery/migration decisions must not run on a read error.
  enum class PathStatus : uint8_t { Present, Absent, Error };
  PathStatus probePath(const char* path);

 private:
  static HalStorage instance;

  bool initialized = false;
  SemaphoreHandle_t storageMutex = nullptr;
};

#define Storage HalStorage::getInstance()

class HalFile : public Print {
  friend class HalStorage;
  class Impl;
  std::unique_ptr<Impl> impl;
  explicit HalFile(std::unique_ptr<Impl> impl);

 public:
  HalFile();
  ~HalFile();
  HalFile(HalFile&&);
  HalFile& operator=(HalFile&&);
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  void flush() override;
  size_t getName(char* name, size_t len);
  size_t size();
  size_t fileSize();
  uint64_t fileSize64();
  bool seek(size_t pos);
  bool seek64(uint64_t pos);
  bool seekCur(int64_t offset);
  bool seekSet(size_t offset);
  int available() const;
  size_t position() const;
  int read(void* buf, size_t count);
  int read();  // read a single byte
  size_t write(const void* buf, size_t count);
  size_t write(const uint8_t* buf, size_t count) override;
  size_t write(uint8_t b) override;
  bool sync();
  bool rename(const char* newPath);
  bool isDirectory() const;
  void rewindDirectory();
  bool close();
  HalFile openNextFile();
  bool isOpen() const;
  operator bool() const;

  // ---- CROSSFADE ADDITIONS (not upstream; defined in HalStorageCompat.cpp) ----

  // Mirrors lib/hal/HalStorage.h. Packed FAT date/time of this entry's last modification.
  bool getModifyTime(uint16_t& fatDate, uint16_t& fatTime) const;
  // Mirrors lib/hal/HalStorage.h. Sticky directory-enumeration error code; 0 means no error.
  uint8_t getError() const;
};

// Only do renaming FsFile to HalFile if this header is included by downstream
// code The renaming is to allow using the thread-safe HalFile instead of the
// raw FsFile, without needing to change the downstream code
#ifndef HAL_STORAGE_IMPL
using FsFile = HalFile;
#endif

// Layout guards -- see the note in HalClock.h for the reasoning. These pin the sizes of the
// declarations in this file so an accidental edit (especially one copied from lib/hal/HalStorage.h,
// which is a different class) fails the build instead of corrupting the objects the simulator's
// own translation unit allocates. They do not detect upstream drift; bumping the pin still requires
// the manual re-diff described at the top of this file.
static_assert(sizeof(HalStorage) == 16,
              "HalStorage layout changed: re-diff against the pinned crosspoint-simulator's "
              "src/HalStorage.h -- never against lib/hal/HalStorage.h.");
static_assert(sizeof(HalFile) == 16,
              "HalFile layout changed: re-diff against the pinned crosspoint-simulator's "
              "src/HalStorage.h -- never against lib/hal/HalStorage.h.");

// Downstream code must use Storage instead of SdMan
#ifdef SdMan
#undef SdMan
#endif
