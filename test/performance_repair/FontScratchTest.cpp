#include <gtest/gtest.h>
#include <FontDecompressor.h>

#include <array>
#include <vector>

TEST(FontScratch, OneAllocationServesDifferentGroupsAndGlyphBytesSurviveSorting) {
  std::vector<uint8_t> compressed;
  std::array<EpdFontGroup, 3> groups{};
  std::array<EpdGlyph, 3> glyphs{};
  const EpdUnicodeInterval interval{'A', 'C', 0};
  for (uint16_t i = 0; i < 3; ++i) {
    const uint16_t size = i + 1;
    groups[i] = {static_cast<uint32_t>(compressed.size()), static_cast<uint32_t>(5 + size), size, 1, i};
    glyphs[i] = {4, static_cast<uint8_t>(size), 64, 0, 0, size, 0};
    compressed.insert(compressed.end(), {1, static_cast<uint8_t>(size), 0,
                                       static_cast<uint8_t>(~size), 255});
    for (int row = 0; row < size; ++row) compressed.push_back(0x30 + i);
  }
  EpdFontData font{};
  font.bitmap = compressed.data();
  font.glyph = glyphs.data();
  font.intervals = &interval;
  font.intervalCount = 1;
  font.groups = groups.data();
  font.groupCount = groups.size();
  font.is2Bit = true;
  FontDecompressor decompressor;
  ASSERT_TRUE(decompressor.init());
  ASSERT_EQ(decompressor.prewarmCache(&font, "ABC"), 0);
  EXPECT_EQ(decompressor.getStats().prewarmScratchAllocations, 1u);
  for (int i = 0; i < 3; ++i) {
    const auto* bitmap = decompressor.getBitmap(&font, &glyphs[i], i);
    ASSERT_NE(bitmap, nullptr);
    for (int row = 0; row <= i; ++row) EXPECT_EQ(bitmap[row], 0x30 + i);
  }
  EXPECT_EQ(decompressor.getStats().cacheMisses, 0u);
  decompressor.clearCache();
  EXPECT_EQ(decompressor.prewarmCache(&font, "CBA"), 0);
  EXPECT_EQ(decompressor.getStats().prewarmScratchAllocations, 2u);
}
