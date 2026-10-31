#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

// Why the last custom-channel OTA attempt failed, in terms a user can read back to us.
// Pure data + formatting (host-tested in test/firmware_update). The device fills it in
// OtaUpdater and shows summary() on the failure screen and report() in /ota_report.txt.
// It only describes a failure; it never changes what the updater accepts.
namespace ota_diagnostics {

enum class Phase : uint8_t {
  NONE,
  MANIFEST_FETCH,   // GET of the channel manifest
  MANIFEST_VERIFY,  // parse / ECDSA signature
  MANIFEST_POLICY,  // channel / target / build / size / URL policy
  STAGE_BEGIN,      // opening the SD staging file
  DOWNLOAD,         // GET of firmware.bin into the staging file
  STAGE_VERIFY,     // size / SHA256 / ESP image check of the staged file
  FLASH,            // writing the inactive partition
};

inline const char* phaseName(const Phase p) {
  switch (p) {
    case Phase::NONE: return "NONE";
    case Phase::MANIFEST_FETCH: return "MANIFEST_FETCH";
    case Phase::MANIFEST_VERIFY: return "MANIFEST_VERIFY";
    case Phase::MANIFEST_POLICY: return "MANIFEST_POLICY";
    case Phase::STAGE_BEGIN: return "STAGE_BEGIN";
    case Phase::DOWNLOAD: return "DOWNLOAD";
    case Phase::STAGE_VERIFY: return "STAGE_VERIFY";
    case Phase::FLASH: return "FLASH";
  }
  return "?";
}

struct Record {
  Phase phase = Phase::NONE;
  const char* error = "";   // static name: ota_fetch / ota_manifest / firmware_upload / flash result
  const char* detail = "";  // optional second static name (session error, image status)
  int httpStatus = 0;
  int hop = -1;             // redirect hop that failed, -1 if not a network failure
  char host[48] = {};       // host of the failing hop
  uint32_t bytesDone = 0;   // firmware bytes received / written before the failure
  uint32_t bytesTotal = 0;
  uint32_t freeHeap = 0;    // at the failure
  uint32_t maxAlloc = 0;    // largest free block at the failure (TLS needs a large contiguous one)

  bool failed() const { return phase != Phase::NONE; }

  void setHost(const char* h) {
    snprintf(host, sizeof(host), "%s", h ? h : "");
  }
};

// One short ASCII line for the e-paper failure screen, e.g.
// "DOWNLOAD CONNECT_FAILED hop1 release-assets.githubusercontent.com heap 41234/18432".
inline std::string summary(const Record& r) {
  if (!r.failed()) return "";
  char buf[160];
  int n = snprintf(buf, sizeof(buf), "%s %s", phaseName(r.phase), r.error);
  if (n > 0 && static_cast<size_t>(n) < sizeof(buf) && r.detail[0] != '\0') {
    n += snprintf(buf + n, sizeof(buf) - n, "/%s", r.detail);
  }
  if (n > 0 && static_cast<size_t>(n) < sizeof(buf) && r.httpStatus > 0) {
    n += snprintf(buf + n, sizeof(buf) - n, " HTTP %d", r.httpStatus);
  }
  if (n > 0 && static_cast<size_t>(n) < sizeof(buf) && r.hop >= 0) {
    n += snprintf(buf + n, sizeof(buf) - n, " hop%d %s", r.hop, r.host);
  }
  if (n > 0 && static_cast<size_t>(n) < sizeof(buf) && r.freeHeap > 0) {
    snprintf(buf + n, sizeof(buf) - n, " heap %u/%u", static_cast<unsigned>(r.freeHeap),
             static_cast<unsigned>(r.maxAlloc));
  }
  return buf;
}

// Multi-line text for /ota_report.txt; `identity` is the running image's OTA identity literal.
inline std::string report(const Record& r, const char* identity, const char* version) {
  char buf[512];
  snprintf(buf, sizeof(buf),
           "OrbitInk OTA failure\nRunning: %s (%s)\nSummary: %s\nPhase: %s\nError: %s\nDetail: %s\n"
           "HTTP status: %d\nHop: %d\nHost: %s\nBytes: %u/%u\nFree heap: %u\nLargest free block: %u\n",
           identity ? identity : "", version ? version : "", summary(r).c_str(), phaseName(r.phase), r.error,
           r.detail, r.httpStatus, r.hop, r.host, static_cast<unsigned>(r.bytesDone),
           static_cast<unsigned>(r.bytesTotal), static_cast<unsigned>(r.freeHeap), static_cast<unsigned>(r.maxAlloc));
  return buf;
}

}  // namespace ota_diagnostics
