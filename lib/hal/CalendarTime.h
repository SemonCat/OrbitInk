#pragma once

#include <cstdint>

namespace CalendarTime {

// Howard Hinnant's integer civil-calendar conversion. The X3 DS3231 is kept
// in UTC, so this avoids process timezone/DST dependencies entirely.
constexpr int64_t daysFromCivil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
  const unsigned dayOfYear = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
  const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  return era * 146097 + static_cast<int>(dayOfEra) - 719468;
}

constexpr uint32_t epochMinutes(int year, unsigned month, unsigned day, unsigned hour, unsigned minute) {
  const int64_t days = daysFromCivil(year, month, day);
  if (days < 0) return 0;
  return static_cast<uint32_t>(days * 1440 + hour * 60 + minute);
}

constexpr uint64_t epochSeconds(int year, unsigned month, unsigned day, unsigned hour, unsigned minute,
                                unsigned second) {
  const int64_t days = daysFromCivil(year, month, day);
  if (days < 0) return 0;
  return static_cast<uint64_t>(days) * 86400ULL + static_cast<uint64_t>(hour) * 3600ULL +
         static_cast<uint64_t>(minute) * 60ULL + second;
}

inline void civilFromDays(int64_t daysSinceEpoch, int& year, unsigned& month, unsigned& day) {
  daysSinceEpoch += 719468;
  const int era = (daysSinceEpoch >= 0 ? daysSinceEpoch : daysSinceEpoch - 146096) / 146097;
  const unsigned dayOfEra = static_cast<unsigned>(daysSinceEpoch - era * 146097);
  const unsigned yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
  year = static_cast<int>(yearOfEra) + era * 400;
  const unsigned dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
  const unsigned mp = (5 * dayOfYear + 2) / 153;
  day = dayOfYear - (153 * mp + 2) / 5 + 1;
  month = mp < 10 ? mp + 3 : mp - 9;
  year += month <= 2;
}

}  // namespace CalendarTime
