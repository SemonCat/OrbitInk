// Regression coverage for SdCardFont::releaseTransientRenderCache(), the
// force-release API added to fix the X3 KOSync heap-fragmentation bug: a
// retained 1-bit 28pt page glyph mini-bitmap (~19.9KB) kept MaxAlloc below
// KOReaderSyncClient's MIN_BLOCK_FOR_TLS=20000 gate even though
// SdCardFont::resetStyleMiniData()'s normal retention floor (>=40KiB free
// heap) had plenty of margin -- FontCacheManager::clearCache() therefore did
// not guarantee release. This test exercises the REAL SdCardFont/
// FontCacheManager code (compiled directly, not reimplemented) against an
// in-memory .cpfont, proving:
//   1. releaseTransientRenderCache() frees mini glyph/bitmap data
//      UNCONDITIONALLY, even when the stubbed free-heap is high enough that
//      the existing clearCache()/resetStyleMiniData() retention bet would
//      keep it (this is the exact scenario the device hit: retention isn't
//      gated on low heap alone -- see MINI_RETAIN_MIN_FREE_HEAP).
//   2. Coverage/interval lookup, style metrics, and the persistent advance
//      table all survive the release and keep working afterward.
//   3. FontCacheManager::forceReleaseTransientCaches() correctly routes the
//      release across every SdCardFont it's given (the multi-font seam real
//      callers -- KOReaderAutoSync, KOReaderSyncActivity -- actually use).
//
// Before this fix, releaseTransientRenderCache()/forceReleaseTransientCaches()
// do not exist: this test does not compile against the pre-fix tree. That
// is this regression test's "red" state (see the diagnosing-bugs skill's
// Phase 5) -- there is no pre-fix runtime behavior to assert against, since
// the whole point of the fix is a NEW API. Reverting the fix and attempting
// to build this test reproduces that red state directly.

#include <FontCacheManager.h>
#include <GlyphArenaRetry.h>
#include <SdCardFont.h>
#include <Utf8.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <map>
#include <new>
#include <string>
#include <vector>

#include "HalStorage.h"
#include "../../lib/Epub/Epub/converters/PngMemory.h"

// Fail only the tiny fixture's bitmap sizes; all other real allocations run normally.
static size_t rejectBitmapAtLeast = 0;
static size_t largestNothrowArray = 0;
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  if (rejectBitmapAtLeast && size >= rejectBitmapAtLeast && size <= 4096 && size % 300 == 0) return nullptr;
  largestNothrowArray = std::max(largestNothrowArray, size);
  try {
    return ::operator new[](size);
  } catch (...) {
    return nullptr;
  }
}

