#include <GfxRenderer.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

#include <algorithm>

#include "Epub/converters/DirectPixelWriter.h"
#include "Epub/converters/PngScanline.h"
#include "SleepOverlay.h"
namespace {
void u32(std::vector<uint8_t>& b, size_t at, uint32_t x) {
  for (int i = 0; i < 4; ++i) b[at + i] = x >> (8 * i);
}
std::vector<uint8_t> bmp() {
  std::vector<uint8_t> b(62);
  b[0] = 'B';
  b[1] = 'M';
  u32(b, 2, 62);
  u32(b, 10, 54);
  u32(b, 14, 40);
  u32(b, 18, 2);
  u32(b, 22, 1);
  b[26] = 1;
  b[28] = 32;
  // Transparent black, then opaque white.
  b[58] = 255;
  b[59] = 255;
  b[60] = 255;
  b[61] = 255;
  return b;
}
HalFile file(const std::vector<uint8_t>& b) {
  HalStorage::registerTestFile("/overlay.bmp", b);
  return Storage.open("/overlay.bmp");
}
}  // namespace
TEST(SleepOverlay, BmpParserChecksBoundsOrientationAndTruncation) {
  auto b = bmp();
  auto f = file(b);
  SleepOverlay::OverlayBmpInfo info;
  ASSERT_TRUE(SleepOverlay::parseOverlayBmpHeader(f, info));
  EXPECT_EQ(info.width, 2);
  EXPECT_FALSE(info.topDown);
  u32(b, 22, 0xffffffff);
  f = file(b);
  ASSERT_TRUE(SleepOverlay::parseOverlayBmpHeader(f, info));
  EXPECT_TRUE(info.topDown);
  u32(b, 22, 0x80000000);
  f = file(b);
  EXPECT_FALSE(SleepOverlay::parseOverlayBmpHeader(f, info));
  b = bmp();
  b.pop_back();
  f = file(b);
  EXPECT_FALSE(SleepOverlay::parseOverlayBmpHeader(f, info));
  b = bmp();
  u32(b, 10, 10);
  f = file(b);
  EXPECT_FALSE(SleepOverlay::parseOverlayBmpHeader(f, info));
  b = bmp();
  u32(b, 18, UINT32_MAX);
  f = file(b);
  EXPECT_FALSE(SleepOverlay::parseOverlayBmpHeader(f, info));
}
TEST(SleepOverlay, AlphaCompositesOverBaseWithoutChangingTransparentPixels) {
  auto f = file(bmp());
  SleepOverlay::OverlayBmpInfo info;
  ASSERT_TRUE(SleepOverlay::parseOverlayBmpHeader(f, info));
  GfxRenderer renderer;
  renderer.orientation = GfxRenderer::LandscapeCounterClockwise;
  renderer.clearScreen(0);
  uint8_t row[8];
  SleepOverlay::BitmapPlacement placement;
  ASSERT_TRUE(SleepOverlay::renderTransparentOverlayPass(f, info, placement, renderer, row,
                                                         SleepOverlay::TransparentOverlayPass::BW));
  EXPECT_EQ(renderer.framebuffer[0], 0x40);  // only the second, opaque white pixel erases the black base
  EXPECT_EQ(renderer.framebuffer[1], 0);
}
TEST(SleepOverlay, PngRgbaAndIndexedTransparencyPreserveOrFlattenAsRequested) {
  uint8_t pixels[] = {0, 0, 0, 0, 255, 255, 255, 255, 0, 0, 0, 128};
  uint8_t gray[3], alpha[3];
  pngScanline::convertLineToGray(pixels, gray, 3, PNG_PIXEL_TRUECOLOR_ALPHA, 8, nullptr, 1, 0, alpha);
  EXPECT_EQ(alpha[0], 0);
  EXPECT_EQ(alpha[1], 255);
  EXPECT_EQ(alpha[2], 128);
  EXPECT_EQ(gray[0], 0);
  pngScanline::convertLineToGray(pixels, gray, 3, PNG_PIXEL_TRUECOLOR_ALPHA, 8, nullptr, 1, 0, nullptr);
  EXPECT_EQ(gray[0], 255);
  EXPECT_EQ(gray[1], 255);
  EXPECT_EQ(gray[2], 127);
  uint8_t palette[1024] = {};
  palette[768] = 0;
  palette[769] = 255;
  uint8_t indexed[] = {0x40};
  pngScanline::convertLineToGray(indexed, gray, 2, PNG_PIXEL_INDEXED, 1, palette, 1, 0, alpha);
  EXPECT_EQ(alpha[0], 0);
  EXPECT_EQ(alpha[1], 255);
}
TEST(SleepOverlay, PngColorKeyAndSubByteGrayscale) {
  uint8_t pixels[] = {0x1b};
  uint8_t gray[4], alpha[4];
  pngScanline::convertLineToGray(pixels, gray, 4, PNG_PIXEL_GRAYSCALE, 2, nullptr, 1, 2, alpha);
  EXPECT_EQ(gray[0], 0);
  EXPECT_EQ(gray[1], 85);
  EXPECT_EQ(gray[2], 170);
  EXPECT_EQ(gray[3], 255);
  EXPECT_EQ(alpha[2], 0);
  EXPECT_EQ(pngScanline::alphaThreshold4x4(0, 0), 0);
  EXPECT_EQ(pngScanline::alphaThreshold4x4(3, 3), 80);
}
TEST(SleepOverlay, DirectWriterOpaqueWhiteAndAllX3Orientations) {
  for (auto orientation : {GfxRenderer::Portrait, GfxRenderer::PortraitInverted, GfxRenderer::LandscapeClockwise,
                           GfxRenderer::LandscapeCounterClockwise}) {
    GfxRenderer r;
    r.orientation = orientation;
    r.clearScreen(0);
    DirectPixelWriter writer;
    writer.init(r);
    writer.beginRow(0);
    writer.writePixel(0, 3);
    EXPECT_EQ(std::count(r.framebuffer.begin(), r.framebuffer.end(), 0), r.framebuffer.size());
    writer.writePixel(0, 3, true);
    const size_t position = orientation == GfxRenderer::Portrait             ? 527 * 99
                            : orientation == GfxRenderer::PortraitInverted   ? 98
                            : orientation == GfxRenderer::LandscapeClockwise ? 52271
                                                                             : 0;
    const uint8_t mask =
        (orientation == GfxRenderer::Portrait || orientation == GfxRenderer::LandscapeCounterClockwise) ? 0x80 : 1;
    EXPECT_EQ(r.framebuffer[position], mask);
  }
}
TEST(ImageBands, SkippedRowsAndColumnsProduceSamePixelsInEveryOrientation) {
  for (auto orientation : {GfxRenderer::Portrait, GfxRenderer::PortraitInverted, GfxRenderer::LandscapeClockwise,
                           GfxRenderer::LandscapeCounterClockwise}) {
    for (int bandTop : {0, 77, 263, 490}) {
      GfxRenderer renderer;
      renderer.orientation = orientation;
      DirectPixelWriter writer;
      writer.init(renderer);
      writer.originY = bandTop;
      writer.clipRows = std::min(32, 528 - bandTop);
      renderer.clearScreen();
      constexpr int x = 5, y = 13, width = 320, height = 480;
      for (int row = 0; row < height; ++row) {
        writer.beginRow(y + row);
        for (int col = 0; col < width; ++col) writer.writePixel(x + col, (row + col) % 4, true);
      }
      const auto reference = renderer.framebuffer;
      renderer.clearScreen();
      int first, end;
      writer.bandRowRange(y, height, first, end);
      if (orientation == GfxRenderer::LandscapeClockwise || orientation == GfxRenderer::LandscapeCounterClockwise) {
        EXPECT_LE(end - first, writer.clipRows);
      } else {
        EXPECT_EQ(end - first, height);
      }
      for (int row = first; row < end; ++row) {
        writer.beginRow(y + row);
        int firstCol, endCol;
        writer.bandColRange(x, width, firstCol, endCol);
        for (int col = firstCol; col < endCol; ++col) writer.writePixel(x + col, (row + col) % 4, true);
      }
      EXPECT_EQ(renderer.framebuffer, reference);
    }
  }
}
TEST(SleepOverlay, PngHeaderRejectsUnsupportedDepthAndDimensions) {
  std::vector<uint8_t> png = {137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 'I', 'H', 'D',
                              'R', 0,  0,  0,  2,  0,  0,  0,  1, 8, 6, 0,  0,   0};
  auto f = file(png);
  EXPECT_TRUE(SleepOverlay::isValidPngHeader(f));
  png[24] = 16;
  f = file(png);
  EXPECT_FALSE(SleepOverlay::isValidPngHeader(f));
  png[24] = 8;
  png[16] = 0xff;
  f = file(png);
  EXPECT_FALSE(SleepOverlay::isValidPngHeader(f));
  png.resize(10);
  f = file(png);
  EXPECT_FALSE(SleepOverlay::isValidPngHeader(f));
}

TEST(SleepOverlay, FailedDecodeRestoresCleanBaseWithoutConsumingWakeSnapshot) {
  GfxRenderer renderer;
  renderer.clearScreen(255);
  renderer.drawPixel(15, 20, true);
  const auto base = renderer.framebuffer;
  const char* path = "/.crosspoint/sleep_frame.bin";
  HalStorage::registerTestFile(path, {base.begin(), base.end()});
  renderer.drawPixel(40, 50, true);  // a scanline emitted before a decoder failure
  renderer.drawPixel(15, 20, false);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  ASSERT_TRUE(SleepOverlay::restoreBaseFrame(path, renderer));
  EXPECT_EQ(renderer.framebuffer, base);
  EXPECT_EQ(renderer.getRenderMode(), GfxRenderer::BW);
  EXPECT_TRUE(Storage.exists(path));  // wake must still consume the original clean base
  HalStorage::registerTestFile(path, {1, 2, 3});
  EXPECT_FALSE(SleepOverlay::restoreBaseFrame(path, renderer));
  EXPECT_EQ(renderer.framebuffer, base);
}
