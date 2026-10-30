#pragma once

#include <cstdint>

namespace ProgressSyncSchedule {

enum Interval : uint8_t {
  Off = 0,
  Minutes30 = 1,
  Hour1 = 2,
  Hours3 = 3,
  Hours6 = 4,
  Hours12 = 5,
  Hours24 = 6,
  IntervalCount
};

constexpr uint8_t DEFAULT_INTERVAL = Hour1;
constexpr uint64_t MICROS_PER_MINUTE = 60ULL * 1000ULL * 1000ULL;

// `present=false` is the upgrade path for settings written before this field existed. Invalid
// values, including values larger than uint8_t, deliberately recover to the communicated 1-hour
// default rather than clamping to a different valid choice.
constexpr uint8_t normalizeStored(uint64_t raw, bool present = true) {
  return present && raw < IntervalCount ? static_cast<uint8_t>(raw) : DEFAULT_INTERVAL;
}

constexpr uint64_t intervalMicros(uint8_t interval) {
  switch (interval) {
    case Off:
      return 0;
    case Minutes30:
      return 30ULL * MICROS_PER_MINUTE;
    case Hour1:
      return 60ULL * MICROS_PER_MINUTE;
    case Hours3:
      return 3ULL * 60ULL * MICROS_PER_MINUTE;
    case Hours6:
      return 6ULL * 60ULL * MICROS_PER_MINUTE;
    case Hours12:
      return 12ULL * 60ULL * MICROS_PER_MINUTE;
    case Hours24:
      return 24ULL * 60ULL * MICROS_PER_MINUTE;
    default:
      return 60ULL * MICROS_PER_MINUTE;
  }
}

constexpr uint8_t enabledProviders(bool hasSavedWifi, bool hasTargetEpub, bool hasKOSyncCredentials,
                                   bool kosyncAutoSyncEnabled, bool hasBookOrbitCredentials) {
  if (!hasSavedWifi || !hasTargetEpub) return 0;
  return static_cast<uint8_t>((hasKOSyncCredentials && kosyncAutoSyncEnabled ? 1 : 0) |
                              (hasBookOrbitCredentials ? 2 : 0));
}

constexpr uint64_t sleepDelayMicros(uint8_t interval, uint8_t providers) {
  return providers == 0 ? 0 : intervalMicros(normalizeStored(interval));
}

static_assert(intervalMicros(Hours24) == 86400000000ULL, "24-hour interval must use uint64_t microseconds");

}  // namespace ProgressSyncSchedule