namespace {

constexpr char TEST_PATH[] = "/test.cpfont";

void appendU8(std::vector<uint8_t>& buf, uint8_t v) { buf.push_back(v); }
void appendU16(std::vector<uint8_t>& buf, uint16_t v) {
  buf.push_back(static_cast<uint8_t>(v & 0xFF));
  buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}
void appendI16(std::vector<uint8_t>& buf, int16_t v) { appendU16(buf, static_cast<uint16_t>(v)); }
void appendU32(std::vector<uint8_t>& buf, uint32_t v) {
  buf.push_back(static_cast<uint8_t>(v & 0xFF));
  buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

// Builds a minimal, valid, single-style (regular), no-kern/no-ligature v4
// .cpfont covering the supplied codepoints, each a trivial 4x2
// 2-bit glyph. Field layout matches lib/EpdFont/scripts/fontconvert_sdcard.py's
// GLYPH_STRUCT_FORMAT/STYLE_TOC_FORMAT exactly (independently re-verified
// against the real generator during the X3 KOSync review this fix followed
// up on) and the on-disk struct sizes SdCardFont.cpp's own static_asserts
// enforce (EpdGlyph=16B, EpdUnicodeInterval=12B).
// Real .cpfont glyphs carry exactly the packed size of width*height 2-bit pixels (SdCardFont
// rejects anything else as corrupt), so derive dimensions from each requested bitmap size.
// Height starts at 2 so the default 2-byte glyph stays the historical 4x2.
std::pair<uint8_t, uint8_t> dimsFor2Bit(uint32_t bytes) {
  for (uint32_t h = bytes ? 2 : 0; h <= 255; ++h) {
    for (uint32_t w = 1; w <= 255; ++w) {
      if ((w * h + 3) / 4 == bytes) return {static_cast<uint8_t>(w), static_cast<uint8_t>(h)};
    }
  }
  ADD_FAILURE() << "no 2-bit glyph dimensions pack to " << bytes << " bytes";
  return {0, 0};
}

std::vector<uint8_t> buildCpfont(const std::vector<uint32_t>& codepoints, uint16_t bitmapBytes = 2,
                                 const std::vector<uint16_t>* bitmapSizes = nullptr) {
  std::vector<uint32_t> allCodepoints = codepoints;
  if (std::find(allCodepoints.begin(), allCodepoints.end(), REPLACEMENT_GLYPH) == allCodepoints.end()) {
    allCodepoints.push_back(REPLACEMENT_GLYPH);
  }
  std::sort(allCodepoints.begin(), allCodepoints.end());
  const uint32_t NUM_GLYPHS = static_cast<uint32_t>(allCodepoints.size());

  // prewarm() always appends REPLACEMENT_GLYPH (Utf8.h, 0xFFFD) to the
  // requested codepoint set if it isn't already present, so it must be
  // covered too or every prewarm() call on this font reports a miss.
  std::vector<uint8_t> intervals;
  uint32_t intervalCount = 0;
  uint32_t rangeStart = 0;
  for (uint32_t i = 1; i <= NUM_GLYPHS; i++) {
    if (i == NUM_GLYPHS || allCodepoints[i] != allCodepoints[i - 1] + 1) {
      appendU32(intervals, allCodepoints[rangeStart]);
      appendU32(intervals, allCodepoints[i - 1]);
      appendU32(intervals, rangeStart);
      intervalCount++;
      rangeStart = i;
    }
  }

  std::vector<uint8_t> glyphs;
  for (uint32_t i = 0; i < NUM_GLYPHS; i++) {
    const uint32_t glyphBitmapBytes = bitmapSizes ? (*bitmapSizes)[i] : bitmapBytes;
    const auto dims = dimsFor2Bit(glyphBitmapBytes);
    appendU8(glyphs, dims.first);                                  // width
    appendU8(glyphs, dims.second);                                 // height
    appendU16(glyphs, 16);                                         // advanceX (12.4 fixed point == 1px)
    appendI16(glyphs, 0);                                          // left
    appendI16(glyphs, 2);                                          // top
    appendU16(glyphs, static_cast<uint16_t>(glyphBitmapBytes));  // dataLength
    appendU16(glyphs, 0);                                          // padding (2x in GLYPH_FORMAT)
    uint32_t bitmapOffset = 0;
    for (uint32_t prior = 0; prior < i; prior++) {
      bitmapOffset += bitmapSizes ? (*bitmapSizes)[prior] : bitmapBytes;
    }
    appendU32(glyphs, bitmapOffset);  // dataOffset within bitmap
  }

  std::vector<uint8_t> bitmap;
  for (uint32_t i = 0; i < NUM_GLYPHS; i++) {
    // Arbitrary non-zero 2-bit pixels so the glyph is visually "real" --
    // content doesn't matter for this test, only that it round-trips.
    const uint32_t glyphBitmapBytes = bitmapSizes ? (*bitmapSizes)[i] : bitmapBytes;
    for (uint32_t b = 0; b < glyphBitmapBytes; ++b) bitmap.push_back(b % 2 ? 0x55 : 0xAA);
  }

  const uint32_t glyphCount = NUM_GLYPHS;
  const uint32_t dataOffset = 32 /*header*/ + 32 /*one TOC entry*/;

  std::vector<uint8_t> file;
  // Header: magic(8) + version(2) + flags(2) + styleCount(1) + reserved(19) = 32
  const char magic[8] = {'C', 'P', 'F', 'O', 'N', 'T', '\0', '\0'};
  file.insert(file.end(), magic, magic + 8);
  appendU16(file, 4);  // CPFONT_VERSION
  appendU16(file, 1);  // flags: bit0=1 -> is2Bit
  appendU8(file, 1);   // styleCount
  file.insert(file.end(), 19, 0);
  EXPECT_EQ(file.size(), 32u);

  // Style TOC entry (32 bytes): styleId(1)+pad(3)+intervalCount(4)+glyphCount(4)+
  // advanceY(1)+ascender(2)+descender(2)+kernL(2)+kernR(2)+kernLCls(1)+kernRCls(1)+
  // ligCount(1)+dataOffset(4)+reserved(4)
  appendU8(file, 0);  // styleId = REGULAR
  file.insert(file.end(), 3, 0);
  appendU32(file, intervalCount);
  appendU32(file, glyphCount);
  appendU8(file, 16);   // advanceY
  appendI16(file, 14);  // ascender
  appendI16(file, -4);  // descender
  appendU16(file, 0);   // kernLeftEntryCount
  appendU16(file, 0);   // kernRightEntryCount
  appendU8(file, 0);    // kernLeftClassCount
  appendU8(file, 0);    // kernRightClassCount
  appendU8(file, 0);    // ligaturePairCount
  appendU32(file, dataOffset);
  file.insert(file.end(), 4, 0);
  EXPECT_EQ(file.size(), 64u);

  file.insert(file.end(), intervals.begin(), intervals.end());
  file.insert(file.end(), glyphs.begin(), glyphs.end());
  // No kern/ligature sections (all counts are 0 above).
  file.insert(file.end(), bitmap.begin(), bitmap.end());
  return file;
}

std::vector<uint8_t> buildMinimalTwoGlyphCpfont(uint16_t bitmapBytes = 2) {
  return buildCpfont({'A', 'B'}, bitmapBytes);
}

std::vector<uint8_t> buildFiveGlyphCpfont(uint16_t bitmapBytes = 2) {
  return buildCpfont({'A', 'B', 'C', 'D'}, bitmapBytes);
}

// A, B small; C..I near the largest real glyph (254x251 2-bit), seven of which exceed the
// 96 KiB union arena cap; then the replacement glyph.
std::vector<uint8_t> buildVariedLargeGlyphCpfont() {
  const std::vector<uint16_t> bitmapSizes = {2, 2, 15939, 15939, 15939, 15939, 15939, 15939, 15939, 2};
  return buildCpfont({'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I'}, 2, &bitmapSizes);
}

// Glyph record `index` of the single style written by buildCpfont().
size_t glyphRecordOffset(const std::vector<uint8_t>& file, uint32_t index) {
  uint32_t intervalCount = 0;
  std::memcpy(&intervalCount, file.data() + 32 + 4, sizeof(intervalCount));
  return 64 + intervalCount * 12 + index * sizeof(EpdGlyph);
}

EpdGlyph readGlyphRecord(const std::vector<uint8_t>& file, uint32_t index) {
  EpdGlyph glyph{};
  std::memcpy(&glyph, file.data() + glyphRecordOffset(file, index), sizeof(glyph));
  return glyph;
}

void writeGlyphRecord(std::vector<uint8_t>& file, uint32_t index, const EpdGlyph& glyph) {
  std::memcpy(file.data() + glyphRecordOffset(file, index), &glyph, sizeof(glyph));
}

}  // namespace

class SdCardFontReleaseTest : public ::testing::Test {
 protected:
  void SetUp() override {
    rejectBitmapAtLeast = 0;
    HalStorage::resetTestFiles();
    HalStorage::registerTestFile(TEST_PATH, buildMinimalTwoGlyphCpfont());
    ASSERT_TRUE(font_.load(TEST_PATH));
    ESP.freeHeap = 0xFFFFFFFFu;  // plenty of "free heap" by default
    ESP.maxAllocHeap = 0xFFFFFFFFu;
  }

