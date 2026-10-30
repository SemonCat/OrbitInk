// Owned simulator adapter: the one HalClock method CrossFade adds on top of the pinned
// crosspoint-simulator HAL. See sim/CrossFadeSimHal/HalClock.h for why this lives here rather than in the
// dependency, and sim/README.md for the boundary rules.

#include <HalClock.h>

#include <cstdlib>
#include <ctime>

// Device semantics (lib/hal/HalClock.h): whole UTC minutes since 1970-01-01, false when the RTC
// is absent or holds no trustworthy time. Implemented purely through the simulator's public
// nowUtc()/hasValidTime()/isAvailable() -- no access to its private state -- so this stays correct
// however the simulator stores its clock offset internally.
//
// nowUtc() is the host wall clock, so simulator timestamps advance in real time rather than being
// frozen. Callers that only need monotonic minute deltas (BookOrbit session clock, KOSync
// debounce) behave the same as on device; anything asserting an exact timestamp would not.
bool HalClock::getEpochMinutes(uint32_t& epochMinutes) const {
  if (!isAvailable() || !hasValidTime()) return false;
  const std::time_t nowSeconds = nowUtc();
  if (nowSeconds <= 0) return false;
  epochMinutes = static_cast<uint32_t>(nowSeconds / 60);
  return true;
}

// Device semantics (lib/hal/HalClock.h): fresh UTC Unix seconds, false when the RTC is absent or
// holds no trustworthy time. The device version reads the RTC directly to skip its own 10s cache;
// the simulator's nowUtc() has no such cache, so there is nothing to bypass here.
bool HalClock::getEpochSeconds(uint64_t& epochSeconds) const {
  // Opt-in RTC-loss repro: mirror a present but unset hardware RTC until the
  // firmware actually calls syncFromNTP(). Never alter persisted event times.
  if (std::getenv("CROSSFADE_SIM_RTC_UNSET") && syncState() != ClockSyncState::Succeeded) return false;
  if (!isAvailable() || !hasValidTime()) return false;
  const std::time_t nowSeconds = nowUtc();
  if (nowSeconds <= 0) return false;
  epochSeconds = static_cast<uint64_t>(nowSeconds);
  return true;
}
