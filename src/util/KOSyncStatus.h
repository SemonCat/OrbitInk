#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include <CalendarTime.h>

namespace KOSyncStatus {

constexpr uint32_t epochMinutes(int year, unsigned month, unsigned day, unsigned hour, unsigned minute) {
  return CalendarTime::epochMinutes(year, month, day, hour, minute);
}

inline bool formatRelative(uint32_t nowMinutes, uint32_t lastSuccessMinutes, char* output, size_t outputSize) {
  if (!output || outputSize == 0 || lastSuccessMinutes == 0 || nowMinutes < lastSuccessMinutes) return false;
  const uint32_t age = nowMinutes - lastSuccessMinutes;
  if (age < 60) {
    std::snprintf(output, outputSize, "%lum", static_cast<unsigned long>(age));
  } else if (age < 1440) {
    std::snprintf(output, outputSize, "%luh", static_cast<unsigned long>(age / 60));
  } else if (age < 7 * 1440) {
    std::snprintf(output, outputSize, "%lud", static_cast<unsigned long>(age / 1440));
  } else {
    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    CalendarTime::civilFromDays(lastSuccessMinutes / 1440, year, month, day);
    (void)year;
    std::snprintf(output, outputSize, "%02u/%02u", month, day);
  }
  return true;
}

}  // namespace KOSyncStatus