  static constexpr uint8_t STYLE_REGULAR = 0;
  SdCardFont font_;
};

bool miniDataResident(SdCardFont& font, uint8_t style) {
  EpdFont* f = font.getEpdFont(style);
  return f != nullptr && f->data != nullptr && f->data->glyph != nullptr && f->data->intervalCount > 0;
}

TEST_F(SdCardFontReleaseTest, LoadExposesCoverageAndStyle) {
  EXPECT_TRUE(font_.hasStyle(STYLE_REGULAR));
  EXPECT_EQ(font_.styleCount(), 1);
  // No mini data yet -- nothing has been prewarmed.
  EXPECT_FALSE(miniDataResident(font_, STYLE_REGULAR));
}

TEST_F(SdCardFontReleaseTest, PrewarmPopulatesMiniDataAndAdvanceTable) {
  const int missed = font_.prewarm("AB", 0x01 /*regular*/, /*metadataOnly=*/false);
  EXPECT_EQ(missed, 0);
  EXPECT_TRUE(miniDataResident(font_, STYLE_REGULAR));

  ASSERT_EQ(font_.buildAdvanceTable("AB", 0x01), 0);
  EXPECT_TRUE(font_.hasAdvanceTable());
  EXPECT_EQ(font_.getAdvance('A', STYLE_REGULAR), 16);
  EXPECT_EQ(font_.getAdvance('B', STYLE_REGULAR), 16);
}

// This is the test that would have failed before this fix existed: the
// existing clearCache() path (via resetStyleMiniData()'s heap-floor bet)
// RETAINS mini data whenever free heap looks generous, exactly the device's
// actual state (Free 40,740 / MaxAlloc 19,444 -- plenty of "free heap" by the
// 40KiB floor's own measure, yet the retained ~19.9KB mini bitmap was still
// the thing keeping MaxAlloc under the TLS gate's MIN_BLOCK_FOR_TLS=20000).
TEST_F(SdCardFontReleaseTest, ClearCacheRetainsMiniDataWhenHeapLooksFree) {
  ASSERT_EQ(font_.prewarm("AB", 0x01, false), 0);
  ASSERT_TRUE(miniDataResident(font_, STYLE_REGULAR));

  ESP.freeHeap = 74948;  // matches the device log's reported free heap
  font_.clearCache();

  EXPECT_TRUE(miniDataResident(font_, STYLE_REGULAR))
      << "clearCache() is expected to retain mini data above the heap floor -- "
         "this documents existing, intentional behavior, not a bug.";
}

TEST_F(SdCardFontReleaseTest, ReleaseTransientRenderCacheForcesReleaseRegardlessOfFreeHeap) {
  ASSERT_EQ(font_.prewarm("AB", 0x01, false), 0);
  ASSERT_TRUE(miniDataResident(font_, STYLE_REGULAR));
  ASSERT_EQ(font_.buildAdvanceTable("AB", 0x01), 0);
  ASSERT_TRUE(font_.hasAdvanceTable());

  // Same generous "free heap" as the retention test above -- clearCache()
  // would keep the mini data here. releaseTransientRenderCache() must not.
  ESP.freeHeap = 74948;
  font_.releaseTransientRenderCache();

  EXPECT_FALSE(miniDataResident(font_, STYLE_REGULAR))
      << "releaseTransientRenderCache() must free mini data unconditionally, "
         "not subject to the resetStyleMiniData() heap-floor retention bet.";

  // Persistent structures must survive: style/coverage metadata and the
  // advance table were NOT touched by the release.
  EXPECT_TRUE(font_.hasStyle(STYLE_REGULAR));
  EXPECT_TRUE(font_.hasAdvanceTable());
  EXPECT_EQ(font_.getAdvance('A', STYLE_REGULAR), 16);
  EXPECT_EQ(font_.getAdvance('B', STYLE_REGULAR), 16);

  // Coverage/interval lookup must still work: re-prewarming the same text
  // must succeed with zero misses using only the still-resident coverage
  // index, without reloading the font file.
  EXPECT_EQ(font_.prewarm("AB", 0x01, false), 0);
  EXPECT_TRUE(miniDataResident(font_, STYLE_REGULAR));
}

TEST_F(SdCardFontReleaseTest, ReleaseIsSafeAndIdempotentWithNoPriorPrewarm) {
  // Calling release before anything was ever prewarmed (e.g. a KOSync call
  // that fires before the reader ever drew a page) must not crash and must
  // leave the font in a normal, usable state.
  EXPECT_FALSE(miniDataResident(font_, STYLE_REGULAR));
  font_.releaseTransientRenderCache();
  EXPECT_FALSE(miniDataResident(font_, STYLE_REGULAR));
  EXPECT_TRUE(font_.hasStyle(STYLE_REGULAR));
  EXPECT_EQ(font_.prewarm("A", 0x01, false), 0);
}

TEST_F(SdCardFontReleaseTest, RetainedUnionAvoidsRepeatedReadsAndKeepsRequestedGlyphs) {
  HalStorage::registerTestFile(TEST_PATH, buildFiveGlyphCpfont());
  ASSERT_TRUE(font_.load(TEST_PATH));

  ASSERT_EQ(font_.prewarm("AB", 0x01, false), 0);
  HalStorage::resetReadCount();
  ASSERT_EQ(font_.prewarm("CD", 0x01, false), 0);
  const size_t unionReadCount = HalStorage::readCount();
  ASSERT_GT(unionReadCount, 0u);

  // The first request remains resident after the second prewarm, so returning
  // to it is a subset hit rather than another SD metadata/bitmap read.
  HalStorage::resetReadCount();
  EXPECT_EQ(font_.prewarm("AB", 0x01, false), 0);
  EXPECT_EQ(HalStorage::readCount(), 0u);

  // The newly requested glyphs are resident too; this also proves the union
  // did not silently retain only the old body glyphs.
  EXPECT_NE(font_.getEpdFont(0)->getGlyph('C'), nullptr);
  EXPECT_NE(font_.getEpdFont(0)->getGlyph('D'), nullptr);
}

TEST_F(SdCardFontReleaseTest, RetainedUnionFallsBackUnderHeapPressure) {
  HalStorage::registerTestFile(TEST_PATH, buildFiveGlyphCpfont(300));
  ASSERT_TRUE(font_.load(TEST_PATH));
  ASSERT_EQ(font_.prewarm("AB", 0x01, false), 0);

  ESP.freeHeap = 20 * 1024;
  ESP.maxAllocHeap = 20 * 1024;
  HalStorage::resetReadCount();
  EXPECT_EQ(font_.prewarm("CD", 0x01, false), 0);
  EXPECT_GT(HalStorage::readCount(), 0u);
  EXPECT_NE(font_.getEpdFont(0)->getGlyph('C'), nullptr);
  EXPECT_NE(font_.getEpdFont(0)->getGlyph('D'), nullptr);
}

TEST_F(SdCardFontReleaseTest, RetainedUnionDownstreamBitmapFailureRetriesRequestedPage) {
  HalStorage::registerTestFile(TEST_PATH, buildFiveGlyphCpfont(300));
  ASSERT_TRUE(font_.load(TEST_PATH));
  ASSERT_EQ(font_.prewarm("AB", 0x01, false), 0);  // resident bitmap: 900 bytes

  // The five-glyph union needs 1500 bytes, but the requested CD page needs
  // only 900. Reject only the union-sized bitmap allocation to prove a failure
  // after union selection retries the original request instead of reporting
  // misses for the enlarged union.
  rejectBitmapAtLeast = 1500;
  HalStorage::resetReadCount();
  EXPECT_EQ(font_.prewarm("CD", 0x01, false), 0);
  EXPECT_GT(HalStorage::readCount(), 0u);
  EXPECT_NE(font_.getEpdFont(0)->getGlyph('C'), nullptr);
  EXPECT_NE(font_.getEpdFont(0)->getGlyph('D'), nullptr);

  // The oversized union must have dropped optional AB before bitmap growth.
  // Re-requesting AB therefore requires SD reads; an uncapped retained union
  // would incorrectly make this a zero-read subset hit.
  HalStorage::resetReadCount();
  EXPECT_EQ(font_.prewarm("AB", 0x01, false), 0);
  EXPECT_GT(HalStorage::readCount(), 0u);
}

TEST_F(SdCardFontReleaseTest, ActualUnionBytesDropOptionalRetentionBeforeGrowth) {
  HalStorage::registerTestFile(TEST_PATH, buildVariedLargeGlyphCpfont());
  ASSERT_TRUE(font_.load(TEST_PATH));
  ASSERT_EQ(font_.prewarm("AB", 0x01, false), 0);

  // The old resident average is tiny, so the admission estimate would pass;
  // the actual C..I metadata makes the union exceed 96 KiB. The
  // optional union must be dropped before allocating that oversized bitmap,
  // while the requested page remains eligible for the normal arena retry.
  HalStorage::resetReadCount();
  EXPECT_EQ(font_.prewarm("CDEFGHI", 0x01, false), 0);
  EXPECT_GT(HalStorage::readCount(), 0u);
  EXPECT_NE(font_.getEpdFont(0)->getGlyph('C'), nullptr);
  EXPECT_NE(font_.getEpdFont(0)->getGlyph('I'), nullptr);

  // The oversized union must have dropped optional AB before bitmap growth.
  // Re-requesting AB therefore requires SD reads; an uncapped retained union
  // would incorrectly make this a zero-read subset hit.
  HalStorage::resetReadCount();
  EXPECT_EQ(font_.prewarm("AB", 0x01, false), 0);
  EXPECT_GT(HalStorage::readCount(), 0u);
}

struct CancelAfterCalls {
  int calls = 0;
  int cancelAt = 0;
};

bool cancelAfterCalls(const void* context) {
  auto* state = static_cast<CancelAfterCalls*>(const_cast<void*>(context));
  return ++state->calls >= state->cancelAt;
}

TEST_F(SdCardFontReleaseTest, RetainedUnionCancellationLeavesPreviousCacheUsable) {
  HalStorage::registerTestFile(TEST_PATH, buildFiveGlyphCpfont());
  ASSERT_TRUE(font_.load(TEST_PATH));
  ASSERT_EQ(font_.prewarm("AB", 0x01, false), 0);
  CancelAfterCalls cancellation{.cancelAt = 3};  // top-level, subset check, then post-union mapping
  EXPECT_EQ(font_.prewarm("CD", 0x01, false, &cancelAfterCalls, &cancellation), SdCardFont::PREWARM_CANCELLED);
  EXPECT_EQ(font_.prewarm("CD", 0x01, false), 0);
  EXPECT_NE(font_.getEpdFont(0)->getGlyph('C'), nullptr);
}

TEST_F(SdCardFontReleaseTest, MetadataOnlyThenFullRequestRebuildsBitmapUnion) {
  HalStorage::registerTestFile(TEST_PATH, buildFiveGlyphCpfont());
  ASSERT_TRUE(font_.load(TEST_PATH));
  ASSERT_EQ(font_.prewarm("AB", 0x01, true), 0);
  ASSERT_EQ(font_.prewarm("CD", 0x01, false), 0);
  EXPECT_NE(font_.getEpdFont(0)->getGlyph('A'), nullptr);
  EXPECT_NE(font_.getEpdFont(0)->getGlyph('C'), nullptr);
}

// Test-only friend (declared as such in FontCacheManager.h) exposing the
// private scan-slot state needed to prove capacity is actually bounded and
// released, not just that fonts keep working afterward.
class FontCacheManagerTestAccess {
 public:
  static size_t scanSlotCapacity(const FontCacheManager& fcm, int index) {
    return fcm.scanSlots_[index].text.capacity();
  }
  static size_t scanSlotCapacityCap() { return FontCacheManager::kScanSlotCapacityCap; }
};

// --- FontCacheManager routing: does forceReleaseTransientCaches() actually
// reach every registered SdCardFont, the way the real multi-font call sites
// (reader font + shared UI-fallback font) need it to? ---

class FontCacheManagerReleaseRoutingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    rejectBitmapAtLeast = 0;
    HalStorage::resetTestFiles();
    HalStorage::registerTestFile("/font_a.cpfont", buildMinimalTwoGlyphCpfont());
    HalStorage::registerTestFile("/font_b.cpfont", buildMinimalTwoGlyphCpfont());
    ASSERT_TRUE(fontA_.load("/font_a.cpfont"));
    ASSERT_TRUE(fontB_.load("/font_b.cpfont"));
    sdCardFonts_[1] = &fontA_;
    sdCardFonts_[2] = &fontB_;
    ESP.freeHeap = 0xFFFFFFFFu;
    ESP.maxAllocHeap = 0xFFFFFFFFu;
  }

  std::map<int, EpdFontFamily> fontMap_;  // empty: this test only exercises the SD-font path
  std::map<int, SdCardFont*> sdCardFonts_;
  SdCardFont fontA_;
  SdCardFont fontB_;
};

