// Exercises the REAL lib/Epub/Epub/converters/ImageToFramebufferDecoder.cpp's
// validateImageDimensions() -- the shared 3-megapixel gate both JpegToFramebufferConverter and
// PngToFramebufferConverter call with dimensions read straight from the decoder library's own
// header parse (PNGdec/JPEGDEC getWidth()/getHeight(), plain `int`, not range-checked against
// int16_t the way ImageDimsProbe's own parse is). The bug this covers: `width * height` computed
// as 32-bit `int` overflows (UB, and can wrap to a small/negative value) for large dimensions
// before ever reaching the MAX_SOURCE_PIXELS compare.

#include <gtest/gtest.h>

#include <climits>
#include <string>

#include "Epub/converters/ImageToFramebufferDecoder.h"

namespace {

// validateImageDimensions()/MAX_SOURCE_PIXELS are `protected`; this test-only subclass exposes
// them without changing production visibility. The pure virtuals are never called by these tests.
class TestableDecoder final : public ImageToFramebufferDecoder {
 public:
  bool decodeToFramebuffer(const std::string&, GfxRenderer&, const RenderConfig&) override { return false; }
  bool getDimensions(const std::string&, ImageDimensions&) const override { return false; }
  const char* getFormatName() const override { return "TEST"; }

  bool validate(int width, int height) { return validateImageDimensions(width, height, "TEST"); }
  static constexpr int maxPixels() { return MAX_SOURCE_PIXELS; }
};

}  // namespace

TEST(ImageDimsValidationTest, RetainsTheLocal3MegapixelLimit) {
  // The task this test backs is explicit that this limit must NOT be raised (e.g. to 8MP).
  EXPECT_EQ(TestableDecoder::maxPixels(), 3145728) << "3MP limit (2048*1536) must stay as-is";
}

TEST(ImageDimsValidationTest, AcceptsDimensionsAtOrUnderTheLimit) {
  TestableDecoder d;
  EXPECT_TRUE(d.validate(2048, 1536));  // exactly at the limit
  EXPECT_TRUE(d.validate(800, 480));    // typical page-fit size
  EXPECT_TRUE(d.validate(1, 1));
}

TEST(ImageDimsValidationTest, RejectsDimensionsOverTheLimitWithoutOverflow) {
  TestableDecoder d;
  EXPECT_FALSE(d.validate(2049, 1536));  // one pixel column over
  EXPECT_FALSE(d.validate(4000, 3000));  // 12MP, comfortably over
}

TEST(ImageDimsValidationTest, RejectsNonPositiveDimensions) {
  TestableDecoder d;
  EXPECT_FALSE(d.validate(0, 100));
  EXPECT_FALSE(d.validate(100, 0));
  EXPECT_FALSE(d.validate(-1, 100)) << "a negative width (e.g. from a corrupt IHDR/SOF field "
                                       "wrapping through int32) must be rejected outright";
  EXPECT_FALSE(d.validate(100, -1));
}

TEST(ImageDimsValidationTest, DoesNotOverflowOnDimensionsWhoseProductExceeds32BitInt) {
  // INT_MAX is ~2.147e9. 50000 * 50000 = 2.5e9, which overflows a 32-bit signed int (UB) if the
  // multiplication is done in `int` -- and on this compiler/platform historically wrapped to a
  // value that passed the old `width * height > MAX_SOURCE_PIXELS` check, silently accepting a
  // 2.5-billion-pixel "image." The fixed 64-bit comparison must reject it outright instead.
  TestableDecoder d;
  EXPECT_FALSE(d.validate(50000, 50000));
  EXPECT_FALSE(d.validate(INT_MAX, 2)) << "must not overflow even for an extreme, clearly-corrupt header value";
}
