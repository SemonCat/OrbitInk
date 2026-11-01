#pragma once

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "ProgressSyncRetry.h"

// Durable, privacy-safe record of every unattended sync event, so an overnight failure on a device
// can be localized afterwards from the SD card: did the timer wake happen at all, did WiFi come up,
// did the TLS heap gate refuse, what was each provider's outcome, and what was armed next.
//
// One text line per event in /.crosspoint/sync-receipts.log (rotated once to .1.log at 8 KiB).
// Written when the device is about to deep sleep (sleep entry, a timer wake, the two boot paths that
// go straight back to sleep) and once per interactive boot -- never while reading. A line holds no
// book path, title, document hash, SSID, server URL, user name, device id or credential.
//
// `cycle` and `prev` come from RTC memory (HalPowerManager), which only a deep-sleep wake keeps:
// a boot line with cycle=0 after a sleep line that armed a timer means the device lost RTC power or
// was reset rather than waking from that sleep; a gap in cycle means events that left no line.
namespace ProgressSyncReceipt {

enum class Trigger : uint8_t { Sleep, Timer, UsbBoot, RejectedWake, Boot };
enum class Wifi : uint8_t { NotTried, Connected, Failed };

struct Record {
  Trigger trigger = Trigger::Sleep;
  int8_t kosync = -1;  // -1: no transaction this event; otherwise ProgressSyncRetry::Outcome
  int8_t bookOrbit = -1;
  Wifi wifi = Wifi::NotTried;
  uint32_t wifiMs = 0;
  bool storeFailed = false;  // SD or state.json could not be read on a timer wake
  uint16_t gateChecks = 0;   // KOSyncRequest TLS heap gate (35,000 free / 20,000 block)
  uint16_t gateRejections = 0;
  uint32_t gateMinFree = 0;     // lowest values the gate saw; 0 when it never ran
  uint32_t gateMinLargest = 0;
  uint32_t freeHeap = 0;        // at commit
  uint32_t largestBlock = 0;
  uint32_t minFreeHeap = 0;     // low-water mark since this boot
  uint64_t nextDelayMicros = 0;  // timer delay requested for the coming sleep (0: none)
  int8_t usb = -1;               // -1 unknown, 0/1 as HalGPIO::isUsbConnected() infers it
  uint32_t cycle = 0;            // deep sleeps since the RTC memory was last reset
  uint64_t prevArmedMicros = 0;  // timer the previous sleep actually armed (0: none, or RTC lost)
  bool hasTime = false;
  uint32_t epochMinutes = 0;
};

inline const char* triggerName(Trigger trigger) {
  switch (trigger) {
    case Trigger::Sleep:
      return "sleep";
    case Trigger::Timer:
      return "timer";
    case Trigger::UsbBoot:
      return "usb-boot";
    case Trigger::RejectedWake:
      return "rejected-wake";
    case Trigger::Boot:
      return "boot";
  }
  return "?";
}

inline const char* outcomeName(int8_t outcome) {
  switch (outcome) {
    case static_cast<int8_t>(ProgressSyncRetry::Outcome::Confirmed):
      return "ok";
    case static_cast<int8_t>(ProgressSyncRetry::Outcome::Retry):
      return "retry";
    case static_cast<int8_t>(ProgressSyncRetry::Outcome::NoRetry):
      return "stop";
    default:
      return "-";
  }
}

// Fixed key order, space separated, newline terminated. Returns the line length, or 0 if it did
// not fit.
inline size_t format(const Record& r, char* out, size_t size) {
  char time[16] = "-";
  if (r.hasTime) snprintf(time, sizeof(time), "%" PRIu32 "m", r.epochMinutes);
  const char* wifi = r.wifi == Wifi::Connected ? "ok" : r.wifi == Wifi::Failed ? "fail" : "-";
  const int n = snprintf(out, size,
                         "at=%s t=%s store=%s ko=%s bo=%s wifi=%s/%" PRIu32 "ms gate=%u/%u gmin=%" PRIu32
                         "/%" PRIu32 " heap=%" PRIu32 "/%" PRIu32 "/%" PRIu32 " next=%" PRIu64 "s usb=%s cycle=%" PRIu32
                         " prev=%" PRIu64 "s\n",
                         time, triggerName(r.trigger), r.storeFailed ? "fail" : "ok", outcomeName(r.kosync),
                         outcomeName(r.bookOrbit), wifi, r.wifiMs, static_cast<unsigned>(r.gateChecks),
                         static_cast<unsigned>(r.gateRejections), r.gateMinFree, r.gateMinLargest, r.freeHeap,
                         r.largestBlock, r.minFreeHeap, r.nextDelayMicros / 1000000ULL,
                         r.usb < 0 ? "-" : (r.usb ? "1" : "0"), r.cycle, r.prevArmedMicros / 1000000ULL);
  return n > 0 && static_cast<size_t>(n) < size ? static_cast<size_t>(n) : 0;
}

// Event lifecycle. begin() resets the in-memory record and the heap-gate counters; the note*()
// calls only update memory (they also run during reading-time syncs, which never commit); commit()
// samples heap/USB/clock and appends one line. Only commit() touches the SD card.
void begin(Trigger trigger);
void noteOutcome(ProgressSyncRetry::Provider provider, ProgressSyncRetry::Outcome outcome);
void noteWifi(bool connected, uint32_t elapsedMs);
void noteStoreFailure();
void commit(uint64_t nextDelayMicros);

}  // namespace ProgressSyncReceipt
