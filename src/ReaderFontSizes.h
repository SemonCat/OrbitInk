#pragma once

#include <SdCardFontRegistry.h>

#include <cstddef>
#include <cstdint>
#include <vector>

// Reader font size is stored as an actual point size (see CrossPointSettings::
// fontPointSize), not an abstract Small/Medium/Large slot. The selectable sizes
// therefore come from whichever family is active: the built-in set below, or the
// .cpfont files a user installed for an SD family.

// The built-in Noto Serif / Noto Sans families are compiled in at exactly these
// point sizes (see the global font objects in main.cpp).
inline constexpr uint8_t BUILTIN_READER_POINT_SIZES[] = {10, 12, 14, 16, 18};

#if CROSSFADE_CUSTOM_ZH_TW
// The custom package keeps a 14pt file for the CJK UI fallback, but that
// implementation detail must not make 14pt appear as a reader choice.
inline constexpr uint8_t CUSTOM_ZH_TW_READER_MIN_POINT_SIZE = 18;
inline constexpr uint8_t CUSTOM_ZH_TW_READER_MAX_POINT_SIZE = 28;
#endif

// Point sizes selectable for the active reader font, ascending: the SD family's
// installed sizes when `sdFamilyName` names one the registry knows, otherwise
// the built-in set. Never returns empty.
std::vector<uint8_t> readerFontPointSizes(const SdCardFontRegistry* registry, const char* sdFamilyName);

// Closest entry in `sizes` (ascending, `count` > 0) to `pt`; ties resolve to the
// smaller size. Takes a raw range rather than a vector because getReaderFontId()
// runs inside the page render loop and must not allocate.
uint8_t snapToNearestPointSize(const uint8_t* sizes, size_t count, uint8_t pt);

inline uint8_t snapToNearestPointSize(const std::vector<uint8_t>& sizes, const uint8_t pt) {
  return sizes.empty() ? pt : snapToNearestPointSize(sizes.data(), sizes.size(), pt);
}

// Snap against the reader-visible sizes for a family. This can differ from
// every installed file: the X3 zh-TW package, for example, installs a 14pt
// UI-only fallback that must never become the reader font.
uint8_t snapReaderPointSize(const SdCardFontRegistry* registry, const char* sdFamilyName, uint8_t pt);
