#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>

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

// Formatting writes into caller-owned buffers: this runs right after OOM failures, and on the
// -fno-exceptions device a failed std::string allocation would abort instead of reporting.
constexpr size_t SUMMARY_MAX = 160;
constexpr size_t REPORT_MAX = 512;

// One short ASCII line for the e-paper failure screen, e.g.
// "DOWNLOAD CONNECT_FAILED hop1 release-assets.githubusercontent.com heap 41234/18432".
// Empty when nothing failed. Always NUL-terminated within `cap`.
inline void summary(const Record& r, char* out, const size_t cap) {
  if (!out || cap == 0) return;
  out[0] = '\0';
  if (!r.failed()) return;
  size_t n = 0;
  const auto append = [&](const char* fmt, auto... args) {
    if (n >= cap) return;
    const int w = snprintf(out + n, cap - n, fmt, args...);
    if (w > 0) n += static_cast<size_t>(w);
  };
  append("%s %s", phaseName(r.phase), r.error);
  if (r.detail[0] != '\0') append("/%s", r.detail);
  if (r.httpStatus > 0) append(" HTTP %d", r.httpStatus);
  if (r.hop >= 0) append(" hop%d %s", r.hop, r.host);
  if (r.freeHeap > 0) append(" heap %u/%u", static_cast<unsigned>(r.freeHeap), static_cast<unsigned>(r.maxAlloc));
}

// Greedy word wrap of a summary into at most `maxLines` NUL-terminated lines of at most
// `lineChars` characters (a longer single word is cut). Stack-only; returns the line count.
constexpr size_t WRAP_LINE_CHARS = 40;
constexpr size_t WRAP_MAX_LINES = 4;
inline size_t wrap(const char* text, char (*lines)[WRAP_LINE_CHARS + 1], const size_t maxLines) {
  size_t count = 0;
  size_t len = 0;
  for (const char* p = text; p && *p && count < maxLines;) {
    while (*p == ' ') p++;
    if (!*p) break;
    size_t word = 0;
    while (p[word] && p[word] != ' ') word++;
    if (len > 0 && len + 1 + word > WRAP_LINE_CHARS) {
      lines[count++][len] = '\0';
      len = 0;
      if (count == maxLines) break;
    }
    if (len > 0) lines[count][len++] = ' ';
    const size_t take = word < WRAP_LINE_CHARS - len ? word : WRAP_LINE_CHARS - len;
    for (size_t i = 0; i < take; i++) lines[count][len++] = p[i];
    p += word;
  }
  if (len > 0 && count < maxLines) lines[count++][len] = '\0';
  return count;
}

// Multi-line text for /ota_report.txt; `identity` is the running image's OTA identity literal.
// Returns the length written (always < cap).
inline size_t report(const Record& r, const char* identity, const char* version, char* out, const size_t cap) {
  if (!out || cap == 0) return 0;
  char line[SUMMARY_MAX];
  summary(r, line, sizeof(line));
  const int w = snprintf(out, cap,
                         "OrbitInk OTA failure\nRunning: %s (%s)\nSummary: %s\nPhase: %s\nError: %s\nDetail: %s\n"
                         "HTTP status: %d\nHop: %d\nHost: %s\nBytes: %u/%u\nFree heap: %u\nLargest free block: %u\n",
                         identity ? identity : "", version ? version : "", line, phaseName(r.phase), r.error, r.detail,
                         r.httpStatus, r.hop, r.host, static_cast<unsigned>(r.bytesDone),
                         static_cast<unsigned>(r.bytesTotal), static_cast<unsigned>(r.freeHeap),
                         static_cast<unsigned>(r.maxAlloc));
  if (w < 0) return 0;
  return static_cast<size_t>(w) < cap ? static_cast<size_t>(w) : cap - 1;
}

}  // namespace ota_diagnostics
