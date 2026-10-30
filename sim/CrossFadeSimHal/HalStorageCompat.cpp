// Owned simulator adapter: the storage HAL members CrossFade adds on top of the pinned
// crosspoint-simulator HAL. See sim/CrossFadeSimHal/HalStorage.h for why these live here rather
// than in the dependency, and sim/README.md for the boundary rules.

#include <HalStorage.h>

// Device contract (lib/hal/HalStorage.h): walk the path from the root and report Absent only
// after a complete, error-free enumeration, so a read failure can never be mistaken for "this
// file was never there" by the destructive migration/recovery paths that call this.
//
// The empty/null guard mirrors the device exactly (lib/hal/HalStorage.cpp:97 returns Error, not
// Absent). It matters which one: Absent is the answer that unblocks a caller -- see
// src/util/BookOrbitMigration.cpp:51, where Absent means "nothing to migrate, report success" --
// so answering Absent to a question this layer cannot actually answer would be the one direction
// that fails unsafe. No current caller passes an empty path; this keeps a future one honest.
//
// For real paths the simulator backs storage with the host filesystem through POSIX calls, where a
// stat of an existing directory does not partially fail the way an SD read can. exists() is
// therefore already the authoritative answer, and Present/Absent -- the distinction the callers
// actually branch on -- is exact.
//
// Consequence to keep in mind when reading simulator results: the I/O-failure Error branch of every
// caller (BookmarkFile's "leave both copies alone", BookOrbitMigration's "don't delete") is never
// exercised on the host. Those branches remain device-only paths and must be reasoned about, or
// covered by the host gtest suites under test/, not by the simulator.
HalStorage::PathStatus HalStorage::probePath(const char* path) {
  if (path == nullptr || path[0] == '\0') return PathStatus::Error;
  return exists(path) ? PathStatus::Present : PathStatus::Absent;
}

// Device contract: packed FAT date/time from the directory entry cached by open()/openNextFile().
// The simulator's HalFile is a POSIX file descriptor with no FAT directory entry behind it and no
// public accessor for host mtime, so this reports "no timestamp available".
//
// That is a different return value from the device but the same outcome for the only caller. For a
// file this firmware wrote itself the device returns *true* with a zeroed date/time (SdFat's
// FatFile::getModifyDateTime() succeeds whenever the directory entry is readable, and this firmware
// registers no SdFat date/time callback -- see lib/hal/HalStorage.h), whereas this returns false.
// src/util/LibraryScanner.cpp:80-84 defaults fatDateTime to 0 and only overwrites it on true, so
// device-true-with-zero and simulator-false both leave it 0: the library index still works, it just
// falls back to size-only change detection.
bool HalFile::getModifyTime(uint16_t& fatDate, uint16_t& fatTime) const {
  fatDate = 0;
  fatTime = 0;
  return false;
}

// Device contract: SdFat records directory-enumeration read failures on the directory handle, so a
// false openNextFile() with getError() == 0 is clean end-of-directory and nonzero is an I/O
// failure. The simulator's readdir-backed enumeration surfaces failure through its return value
// and keeps no sticky error register, so end-of-directory is the only outcome to report. Zero
// means exactly that: no error pending.
uint8_t HalFile::getError() const { return 0; }
