#pragma once

#include <cstdint>

namespace ProgressTimestamp {

// Reading-position timestamps use Unix seconds on disk and on BookOrbit's wire API. The lower
// bound rejects an unset/lost-power RTC; the upper bound rejects millisecond values and corrupt
// far-future clocks without tying the firmware to today's date.
constexpr uint64_t MIN_VALID = 946684801ULL;   // just after 2000-01-01T00:00:00Z
constexpr uint64_t MAX_VALID = 4102444799ULL;  // 2099-12-31T23:59:59Z

constexpr bool valid(uint64_t value) { return value >= MIN_VALID && value <= MAX_VALID; }

}  // namespace ProgressTimestamp
