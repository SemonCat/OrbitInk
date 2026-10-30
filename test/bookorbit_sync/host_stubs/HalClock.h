#pragma once
// Host stand-in for lib/hal/HalClock.h, reduced to the one call BookOrbitStatsSync.cpp makes.
#include <cstdint>

class HalClock {
 public:
  bool available = true;
  uint32_t epochMinutes = 0;
  bool getEpochMinutes(uint32_t& out) const {
    if (!available) return false;
    out = epochMinutes;
    return true;
  }
};

extern HalClock halClock;
