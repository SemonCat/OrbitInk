#include "ReaderFontSizes.h"

#include <algorithm>
#include <cstring>
#include <iterator>

std::vector<uint8_t> readerFontPointSizes(const SdCardFontRegistry* registry, const char* sdFamilyName) {
  if (registry && sdFamilyName && sdFamilyName[0] != '\0') {
    if (const auto* family = registry->findFamily(sdFamilyName)) {
      auto sizes = family->availableSizes();
#if CROSSFADE_CUSTOM_ZH_TW
      if (std::strcmp(sdFamilyName, "GuanKiapTsingKhai") == 0) {
        sizes.erase(std::remove_if(sizes.begin(), sizes.end(), [](const uint8_t pointSize) {
                      return pointSize < CUSTOM_ZH_TW_READER_MIN_POINT_SIZE ||
                             pointSize > CUSTOM_ZH_TW_READER_MAX_POINT_SIZE;
                    }),
                    sizes.end());
      }
#endif
      if (!sizes.empty()) return sizes;
    }
  }
  return {std::begin(BUILTIN_READER_POINT_SIZES), std::end(BUILTIN_READER_POINT_SIZES)};
}

uint8_t snapToNearestPointSize(const uint8_t* sizes, const size_t count, const uint8_t pt) {
  if (!sizes || count == 0) return pt;

  uint8_t best = sizes[0];
  uint8_t bestDelta = best > pt ? best - pt : pt - best;
  for (size_t i = 1; i < count; i++) {
    const uint8_t delta = sizes[i] > pt ? sizes[i] - pt : pt - sizes[i];
    // Strictly-less keeps the smaller size on a tie, since `sizes` is ascending.
    if (delta < bestDelta) {
      best = sizes[i];
      bestDelta = delta;
    }
  }
  return best;
}

uint8_t snapReaderPointSize(const SdCardFontRegistry* registry, const char* sdFamilyName, const uint8_t pt) {
  return snapToNearestPointSize(readerFontPointSizes(registry, sdFamilyName), pt);
}
