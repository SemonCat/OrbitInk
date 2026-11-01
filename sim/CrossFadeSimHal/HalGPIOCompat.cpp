// Owned simulator adapter: the timer-aware wake reason. See sim/CrossFadeSimHal/HalGPIO.h.

#include <HalGPIO.h>

#include <cstdlib>
#include <cstring>

// This file needs the simulator's own definition, not the firmware-facing alias.
#undef getWakeupReason

HalGPIO::WakeupReason HalGPIO::crossfadeWakeupReason() const {
  // Set by HalPowerManagerCompat.cpp right before an accelerated timer relaunch. Read once per
  // process, like the device's stable reset cause.
  static const bool timerWake = [] {
    const char* value = std::getenv("CROSSFADE_SIM_TIMER_WAKE");
    const bool set = value && std::strcmp(value, "1") == 0;
    unsetenv("CROSSFADE_SIM_TIMER_WAKE");
    return set;
  }();
  if (timerWake) {
    // The relaunch went through rebootAsPowerWake(); consume its power-button marker so it can
    // never surface as a later PowerButton reason in this process.
    (void)getWakeupReason();
    return WakeupReason::Timer;
  }
  return getWakeupReason();
}