TEST_F(FontCacheManagerReleaseRoutingTest, ReleasesEveryRegisteredSdCardFont) {
  FontCacheManager fcm(fontMap_, sdCardFonts_);

  ASSERT_EQ(fontA_.prewarm("AB", 0x01, false), 0);
  ASSERT_EQ(fontB_.prewarm("AB", 0x01, false), 0);
  ASSERT_TRUE(miniDataResident(fontA_, 0));
  ASSERT_TRUE(miniDataResident(fontB_, 0));

  ESP.freeHeap = 74948;  // generous by the retention floor's own measure
  fcm.forceReleaseTransientCaches();

  EXPECT_FALSE(miniDataResident(fontA_, 0)) << "font A (e.g. the reader font) must be released";
  EXPECT_FALSE(miniDataResident(fontB_, 0)) << "font B (e.g. the shared UI fallback font) must be released";

  // Both fonts stay independently usable afterward.
  EXPECT_EQ(fontA_.prewarm("AB", 0x01, false), 0);
  EXPECT_EQ(fontB_.prewarm("AB", 0x01, false), 0);
}

TEST(GlyphArenaRetry, CapacityHonorsHeadroomAndBounds) {
  EXPECT_EQ(glyphArena::retryCapacity(512, 100, 20000), 159u);
  EXPECT_EQ(glyphArena::retryCapacity(512, 100, 4096), 0u);
  EXPECT_EQ(glyphArena::retryCapacity(512, 100, 4095), 0u);
  EXPECT_EQ(glyphArena::retryCapacity(512, 100, 4195), 0u);
  EXPECT_EQ(glyphArena::retryCapacity(512, 100, 4196), 1u);
  EXPECT_EQ(glyphArena::retryCapacity(3, 0, UINT32_MAX), 2u);
  EXPECT_EQ(glyphArena::retryCapacity(1, 1, UINT32_MAX), 0u);
  EXPECT_EQ(glyphArena::retryCapacity(0, 1, UINT32_MAX), 0u);
}
TEST_F(SdCardFontReleaseTest, ArenaRetriesSmallerPrefix) {
  HalStorage::registerTestFile(TEST_PATH, buildMinimalTwoGlyphCpfont(300));
  ASSERT_TRUE(font_.load(TEST_PATH));
  rejectBitmapAtLeast = 900;
  EXPECT_EQ(font_.prewarm("AB", 1, false), 1);
  rejectBitmapAtLeast = 0;
  ASSERT_TRUE(miniDataResident(font_, 0));
  EXPECT_EQ(font_.getEpdFont(0)->data->intervals[0].last, static_cast<uint32_t>('B'));
}
TEST_F(SdCardFontReleaseTest, ArenaBacksOffAgainAfterEstimateFails) {
  HalStorage::registerTestFile(TEST_PATH, buildMinimalTwoGlyphCpfont(300));
  ASSERT_TRUE(font_.load(TEST_PATH));
  rejectBitmapAtLeast = 600;
  EXPECT_EQ(font_.prewarm("AB", 1, false), 2);
  rejectBitmapAtLeast = 0;
  ASSERT_TRUE(miniDataResident(font_, 0));
  EXPECT_EQ(font_.getEpdFont(0)->data->intervals[0].last, static_cast<uint32_t>('A'));
  // A glyph outside the reduced arena still renders through the existing SD overflow ring.
  const auto* glyph = font_.getEpdFont(0)->getGlyph('B');
  ASSERT_NE(glyph, nullptr);
  EXPECT_TRUE(font_.isOverflowGlyph(glyph));
  ASSERT_NE(font_.getOverflowBitmap(glyph), nullptr);
  EXPECT_EQ(font_.getOverflowBitmap(glyph)[0], 0xAA);
}
TEST_F(SdCardFontReleaseTest, ArenaCleanFailureKeepsCoverageAndCanRecover) {
  HalStorage::registerTestFile(TEST_PATH, buildMinimalTwoGlyphCpfont(300));
  ASSERT_TRUE(font_.load(TEST_PATH));
  rejectBitmapAtLeast = 300;
  EXPECT_EQ(font_.prewarm("AB", 1, false), 3);
  rejectBitmapAtLeast = 0;
  EXPECT_FALSE(miniDataResident(font_, 0));
  EXPECT_TRUE(font_.hasStyle(0));
  EXPECT_EQ(font_.prewarm("AB", 1, false), 0);
}
TEST_F(SdCardFontReleaseTest, ArenaDoesNotRetryBelowHeadroom) {
  HalStorage::registerTestFile(TEST_PATH, buildMinimalTwoGlyphCpfont(300));
  ASSERT_TRUE(font_.load(TEST_PATH));
  rejectBitmapAtLeast = 900;
  ESP.maxAllocHeap = glyphArena::RESERVE;
  EXPECT_EQ(font_.prewarm("AB", 1, false), 3);
  rejectBitmapAtLeast = 0;
  EXPECT_FALSE(miniDataResident(font_, 0));
}

