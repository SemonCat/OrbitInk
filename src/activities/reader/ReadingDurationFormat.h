#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace ReadingDurationFormat {

// Compact "3h 20m" / "45m" / "<1m" reading-duration label.
//
// Deliberately untranslated, matching the equally compact "%.0f%%" progress label it sits next to:
// these are numeric badges in tight layouts, not prose. (The surrounding captions -- "Time Read",
// "Time Left" -- do go through tr().)
//
// Previously duplicated in BookStatsActivity.cpp and LyraCarouselTheme.cpp, whose comment noted a
// shared header was not worth it "at two call sites". The Dashboard card is the third, so it is.
inline void compact(const uint32_t seconds, char* buf, const size_t len) {
  if (seconds < 60) {
    snprintf(buf, len, "<1m");
    return;
  }
  const uint32_t hours = seconds / 3600;
  const uint32_t minutes = (seconds % 3600) / 60;
  if (hours == 0) {
    snprintf(buf, len, "%lum", static_cast<unsigned long>(minutes));
  } else {
    snprintf(buf, len, "%luh %lum", static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes));
  }
}

}  // namespace ReadingDurationFormat
