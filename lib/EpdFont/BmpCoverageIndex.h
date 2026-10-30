#pragma once

#include <cstddef>
#include <cstdint>

namespace BmpCoverageIndex {

constexpr uint16_t PAGE_COUNT = 256;
constexpr uint16_t PAGE_MAP_WORDS = PAGE_COUNT / 32;
constexpr uint16_t WORDS_PER_PAGE = 8;

inline uint16_t popcount32(const uint32_t value) {
  return static_cast<uint16_t>(__builtin_popcount(value));
}

// Returns the glyph-table index for a BMP codepoint. Coverage words are
// packed by populated 256-codepoint page; a uint16_t prefix offset follows
// those words for each populated page. The rank of a set bit is therefore the
// glyph index used by the on-disk .cpfont table.
inline int32_t findGlyphIndex(const uint32_t* coveredPages, const uint32_t* coverageData,
                              const uint16_t coveredPageCount, const uint32_t codepoint) {
  if (!coveredPages || !coverageData || codepoint > UINT16_MAX) return -1;

  const uint16_t bmpCodepoint = static_cast<uint16_t>(codepoint);
  const uint16_t page = bmpCodepoint >> 8;
  const uint16_t pageWord = page / 32;
  const uint32_t pageMask = 1u << (page % 32);
  if ((coveredPages[pageWord] & pageMask) == 0) return -1;

  uint16_t pageSlot = 0;
  for (uint16_t word = 0; word < pageWord; ++word) {
    pageSlot += popcount32(coveredPages[word]);
  }
  pageSlot += popcount32(coveredPages[pageWord] & (pageMask - 1u));
  if (pageSlot >= coveredPageCount) return -1;

  const uint16_t bitInPage = bmpCodepoint & 0xFFu;
  const uint16_t firstWord = pageSlot * WORDS_PER_PAGE;
  const uint16_t targetWord = bitInPage / 32;
  const uint32_t targetMask = 1u << (bitInPage % 32);
  if ((coverageData[firstWord + targetWord] & targetMask) == 0) return -1;

  const size_t coverageWordCount = static_cast<size_t>(coveredPageCount) * WORDS_PER_PAGE;
  const auto* pageGlyphOffsets = reinterpret_cast<const uint16_t*>(coverageData + coverageWordCount);
  uint32_t glyphIndex = pageGlyphOffsets[pageSlot];
  for (uint16_t word = 0; word < targetWord; ++word) {
    glyphIndex += popcount32(coverageData[firstWord + word]);
  }
  glyphIndex += popcount32(coverageData[firstWord + targetWord] & (targetMask - 1u));
  return static_cast<int32_t>(glyphIndex);
}

}  // namespace BmpCoverageIndex
