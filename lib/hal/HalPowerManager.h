#pragma once

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <InputManager.h>
#include <Logging.h>
#include <freertos/semphr.h>

#include <cassert>

#include "HalGPIO.h"

class HalPowerManager;
extern HalPowerManager powerManager;  // Singleton

class HalPowerManager {
 public:
  class Lock;

 private:
  int normalFreq = 0;  // MHz
  bool isLowPower = false;

  mutable int _batteryCachedPercent = 0;         // Last read battery percentage (0-100)
  mutable unsigned long _batteryLastPollMs = 0;  // Timestamp of last battery read in milliseconds

  Lock* locks = nullptr;
  TaskHandle_t busyTask = nullptr;
  unsigned busyDepth = 0;
  SemaphoreHandle_t modeMutex = nullptr;
  void updateFrequency(bool idle);

 public:
#if BOARD_HAS_PSRAM
  static constexpr int LOW_POWER_FREQ = 80;  // MHz
#else
  static constexpr int LOW_POWER_FREQ = 10;  // MHz
#endif
  static constexpr unsigned long IDLE_POWER_SAVING_MS = 3000;  // ms
  static constexpr unsigned long BATTERY_POLL_MS = 1500;       // ms

  void begin();

  // Control CPU frequency for power saving
  void setPowerSaving(bool enabled);

  // Plain function-pointer hooks for EInkDisplay::setBusyWaitHooks (installed by
  // HalDisplay::begin()). No capture/context available at that call site, so these are static and
  // reach the singleton directly. Only locks owned by the waiting task are suspended;
  // another task's lock and an active radio still require normal speed.
  static void beginDisplayBusyWait();
  static void endDisplayBusyWait();

  // Setup wake up GPIO and enter deep sleep
  // Should be called inside main loop() to handle the currentLockMode
  void startDeepSleep(HalGPIO& gpio, uint64_t retryDelayMicros = 0) const;

  // The timer delay the previous startDeepSleep() actually armed, kept in RTC slow memory: it
  // survives deep sleep and reads 0 after any other reset (power-on, flash, restart, panic). A
  // timer wake that cannot read its SD stores re-arms this instead of ending the cadence.
  static uint64_t retainedTimerDelayMicros();
  // Deep sleeps entered since the last reset that was not a deep-sleep wake, in the same RTC
  // memory. Back at 0 means RTC power or state was lost (power cut, brownout, panic, restart).
  static uint32_t retainedSleepCycles();

  // Get battery percentage (range 0-100)
  uint16_t getBatteryPercentage() const;

  // RAII helper class to manage power saving locks
  // Usage: create an instance of Lock in a scope to disable power saving, for example when running a task that needs
  // full performance. When the Lock instance is destroyed (goes out of scope), power saving will be re-enabled.
  class Lock {
    friend class HalPowerManager;
    Lock* next = nullptr;
    TaskHandle_t owner = nullptr;

   public:
    explicit Lock();
    ~Lock();

    // Non-copyable and non-movable
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    Lock(Lock&&) = delete;
    Lock& operator=(Lock&&) = delete;
  };
};
