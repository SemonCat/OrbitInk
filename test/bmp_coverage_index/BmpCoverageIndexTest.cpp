#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "BmpCoverageIndex.h"

namespace {

struct Fixture {
  std::array<uint32_t, BmpCoverageIndex::PAGE_MAP_WORDS> pages{};
  std::vector<uint32_t> data;
  uint16_t pageCount = 0;
};

Fixture build(const std::vector<std::pair<uint16_t, uint16_t>>& intervals) {
  Fixture out;
  for (const auto [first, last] : intervals) {
    for (uint16_t page = first >> 8; page <= (last >> 8); ++page) {
      out.pages[page / 32] |= 1u << (page % 32);
    }
  }
  for (const uint32_t word : out.pages) out.pageCount += BmpCoverageIndex::popcount32(word);

  const size_t coverageWords = static_cast<size_t>(out.pageCount) * BmpCoverageIndex::WORDS_PER_PAGE;
  out.data.assign(coverageWords + (out.pageCount + 1) / 2, 0);
  std::array<uint8_t, BmpCoverageIndex::PAGE_COUNT> pageSlots{};
  pageSlots.fill(0xFF);
  uint8_t slot = 0;
  for (uint16_t page = 0; page < BmpCoverageIndex::PAGE_COUNT; ++page) {
    if ((out.pages[page / 32] & (1u << (page % 32))) != 0) pageSlots[page] = slot++;
  }
  for (const auto [first, last] : intervals) {
    for (uint32_t cp = first; cp <= last; ++cp) {
      const uint16_t page = static_cast<uint16_t>(cp >> 8);
      const uint16_t bit = static_cast<uint16_t>(cp & 0xFFu);
      out.data[pageSlots[page] * BmpCoverageIndex::WORDS_PER_PAGE + bit / 32] |= 1u << (bit % 32);
    }
  }
  auto* offsets = reinterpret_cast<uint16_t*>(out.data.data() + coverageWords);
  uint16_t glyphOffset = 0;
  for (uint16_t pageSlot = 0; pageSlot < out.pageCount; ++pageSlot) {
    offsets[pageSlot] = glyphOffset;
    for (uint16_t word = 0; word < BmpCoverageIndex::WORDS_PER_PAGE; ++word) {
      glyphOffset += BmpCoverageIndex::popcount32(
          out.data[pageSlot * BmpCoverageIndex::WORDS_PER_PAGE + word]);
    }
  }
  return out;
}

int32_t intervalLookup(const std::vector<std::pair<uint16_t, uint16_t>>& intervals, uint32_t codepoint) {
  uint32_t offset = 0;
  for (const auto [first, last] : intervals) {
    if (codepoint < first) return -1;
    if (codepoint <= last) return static_cast<int32_t>(offset + codepoint - first);
    offset += last - first + 1;
  }
  return -1;
}

}  // namespace

TEST(BmpCoverageIndex, MatchesIntervalLookupAcrossEntireBmp) {
  const std::vector<std::pair<uint16_t, uint16_t>> intervals = {
      {0x0020, 0x007E}, {0x3400, 0x3403}, {0x4837, 0x4837}, {0x4E00, 0x4E08},
      {0x9019, 0x9019}, {0xFF01, 0xFF0F}, {0xFFFD, 0xFFFD},
  };
  const Fixture fixture = build(intervals);
  for (uint32_t cp = 0; cp <= UINT16_MAX; ++cp) {
    EXPECT_EQ(intervalLookup(intervals, cp),
              BmpCoverageIndex::findGlyphIndex(fixture.pages.data(), fixture.data.data(), fixture.pageCount, cp))
        << "U+" << std::hex << cp;
  }
}

TEST(BmpCoverageIndex, RejectsSupplementaryCodepointsAndMissingPages) {
  const auto fixture = build({{0x4E00, 0x4E02}});
  EXPECT_EQ(-1, BmpCoverageIndex::findGlyphIndex(fixture.pages.data(), fixture.data.data(), fixture.pageCount, 0x3400));
  EXPECT_EQ(-1, BmpCoverageIndex::findGlyphIndex(fixture.pages.data(), fixture.data.data(), fixture.pageCount, 0x10000));
}
