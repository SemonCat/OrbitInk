#pragma once
#include <cstdint>
class Rtc {
 public:
  struct DateTime {
    uint16_t year = 2026;
    uint8_t month = 10, day = 4, hour = 0, minute = 0, second = 0, weekday = 0;
  };
  static inline bool present = true, readable = true, writable = true;
  static DateTime value;
  bool begin() { return present; }
  bool now(DateTime& out) { out = value; return readable; }
  bool set(const DateTime& in) { if (!writable) return false; value = in; readable = true; return true; }
};
inline Rtc::DateTime Rtc::value;
