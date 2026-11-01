#pragma once

// Owned simulator adapter: superset shadow of crosspoint-simulator's HalPowerManager.
//
// WHY THIS FILE EXISTS
// The simulator replaces the firmware HAL wholesale (`lib_ignore = hal`), so on the host
// `#include <HalPowerManager.h>` resolves here instead of lib/hal/HalPowerManager.h. CrossFade's
// HalPowerManager carries a `startDeepSleep(HalGPIO&, uint64_t)` overload the pinned simulator does
// not; src/main.cpp calls it at four sites (progress-sync retry sleep and the post-wakeup power
// paths) with an explicit microsecond delay. A method cannot be added to a class from outside it,
// and the simulator checkout under .pio/libdeps must not be patched -- so this header declares the
// class as the pinned simulator declares it, plus that overload, which
// sim/CrossFadeSimHal/HalPowerManagerCompat.cpp defines using only the simulator's own public API
// (specifically, by delegating to the pinned single-argument startDeepSleep()).
//
// MAINTENANCE CONTRACT
// Everything above the CROSSFADE ADDITIONS marker mirrors crosspoint-simulator@dacbbbc
// src/HalPowerManager.h declaration for declaration -- same members, same order, same inline
// bodies -- but reformatted to this repo's clang-format style, so compare declarations rather than
// bytes. The simulator's own HalPowerManager.cpp includes its copy via a quoted include and so
// keeps compiling against the original; this shadow is what the firmware sees. The two definitions
// must agree on data layout and on every inline body, because the object itself (`powerManager`) is
// constructed in the simulator's translation unit. The exact dependency pin in platformio.ini is
// what keeps that true. When bumping the pin, re-diff this file against the new
// src/HalPowerManager.h before anything else.
//
// See sim/README.md.

#include <Arduino.h>
#include <InputManager.h>
#include <Logging.h>
#include <freertos/semphr.h>

#include <cassert>

#include "HalGPIO.h"

class HalPowerManager;
extern HalPowerManager powerManager;  // Singleton

class HalPowerManager {
  int normalFreq = 0;  // MHz
  bool isLowPower = false;

  enum LockMode { None, NormalSpeed };
  LockMode currentLockMode = None;
  SemaphoreHandle_t modeMutex = nullptr;  // Protect access to currentLockMode

 public:
  static constexpr int LOW_POWER_FREQ = 10;  // MHz
  static constexpr unsigned long IDLE_POWER_SAVING_MS = 3000;
  static constexpr unsigned long IDLE_DOWNCLOCK_MS = 500;
  static constexpr unsigned long IDLE_LIGHT_SLEEP_MS = 1000;
  static constexpr unsigned long BATTERY_POLL_MS = 1500;
  static constexpr unsigned long LIGHT_SLEEP_SLICE_MS = 50;
  static constexpr unsigned long BUSY_SLEEP_SLICE_MS = 20;

  void begin();

  // Control CPU frequency for power saving
  void setPowerSaving(bool enabled);

  bool lightSleep(const HalGPIO &) const {
    delay(50);
    return true;
  }
  bool onEinkBusyWaitSlice(int8_t, uint8_t) { return false; }
  void noteMainLoopIteration() {}
  void noteRenderWaitBegin() {}
  void noteRenderWaitEnd() {}

  // Setup wake up GPIO and enter deep sleep
  // Should be called inside main loop() to handle the currentLockMode
  void startDeepSleep(HalGPIO &gpio) const;

  // Get battery percentage (range 0-100)
  uint16_t getBatteryPercentage() const;

  // RAII helper class to manage power saving locks
  // Usage: create an instance of Lock in a scope to disable power saving, for
  // example when running a task that needs full performance. When the Lock
  // instance is destroyed (goes out of scope), power saving will be re-enabled.
  class Lock {
    friend class HalPowerManager;
    bool valid = false;

   public:
    explicit Lock();
    ~Lock();

    // Non-copyable and non-movable
    Lock(const Lock &) = delete;
    Lock &operator=(const Lock &) = delete;
    Lock(Lock &&) = delete;
    Lock &operator=(Lock &&) = delete;
  };

  // ---- CROSSFADE ADDITIONS (not present upstream; defined in sim/HalPowerManagerCompat.cpp) ----

  // Mirrors lib/hal/HalPowerManager.h: enter deep sleep with an RTC timer wake scheduled
  // `sleepDelayMicros` from now, in addition to the button wakeups `startDeepSleep(HalGPIO&)`
  // already handles. Declared here only -- never defined inline -- so it cannot depend on this
  // shadow's view of the object's private data.
  //
  // By default the delay is unused and this blocks for a button wake, exactly as before. With
  // CROSSFADE_SIM_TIMER_DIVISOR set, a non-zero delay instead sleeps delay/divisor and relaunches
  // the process as a timer wake (fresh memory, same SD card) -- see HalPowerManagerCompat.cpp and
  // sim/README.md for the knobs and what this does and does not model.
  void startDeepSleep(HalGPIO &gpio, uint64_t sleepDelayMicros) const;

  // Mirrors lib/hal/HalPowerManager.h. The RTC slow memory is modeled by the environment the
  // relaunch inherits; a fresh launch is a power-on and reads 0. Static: no layout change.
  static uint64_t retainedTimerDelayMicros();
  static uint32_t retainedSleepCycles();
};

// Layout guard. This pins the size of *this* declaration, which is the one thing a future edit is
// most likely to get wrong: CrossFade's real lib/hal/HalPowerManager.h has a completely different
// private section, so "completing" this shadow from the production header instead of from the
// pinned simulator's would compile, link, and then read past the end of the `powerManager` object
// that the simulator's translation unit actually allocated. This assert turns that into a build
// failure.
//
// It cannot see the simulator's own header, so it does NOT detect upstream drift -- bumping the
// pin still requires the manual re-diff described above.
static_assert(sizeof(HalPowerManager) == 24,
              "HalPowerManager layout changed: re-diff against the pinned crosspoint-simulator's "
              "src/HalPowerManager.h -- never against lib/hal/HalPowerManager.h.");
