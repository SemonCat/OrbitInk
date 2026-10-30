#pragma once
#include <algorithm>
#include <cstdint>

namespace glyphArena {
// Leave working headroom outside the single contiguous bitmap allocation.
constexpr uint32_t RESERVE = 4096;
constexpr uint32_t retryCapacity(uint32_t requested, uint32_t bytesPerGlyph, uint32_t maxAlloc) {
  if (requested <= 1 || maxAlloc <= RESERVE) return 0;
  const uint32_t fit = (maxAlloc - RESERVE) / (bytesPerGlyph ? bytesPerGlyph : 1);
  return std::min(requested - 1, fit);
}
}  // namespace glyphArena