TEST_F(FontCacheManagerReleaseRoutingTest, SleepOverlayReleaseDropsAdvancesAndKeepsFontsUsable) {
  FontCacheManager fcm(fontMap_, sdCardFonts_);
  const auto hash = fontA_.contentHash();
  auto* epd = fontA_.getEpdFont(0);
  for (auto* font : {&fontA_, &fontB_}) {
    ASSERT_EQ(font->prewarm("AB", 1, false), 0);
    ASSERT_EQ(font->buildAdvanceTable("AB", 1), 0);
  }
  fcm.forceReleaseTransientCaches();
  EXPECT_TRUE(fontA_.hasAdvanceTable());
  EXPECT_TRUE(fontB_.hasAdvanceTable());
  fcm.forceReleaseSleepOverlayCaches();
  fcm.forceReleaseSleepOverlayCaches();  // Safe on repeated candidates / empty caches.
  for (auto* font : {&fontA_, &fontB_}) {
    EXPECT_FALSE(font->hasAdvanceTable());
    EXPECT_EQ(font->getAdvance('A', 0), 0);
    EXPECT_FALSE(miniDataResident(*font, 0));
    // A retained EpdFont is safe immediately, including SD overflow glyph reads.
    ASSERT_NE(font->getEpdFont(0)->getGlyph('B'), nullptr);
    EXPECT_EQ(font->prewarm("AB", 1, false), 0);
    EXPECT_EQ(font->buildAdvanceTable("AB", 1), 0);
    EXPECT_EQ(font->getAdvance('A', 0), 16);
  }
  EXPECT_EQ(fontA_.getEpdFont(0), epd);
  EXPECT_EQ(fontA_.contentHash(), hash);
}

