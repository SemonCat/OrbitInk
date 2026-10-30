#pragma once

#include <HalStorage.h>
#include <Logging.h>

#include <cstddef>
#include <string>

// Crash-safe whole-file replacement for small app-owned metadata files (the BookOrbit library index
// and catalog cache), with exactly the state machine BookOrbitStatsQueue already uses for its queue
// files (lib/KOReaderSync/BookOrbitStatsQueue.cpp promoteReplacement / recoverInterruptedReplacement):
//
//   write <path>.tmp fully, flush, close, reopen and verify its size, then
//   promote:  refuse if an unresolved <path>.bak exists; live -> .bak; .tmp -> live; drop .bak.
//             A failed tmp -> live rename restores .bak -> live.
//   recover (run before every read or replace):
//     live present        -> the last promotion committed: drop stale .bak and .tmp
//     .bak, no live       -> crashed after staging aside: restore .bak, drop .tmp
//     only .tmp           -> never verified complete: discard it, never promote it
//
// At every instant at least one of live/.bak is a complete, verified file, and a partially written
// .tmp can never become live. The stats queue keeps its own copy of this logic unchanged.
namespace AtomicFileReplace {

inline std::string tmpPath(const std::string& path) { return path + ".tmp"; }
inline std::string bakPath(const std::string& path) { return path + ".bak"; }

// Returns false only when the state is unresolved (a .bak that is the only copy could not be put
// back); callers must then neither read nor replace, so that backup is never overwritten.
inline bool recover(const char* module, const std::string& path) {
  const std::string tmp = tmpPath(path);
  const std::string bak = bakPath(path);
  const bool haveLive = Storage.exists(path.c_str());
  const bool haveBak = Storage.exists(bak.c_str());
  const bool haveTmp = Storage.exists(tmp.c_str());
  if (!haveBak && !haveTmp) return true;
  if (haveLive) {
    Storage.remove(bak.c_str());
    Storage.remove(tmp.c_str());
    return true;
  }
  if (haveBak) {
    if (!Storage.rename(bak.c_str(), path.c_str())) {
      LOG_ERR(module, "Could not restore backup; leaving it for a later attempt");
      return false;
    }
    Storage.remove(tmp.c_str());
    return true;
  }
  LOG_INF(module, "Discarding an unverified temp file");
  Storage.remove(tmp.c_str());
  return true;
}

// Promotes an already written and verified <path>.tmp. See the state machine above.
inline bool promote(const char* module, const std::string& path) {
  const std::string tmp = tmpPath(path);
  const std::string bak = bakPath(path);
  if (!Storage.exists(tmp.c_str())) return false;
  if (Storage.exists(bak.c_str())) {
    LOG_ERR(module, "Unresolved backup present; refusing to replace over it");
    Storage.remove(tmp.c_str());
    return false;
  }
  if (Storage.exists(path.c_str()) && !Storage.rename(path.c_str(), bak.c_str())) {
    LOG_ERR(module, "Could not stage the live file aside; keeping it");
    Storage.remove(tmp.c_str());
    return false;
  }
  if (!Storage.rename(tmp.c_str(), path.c_str())) {
    LOG_ERR(module, "Could not promote replacement; restoring backup");
    if (Storage.exists(bak.c_str()) && !Storage.rename(bak.c_str(), path.c_str())) {
      LOG_ERR(module, "Could not restore backup; it stays for recovery on next access");
    }
    Storage.remove(tmp.c_str());
    return false;
  }
  Storage.remove(bak.c_str());
  return true;
}

// Verifies that <path>.tmp holds exactly `expectedBytes` after it has been closed. A short write,
// a failed flush/close or a truncated file all fail here, and the temp file is discarded.
inline bool verifyTmp(const char* module, const std::string& path, const size_t expectedBytes) {
  const std::string tmp = tmpPath(path);
  size_t actual = 0;
  {
    HalFile f;
    if (!Storage.openFileForRead(module, tmp, f)) {
      Storage.remove(tmp.c_str());
      return false;
    }
    actual = f.fileSize();
  }
  if (actual != expectedBytes) {
    LOG_ERR(module, "Temp file size %u != expected %u; discarding", static_cast<unsigned>(actual),
            static_cast<unsigned>(expectedBytes));
    Storage.remove(tmp.c_str());
    return false;
  }
  return true;
}

}  // namespace AtomicFileReplace
