#pragma once

#include <cstdint>

#ifndef CROSSFADE_CUSTOM_ZH_TW
#define CROSSFADE_CUSTOM_ZH_TW 0
#endif

namespace CustomZhTwProfile {

inline constexpr char FONT_FAMILY[] = "GuanKiapTsingKhai";
inline constexpr uint8_t UI_POINT_SIZE = 14;
inline constexpr uint8_t READER_POINT_SIZE = 28;
// The X3's grayscale text pipeline re-renders every page twice and runs a
// second waveform. Keep it opt-in for this latency-focused private build.
inline constexpr bool DEFAULT_TEXT_ANTI_ALIASING = false;
// Idle prewarm runs from Activity::loop() on the same task that polls buttons.
// It cannot observe a new button edge until it returns, so on X3 it creates a
// visible input stall rather than useful background work. The render task's
// normal, cancellable page prewarm remains enabled.
inline constexpr bool IDLE_PAGE_PREWARM_ENABLED = CROSSFADE_CUSTOM_ZH_TW == 0;

constexpr uint32_t sizeBit(const uint8_t pointSize) {
  return pointSize < 32 ? (uint32_t{1} << pointSize) : 0;
}

inline constexpr uint32_t REQUIRED_SIZE_MASK =
    sizeBit(UI_POINT_SIZE) | sizeBit(18) | sizeBit(20) | sizeBit(22) | sizeBit(24) | sizeBit(26) |
    sizeBit(READER_POINT_SIZE);

constexpr bool assetsComplete(const uint32_t availableSizeMask) {
  return (availableSizeMask & REQUIRED_SIZE_MASK) == REQUIRED_SIZE_MASK;
}

constexpr bool shouldApplyDefaults(const bool alreadyApplied, const uint32_t availableSizeMask) {
  return !alreadyApplied && assetsComplete(availableSizeMask);
}

constexpr bool shouldApplyPerformanceDefaults(const bool fontProfileApplied,
                                              const bool performanceProfileApplied) {
  return fontProfileApplied && !performanceProfileApplied;
}

// The BookOrbit-first Home (UI theme BOOKORBIT) is applied once, after the zh-TW font profile is
// known-good, so it never lands on a device that would still render tofu. A later user theme
// choice is preserved.
constexpr bool shouldApplyHomeDefaults(const bool fontProfileApplied, const bool homeProfileApplied) {
  return fontProfileApplied && !homeProfileApplied;
}

// Journal (文藝刊物) migration, independent of and after the Home migration above. It moves only the
// product default -- theme BOOKORBIT, which the Home migration set -- to JOURNAL, and leaves any
// other saved theme alone. The marker is recorded whenever the font profile is known-good, so a
// later deliberate choice of BOOKORBIT is never migrated again. Themes are passed as raw
// persisted values (BOOKORBIT = 6, JOURNAL = 7 in CrossPointSettings::UI_THEME).
inline constexpr uint8_t THEME_BOOKORBIT = 6;
inline constexpr uint8_t THEME_JOURNAL = 7;

struct JournalMigration {
  bool markApplied;  // persist zhTwJournalProfileApplied = 1
  uint8_t theme;     // resulting uiTheme
};

constexpr JournalMigration journalMigration(const bool fontProfileApplied, const bool journalProfileApplied,
                                            const uint8_t currentTheme) {
  if (!fontProfileApplied || journalProfileApplied) return {false, currentTheme};
  return {true, currentTheme == THEME_BOOKORBIT ? THEME_JOURNAL : currentTheme};
}

}  // namespace CustomZhTwProfile