// --- Scan-mode batching: recordText() must batch by RESOLVED font id, so
// body text and status-bar text landing on different fonts within the same
// PrewarmScope both get prewarmed (the original bug stored only the first
// scanFontId_ seen, silently dropping every other font's scanned text). ---

TEST_F(FontCacheManagerReleaseRoutingTest, ScanBatchesMultipleFontIdsInOneScope) {
  FontCacheManager fcm(fontMap_, sdCardFonts_);
  {
    auto scope = fcm.createPrewarmScope();
    EXPECT_TRUE(fcm.isScanning());
    fcm.recordText("AB", /*fontId=*/1, EpdFontFamily::REGULAR);
    fcm.recordText("AB", /*fontId=*/2, EpdFontFamily::REGULAR);
    ASSERT_TRUE(scope.endScanAndPrewarm());
  }
  EXPECT_FALSE(fcm.isScanning());
  EXPECT_TRUE(miniDataResident(fontA_, 0)) << "font id 1's scanned text must be prewarmed";
  EXPECT_TRUE(miniDataResident(fontB_, 0)) << "font id 2's scanned text must be prewarmed too, not dropped";
}

// --- Retained scan-buffer capacity: resetScanSlots() must not let a slot's
// std::string capacity grow unbounded across page turns, and
// forceReleaseTransientCaches()/forceReleaseSleepOverlayCaches() must be able
// to reclaim it on demand for TLS/sleep-overlay heap gates. These assert
// actual retained capacity via FontCacheManagerTestAccess (a test-only friend
// on FontCacheManager) rather than only checking font usability afterward --
// a still-usable font can't distinguish "capacity was capped/released" from
// "capacity was never touched and simply happens not to matter yet."

TEST_F(FontCacheManagerReleaseRoutingTest, ResetScanSlotsCapsCapacityAfterOversizedScan) {
  FontCacheManager fcm(fontMap_, sdCardFonts_);
  const size_t cap = FontCacheManagerTestAccess::scanSlotCapacityCap();
  const std::string oversized(cap * 4, 'A');  // far larger than the cap
  {
    auto scope = fcm.createPrewarmScope();
    fcm.recordText(oversized.c_str(), /*fontId=*/1, EpdFontFamily::REGULAR);
    ASSERT_GT(FontCacheManagerTestAccess::scanSlotCapacity(fcm, 0), cap)
        << "sanity check: recording an oversized run must actually grow capacity past the cap "
           "before the reset -- otherwise this test can't tell capping from a no-op";
    ASSERT_TRUE(scope.endScanAndPrewarm());  // endScanAndPrewarm() calls resetScanSlots()
  }

  EXPECT_LE(FontCacheManagerTestAccess::scanSlotCapacity(fcm, 0), cap)
      << "resetScanSlots() must shrink a slot's capacity back down once it exceeds "
         "kScanSlotCapacityCap, not retain an oversized page's buffer indefinitely";

  // Scan slots must remain fully usable after being capped.
  {
    auto scope = fcm.createPrewarmScope();
    fcm.recordText("A", /*fontId=*/1, EpdFontFamily::REGULAR);
    ASSERT_TRUE(scope.endScanAndPrewarm());
  }
  EXPECT_TRUE(miniDataResident(fontA_, 0));
}

TEST_F(FontCacheManagerReleaseRoutingTest, ForceReleaseTransientCachesReleasesRetainedCapacityBelowCap) {
  FontCacheManager fcm(fontMap_, sdCardFonts_);
  const size_t cap = FontCacheManagerTestAccess::scanSlotCapacityCap();
  // Half the cap: small enough that resetScanSlots()'s own cap-enforcement
  // branch is never taken (capacity <= cap throughout), so any capacity drop
  // observed after forceReleaseTransientCaches() must come from
  // releaseScanSlotCapacity() itself, not from the ordinary end-of-scan reset.
  const std::string withinCap(cap / 2, 'A');
  {
    auto scope = fcm.createPrewarmScope();
    fcm.recordText(withinCap.c_str(), /*fontId=*/1, EpdFontFamily::REGULAR);
    ASSERT_TRUE(scope.endScanAndPrewarm());
  }

  const size_t retained = FontCacheManagerTestAccess::scanSlotCapacity(fcm, 0);
  ASSERT_GE(retained, withinCap.size())
      << "sanity check: an ordinary scan below the cap must retain its capacity across the "
         "reset (this is the existing, intentional baseline behavior) -- otherwise this test "
         "can't tell a real release apart from resetScanSlots() having already cleared it";
  ASSERT_LE(retained, cap);

  fcm.forceReleaseTransientCaches();

  EXPECT_LT(FontCacheManagerTestAccess::scanSlotCapacity(fcm, 0), withinCap.size())
      << "forceReleaseTransientCaches() must actually drop retained scan-slot capacity "
         "(via releaseScanSlotCapacity()), not merely leave the pre-existing cap in place -- "
         "otherwise a TLS-handshake heap gate relying on this call gets no benefit from it";

  // Must remain fully usable afterward.
  {
    auto scope = fcm.createPrewarmScope();
    fcm.recordText("B", /*fontId=*/2, EpdFontFamily::REGULAR);
    ASSERT_TRUE(scope.endScanAndPrewarm());
  }
  EXPECT_TRUE(miniDataResident(fontB_, 0));
}

TEST_F(FontCacheManagerReleaseRoutingTest, ForceReleaseSleepOverlayCachesReleasesRetainedCapacityBelowCap) {
  FontCacheManager fcm(fontMap_, sdCardFonts_);
  const size_t cap = FontCacheManagerTestAccess::scanSlotCapacityCap();
  const std::string withinCap(cap / 2, 'A');
  {
    auto scope = fcm.createPrewarmScope();
    fcm.recordText(withinCap.c_str(), /*fontId=*/1, EpdFontFamily::REGULAR);
    ASSERT_TRUE(scope.endScanAndPrewarm());
  }

  const size_t retained = FontCacheManagerTestAccess::scanSlotCapacity(fcm, 0);
  ASSERT_GE(retained, withinCap.size());

  fcm.forceReleaseSleepOverlayCaches();

  EXPECT_LT(FontCacheManagerTestAccess::scanSlotCapacity(fcm, 0), withinCap.size())
      << "forceReleaseSleepOverlayCaches() must also route through releaseScanSlotCapacity()";

  {
    auto scope = fcm.createPrewarmScope();
    fcm.recordText("A", /*fontId=*/1, EpdFontFamily::REGULAR);
    ASSERT_TRUE(scope.endScanAndPrewarm());
  }
  EXPECT_TRUE(miniDataResident(fontA_, 0));
}

