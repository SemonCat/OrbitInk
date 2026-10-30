#pragma once

// Owned simulator adapter: superset shadow of crosspoint-simulator's HalClock.
//
// WHY THIS FILE EXISTS
// The simulator replaces the firmware HAL wholesale (`lib_ignore = hal`), so on the host
// `#include <HalClock.h>` resolves here instead of lib/hal/HalClock.h. CrossFade's HalClock
// carries getEpochMinutes() and getEpochSeconds(), which the pinned simulator does not; call
// sites needing them include src/util/BookOrbitBookmarkSync.cpp, src/util/BookOrbitStatsSync.cpp,
// src/util/KOReaderAutoSync.cpp, src/activities/reader/EpubReaderActivity.cpp. A method cannot
// be added to a class from outside it, and the simulator checkout under .pio/libdeps must not be
// patched -- so this header declares the class as the pinned simulator declares it, plus those
// methods, which sim/HalClockCompat.cpp defines using only the simulator's own public API.
//
// MAINTENANCE CONTRACT
// Everything above the CROSSFADE ADDITIONS marker mirrors crosspoint-simulator@dacbbbc
// src/HalClock.h declaration for declaration -- same members, same order, same inline bodies --
// but reformatted to this repo's clang-format style, so compare declarations rather than bytes.
// The simulator's own HalClock.cpp includes its copy via a quoted include and so keeps compiling
// against the original; this shadow is what the firmware sees. The two definitions must agree on
// data layout and on every inline body, because the object itself is constructed in the
// simulator's translation unit. The exact dependency pin in platformio.ini is what keeps that
// true: nothing upstream can move under us. When bumping the pin, re-diff this file against the
// new src/HalClock.h before anything else.
//
// See sim/README.md.

#include <Arduino.h>

#include <cstddef>
#include <cstdint>
#include <ctime>

enum class ClockSyncState : uint8_t {
  Idle,
  Syncing,
  Succeeded,
  Failed,
};

class HalClock;
extern HalClock halClock;

class HalClock {
  bool _available = false;
  bool _autoSyncEnabled = true;
  bool _autoSyncAttempted = false;
  std::time_t _utcOffsetSeconds = 0;
  ClockSyncState _syncState = ClockSyncState::Idle;

 public:
  void begin();
  void update();

  std::time_t nowUtc() const;
  bool hasValidTime() const;
  bool setUtcTime(std::time_t epoch);
  void setAutoSyncEnabled(bool enabled);
  bool requestSync();
  bool syncNow(uint32_t timeoutMs = 10000);
  ClockSyncState syncState() const { return _syncState; }

  bool isAvailable() const { return _available; }
  bool getTime(uint8_t& hour, uint8_t& minute) const;
  bool getDateTime(uint16_t& year, uint8_t& month, uint8_t& day, uint8_t& hour, uint8_t& minute) const;
  bool formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased = 48,
                  bool use12Hour = false) const;
  bool formatDate(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased = 48) const;
  bool syncFromNTP();

  // ---- CROSSFADE ADDITIONS (not present upstream; defined in sim/HalClockCompat.cpp) ----

  // Mirrors lib/hal/HalClock.h: current UTC time as whole minutes since 1970-01-01, false when
  // no usable clock is available. Declared here only -- never defined inline -- so it cannot
  // depend on this shadow's view of the object's private data.
  bool getEpochMinutes(uint32_t& epochMinutes) const;

  // Mirrors lib/hal/HalClock.h: fresh UTC Unix seconds, false when no usable clock is available.
  // The real device intentionally bypasses its 10s display cache for this call (a save/retry must
  // never manufacture a newer event time); the simulator has no such cache to bypass -- nowUtc()
  // already reads the host wall clock on every call -- so this is a direct passthrough.
  bool getEpochSeconds(uint64_t& epochSeconds) const;
};

// Layout guard. This pins the size of *this* declaration, which is the one thing a future edit is
// most likely to get wrong: CrossFade's real lib/hal/HalClock.h has a completely different private
// section (it embeds a FreeInk SDK `Rtc` by value plus five cache fields), so "completing" this
// shadow from the production header instead of from the pinned simulator's would compile, link,
// and then read past the end of the `halClock` object that the simulator's translation unit
// actually allocated. This assert turns that into a build failure.
//
// It cannot see the simulator's own header, so it does NOT detect upstream drift -- bumping the
// pin still requires the manual re-diff described above.
static_assert(sizeof(HalClock) == 24,
              "HalClock layout changed: re-diff against the pinned crosspoint-simulator's "
              "src/HalClock.h -- never against lib/hal/HalClock.h.");
