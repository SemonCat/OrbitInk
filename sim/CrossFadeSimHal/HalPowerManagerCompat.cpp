// Owned simulator adapter: the one HalPowerManager method CrossFade adds on top of the pinned
// crosspoint-simulator HAL. See sim/CrossFadeSimHal/HalPowerManager.h for why this lives here
// rather than in the dependency, and sim/README.md for the boundary rules.

#include <HalPowerManager.h>

// Device semantics (lib/hal/HalPowerManager.h): deep sleep with an RTC timer wake scheduled
// `sleepDelayMicros` from now, on top of the button wakeups the pinned single-argument overload
// already covers. The simulator has no RTC/timer wake path to program, so `sleepDelayMicros` is
// unused here -- see the fidelity-gap note in the header for what that means for callers.
void HalPowerManager::startDeepSleep(HalGPIO &gpio, uint64_t /*sleepDelayMicros*/) const {
  startDeepSleep(gpio);
}