TEST(SleepOverlayPngMemory, ReleaseDecisionChecksTotalAndContiguousHeap) {
  constexpr size_t decoderBytes = 58000;
  EXPECT_FALSE(pngMemory::hasDecoderHeadroom(61439, 61439, decoderBytes));
  EXPECT_FALSE(pngMemory::hasDecoderHeadroom(80000, decoderBytes - 1, decoderBytes));
  EXPECT_TRUE(pngMemory::hasDecoderHeadroom(61440, decoderBytes, decoderBytes));
  EXPECT_TRUE(pngMemory::hasDecoderHeadroom(80000, 70000, decoderBytes));
}

TEST(SdCardFontMeasurement, SaturationStillCachesLaterCjkParagraphs) {
  HalStorage::resetTestFiles();
  ESP.freeHeap = ESP.maxAllocHeap = 0xFFFFFFFFu;
  std::vector<uint32_t> cps;
  std::string first;
  std::string later;
  for (uint32_t cp = 0x4E00; cp < 0x4E00 + 1200; ++cp) {
    cps.push_back(cp);
    utf8AppendCodepoint(cp, cp < 0x4E00 + 800 ? first : later);
  }
  HalStorage::registerTestFile(TEST_PATH, buildCpfont(cps));
  SdCardFont font;
  ASSERT_TRUE(font.load(TEST_PATH));
  ASSERT_EQ(font.buildAdvanceTable(first.c_str(), 1), 0);
  ASSERT_EQ(font.buildAdvanceTable(later.c_str(), 1), 0);
  HalStorage::resetReadCount();
  for (uint32_t cp = 0x4E00 + 800; cp < 0x4E00 + 1200; ++cp) EXPECT_EQ(font.measureAdvance(cp, 0), 16);
  EXPECT_EQ(HalStorage::readCount(), 0u);
  EXPECT_EQ(font.getStats().overflowLoads, 0u);
  // An evicted early character needs one metadata record, never a bitmap/overflow arena.
  EXPECT_EQ(font.measureAdvance(0x4E00 + 700, 0), 16);
  EXPECT_LE(HalStorage::readCount(), 1u);
  EXPECT_EQ(font.getStats().overflowLoads, 0u);
}

TEST(SdCardFontMeasurement, SaturatedCacheKeepsCurrentHitsAlongsideNewGlyphs) {
  for (const uint32_t newCount : {1u, 384u}) {
    SCOPED_TRACE(newCount);
    HalStorage::resetTestFiles();
    ESP.freeHeap = ESP.maxAllocHeap = 0xFFFFFFFFu;
    std::vector<uint32_t> cps;
    std::string first;
    std::string mixed;
    for (uint32_t i = 0; i < 768 + newCount; ++i) {
      const uint32_t cp = 0x4E00 + i;
      cps.push_back(cp);
      if (i < 768) utf8AppendCodepoint(cp, first);
      if (i >= (newCount == 1 ? 767u : 384u)) utf8AppendCodepoint(cp, mixed);
    }
    HalStorage::registerTestFile(TEST_PATH, buildCpfont(cps));
    SdCardFont font;
    ASSERT_TRUE(font.load(TEST_PATH));
    ASSERT_EQ(font.buildAdvanceTable(first.c_str(), 1), 0);
    ASSERT_EQ(font.buildAdvanceTable(mixed.c_str(), 1), 0);
    HalStorage::resetReadCount();
    for (int repeat = 0; repeat < 3; ++repeat) {
      ASSERT_EQ(font.buildAdvanceTable(mixed.c_str(), 1), 0);
      for (uint32_t i = newCount == 1 ? 767u : 384u; i < cps.size(); ++i) {
        EXPECT_EQ(font.measureAdvance(cps[i], 0), 16);
      }
    }
    EXPECT_EQ(HalStorage::readCount(), 0u);
    EXPECT_EQ(font.getStats().overflowLoads, 0u);
  }
}

TEST(SdCardFontMeasurement, ColdMetricFallbackReadsNoBitmapAndRetainsNoMiniArena) {
  HalStorage::resetTestFiles();
  HalStorage::registerTestFile(TEST_PATH, buildMinimalTwoGlyphCpfont());
  SdCardFont font;
  ASSERT_TRUE(font.load(TEST_PATH));
  HalStorage::resetReadCount();
  EXPECT_EQ(font.measureAdvance('A', 0), 16);
  EXPECT_EQ(HalStorage::readCount(), 1u);
  EXPECT_FALSE(miniDataResident(font, 0));
  EXPECT_EQ(font.getStats().overflowLoads, 0u);
}

TEST_F(SdCardFontReleaseTest, ExtendingSmallResidentPageReadsOnlyNewGlyphs) {
  HalStorage::registerTestFile(TEST_PATH, buildCpfont({'A', 'B', 'C'}));
  ASSERT_TRUE(font_.load(TEST_PATH));
  ASSERT_EQ(font_.prewarm("AB", 1, false), 0);
  HalStorage::resetReadCount();
  ASSERT_EQ(font_.prewarm("ABC", 1, false), 0);
  EXPECT_EQ(HalStorage::readCount(), 2u);  // C's metadata and bitmap only
  EXPECT_TRUE(miniDataResident(font_, 0));
  EXPECT_EQ(font_.getEpdFont(0)->getGlyph('A')->advanceX, 16);
  font_.releaseTransientRenderCache();
  EXPECT_FALSE(miniDataResident(font_, 0));
}

// --- Corrupt .cpfont glyph entries (adapted from CrossPoint c1d99e5, #4091) ---
// Every SD glyph-record consumer must treat an entry the file cannot back as an empty glyph:
// no bitmap allocation sized from it, no read of its bytes as pixels, zero advance.

