#pragma once
#include <CalendarTime.h>

#include <cstdint>

// -----------------------------------------------------------------------------
// Session wall clock for BookOrbit page-stat timestamps.
//
// The X3's HalClock only exposes whole epoch MINUTES (lib/hal/HalClock.h:
// getEpochMinutes) -- BookOrbit needs epoch SECONDS. Rather than patch the HAL or the
// vendor SDK, or spend an extra NTP round trip (which ee4490f's hardening deliberately
// avoids when the RTC is already valid), this anchors once on an RTC reading and projects
// forward with millis(). Header-only and free of every Arduino/HAL dependency except the
// pure-integer CalendarTime, so the projection arithmetic is host-testable on its own.
//
// Wraparound: millis() wraps every ~49.7 days. Both the anchor and "now" are unsigned
// 32-bit, and the elapsed computation is a plain unsigned subtraction, which is exact
// across a single wrap by modular arithmetic -- no signed comparison anywhere.
// -----------------------------------------------------------------------------
class BookOrbitSessionClock {
 public:
  // Rejects an RTC that lost power and came back at its epoch default. A DS3231 in that state
  // still reports a syntactically valid time (and would satisfy the server's own startTime >= 1
  // check), so "getEpochMinutes returned true" is not sufficient on its own -- without this
  // floor a power-cycled RTC silently poisons the stats with year-2000 sessions.
  static constexpr uint32_t MIN_VALID_EPOCH_MINUTES = CalendarTime::epochMinutes(2024, 1, 1, 0, 0);

  // Anything beyond ~2100 is equally a broken clock, and keeps anchorEpochMinutes * 60 well
  // inside uint32_t (which saturates at epoch second 4,294,967,295 -- February 2106).
  static constexpr uint32_t MAX_VALID_EPOCH_MINUTES = CalendarTime::epochMinutes(2100, 1, 1, 0, 0);

  // Re-anchor when a fresh RTC reading has drifted this far from the projection. Covers a
  // deep-sleep resume or a mid-session NTP correction; well above the 60 s quantisation of a
  // minutes-only clock, so ordinary truncation never triggers a re-anchor.
  //
  // Stale-reading budget, since HalClock caches its epoch-minute reading for 10 s: the worst
  // combined error a reconcile() can observe is that 10 s of staleness plus the 60 s the
  // minutes-only RTC truncates, so ~70 s -- comfortably inside this tolerance, which means
  // ordinary staleness can neither trigger a spurious re-anchor nor mask a genuine clock jump
  // (a deep-sleep resume moves the clock by minutes, not seconds).
  //
  // The residual error this leaves on a stamp is therefore under ~70 s. It is deliberately not
  // hardened further: BookOrbit clusters events into sessions with a 1800 s gap
  // (KOREADER_SESSION_GAP_SECONDS), so an error an order of magnitude smaller than that changes
  // no derived session, and the only ways to shrink it -- polling the RTC harder, or reading
  // sub-minute time the HAL does not expose -- cost either battery or a HAL change this feature
  // is not entitled to make. Anything beyond that is a hardware-behaviour question that only a
  // real device can answer.
  static constexpr uint32_t RESYNC_TOLERANCE_SECONDS = 120;

  /** True when the RTC reading is inside the plausible window above. */
  static bool isPlausible(uint32_t epochMinutes) {
    return epochMinutes >= MIN_VALID_EPOCH_MINUTES && epochMinutes < MAX_VALID_EPOCH_MINUTES;
  }

  /**
   * Anchor on an RTC reading taken at `nowMillis`. Returns false (and leaves the clock
   * unanchored) for an implausible reading -- callers then skip stats capture entirely while
   * normal progress sync carries on unaffected.
   */
  bool anchor(uint32_t epochMinutes, uint32_t nowMillis) {
    if (!isPlausible(epochMinutes)) return false;
    anchorEpochMinutes_ = epochMinutes;
    anchorMillis_ = nowMillis;
    anchored_ = true;
    return true;
  }

  bool isAnchored() const { return anchored_; }

  /**
   * Current UTC epoch seconds. False when never successfully anchored.
   *
   * Granularity is whole minutes at the anchor point (the RTC gives no finer reading), so a
   * stamp can sit up to 59 s behind true wall time. That is immaterial to the server, which
   * clusters events with a 1800 s session gap (koreader-stats.util.ts
   * KOREADER_SESSION_GAP_SECONDS).
   */
  bool now(uint32_t nowMillis, uint32_t& outEpochSeconds) const {
    if (!anchored_) return false;
    const uint32_t elapsedMs = nowMillis - anchorMillis_;  // unsigned: correct across a wrap
    outEpochSeconds = anchorEpochMinutes_ * 60u + elapsedMs / 1000u;
    return true;
  }

  /**
   * Fold in a fresh RTC reading, re-anchoring only when the projection has drifted past
   * RESYNC_TOLERANCE_SECONDS. Re-anchoring affects only stamps taken afterwards; events
   * already captured keep the timestamps they were stored with, which is what the server's
   * (user, book, device, page, startTime) dedup key requires of a retry.
   */
  void reconcile(uint32_t epochMinutes, uint32_t nowMillis) {
    if (!isPlausible(epochMinutes)) return;
    uint32_t projected = 0;
    if (!now(nowMillis, projected)) {
      anchor(epochMinutes, nowMillis);
      return;
    }
    const uint32_t observed = epochMinutes * 60u;
    const uint32_t delta = projected > observed ? projected - observed : observed - projected;
    if (delta > RESYNC_TOLERANCE_SECONDS) anchor(epochMinutes, nowMillis);
  }

 private:
  uint32_t anchorEpochMinutes_ = 0;
  uint32_t anchorMillis_ = 0;
  bool anchored_ = false;
};