namespace {
const EpdGlyph* miniGlyph(SdCardFont& font, uint32_t cp) {
  const EpdFontData* data = font.getEpdFont(0)->data;
  for (uint32_t i = 0; i < data->intervalCount; ++i) {
    const auto& iv = data->intervals[i];
    if (cp >= iv.first && cp <= iv.last) return data->glyph + iv.offset + cp - iv.first;
  }
  return nullptr;
}

// 'A'..'D' are glyph indices 0..3; index 2 ('C') is corrupted by `corrupt`.
template <typename Corrupt>
std::vector<uint8_t> fontWithCorruptC(Corrupt corrupt) {
  auto file = buildCpfont({'A', 'B', 'C', 'D'}, 300);
  EpdGlyph glyph = readGlyphRecord(file, 2);
  corrupt(glyph, file);
  writeGlyphRecord(file, 2, glyph);
  return file;
}

std::vector<std::vector<uint8_t>> corruptFonts() {
  return {
      fontWithCorruptC([](EpdGlyph& g, auto&) { g.dataLength = 49335; }),      // upstream case: oversized
      fontWithCorruptC([](EpdGlyph& g, auto&) { g.dataLength = 299; }),        // short: renderer over-read
      fontWithCorruptC([](EpdGlyph& g, auto&) { g.width = 255; g.height = 255; }),  // dims disagree
      fontWithCorruptC([](EpdGlyph& g, auto&) { g.dataOffset = 0xFFFFFF00u; }),     // offset overflow
      fontWithCorruptC([](EpdGlyph& g, auto& file) { g.dataOffset = static_cast<uint32_t>(file.size()); }),
  };
}
}  // namespace

TEST(SdCardFontCorruptGlyph, PrewarmDrawsCorruptEntryAsMissingWithoutSizingTheArena) {
  for (const auto& bytes : corruptFonts()) {
    HalStorage::resetTestFiles();
    HalStorage::registerTestFile(TEST_PATH, bytes);
    SdCardFont font;
    ASSERT_TRUE(font.load(TEST_PATH));
    largestNothrowArray = 0;
    ASSERT_EQ(font.prewarm("ABCD", 1, false), 0);
    EXPECT_LT(largestNothrowArray, 4096u);  // never a 49335-byte (or wrapped) bitmap arena
    const EpdGlyph* c = miniGlyph(font, 'C');
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->dataLength, 0);
    EXPECT_EQ(c->width, 0);
    EXPECT_EQ(c->advanceX, 0);
    for (const uint32_t cp : {'A', 'B', 'D'}) {
      const EpdGlyph* ok = miniGlyph(font, cp);
      ASSERT_NE(ok, nullptr);
      EXPECT_EQ(ok->dataLength, 300);
      EXPECT_EQ(ok->advanceX, 16);
    }
  }
}

TEST(SdCardFontCorruptGlyph, AdvanceMeasurementAndBatchTableReportZeroForCorruptEntry) {
  for (const auto& bytes : corruptFonts()) {
    HalStorage::resetTestFiles();
    HalStorage::registerTestFile(TEST_PATH, bytes);
    SdCardFont cold;
    ASSERT_TRUE(cold.load(TEST_PATH));
    EXPECT_EQ(cold.measureAdvance('C', 0), 0);
    EXPECT_EQ(cold.measureAdvance('D', 0), 16);
    SdCardFont batch;
    ASSERT_TRUE(batch.load(TEST_PATH));
    ASSERT_EQ(batch.buildAdvanceTable("ABCD", 1), 0);
    EXPECT_EQ(batch.getAdvance('C', 0), 0);
    EXPECT_EQ(batch.getAdvance('B', 0), 16);
  }
}

TEST(SdCardFontCorruptGlyph, OverflowMissCachesAnEmptyGlyphWithoutAllocatingOrRereading) {
  for (const auto& bytes : corruptFonts()) {
    HalStorage::resetTestFiles();
    HalStorage::registerTestFile(TEST_PATH, bytes);
    SdCardFont font;
    ASSERT_TRUE(font.load(TEST_PATH));
    ASSERT_EQ(font.prewarm("A", 1, false), 0);
    largestNothrowArray = 0;
    HalStorage::resetReadCount();
    const EpdGlyph* c = font.getEpdFont(0)->getGlyph('C');
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->dataLength, 0);
    EXPECT_EQ(c->advanceX, 0);
    EXPECT_EQ(font.getOverflowBitmap(c), nullptr);
    EXPECT_EQ(largestNothrowArray, 0u);
    EXPECT_EQ(HalStorage::readCount(), 1u);  // metadata only; no bitmap read
    HalStorage::resetReadCount();
    EXPECT_EQ(font.getEpdFont(0)->getGlyph('C'), c);  // cached, not re-read
    EXPECT_EQ(HalStorage::readCount(), 0u);
    const EpdGlyph* d = font.getEpdFont(0)->getGlyph('D');
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->dataLength, 300);
    ASSERT_NE(font.getOverflowBitmap(d), nullptr);
  }
}

TEST(SdCardFontCorruptGlyph, TruncatedBitmapSectionLosesOnlyGlyphsPastTheEnd) {
  auto bytes = buildCpfont({'A', 'B', 'C', 'D'}, 300);
  bytes.resize(bytes.size() - 300 - 1);  // REPLACEMENT_GLYPH (last) is gone and D loses its final byte
  HalStorage::resetTestFiles();
  HalStorage::registerTestFile(TEST_PATH, bytes);
  SdCardFont font;
  ASSERT_TRUE(font.load(TEST_PATH));
  ASSERT_EQ(font.prewarm("ABCD", 1, false), 0);
  EXPECT_EQ(miniGlyph(font, 'C')->dataLength, 300);
  EXPECT_EQ(miniGlyph(font, 'D')->dataLength, 0);
  EXPECT_EQ(miniGlyph(font, REPLACEMENT_GLYPH)->dataLength, 0);
}

TEST(SdCardFontCorruptGlyph, ValidFontKeepsItsReadCountsAndBitmaps) {
  HalStorage::resetTestFiles();
  HalStorage::registerTestFile(TEST_PATH, buildCpfont({'A', 'B', 'C', 'D'}, 300));
  SdCardFont font;
  ASSERT_TRUE(font.load(TEST_PATH));
  HalStorage::resetReadCount();
  ASSERT_EQ(font.prewarm("ABCD", 1, false), 0);
  // One contiguous metadata run + one contiguous bitmap run for the five sorted glyphs.
  EXPECT_EQ(HalStorage::readCount(), 10u);
  for (const uint32_t cp : {'A', 'B', 'C', 'D'}) EXPECT_EQ(miniGlyph(font, cp)->dataLength, 300);
}
