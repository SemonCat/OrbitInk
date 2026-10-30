#pragma once
#include <EpdFontFamily.h>
#include <HalDisplay.h>
#include <Utf8.h>

#include <array>
#include <string>
#include <vector>
namespace BidiUtils {
enum class BidiBaseDir : signed char { AUTO = -1, LTR = 0, RTL = 1 };
}
// Deterministic metrics only; parser, CJK tokenization, bidi and line/page layout are production code.
class GfxRenderer {
 public:
  enum Orientation { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };
  Orientation orientation = Portrait;
  RenderMode mode = BW;
  mutable std::array<uint8_t, 792 * 528 / 8> framebuffer{};
  int getDisplayWidth() const { return 792; }
  int getDisplayHeight() const { return 528; }
  int getDisplayWidthBytes() const { return 99; }
  int getScreenWidth() const { return orientation == Portrait || orientation == PortraitInverted ? 528 : 792; }
  int getScreenHeight() const { return orientation == Portrait || orientation == PortraitInverted ? 792 : 528; }
  size_t getBufferSize() const { return framebuffer.size(); }
  uint8_t* getFrameBuffer() const { return framebuffer.data(); }
  uint8_t* getWriteTarget() { return framebuffer.data(); }
  int getWriteOriginY() const { return 0; }
  int getWriteRows() const { return 528; }
  Orientation getOrientation() const { return orientation; }
  RenderMode getRenderMode() const { return mode; }
  void setRenderMode(RenderMode m) { mode = m; }
  void clearScreen(uint8_t value = 255) const { framebuffer.fill(value); }
  void displayGrayscaleBase(HalDisplay::RefreshMode) const {}
  void copyGrayscaleLsbBuffers() const {}
  void copyGrayscaleMsbBuffers() const {}
  void displayGrayBuffer() const {}
  void drawPixel(int x, int y, bool black) const {
    if (x < 0 || y < 0 || x >= getScreenWidth() || y >= getScreenHeight()) return;
    int px = x, py = y;
    if (orientation == Portrait) {
      px = y;
      py = 527 - x;
    } else if (orientation == PortraitInverted) {
      px = 791 - y;
      py = x;
    } else if (orientation == LandscapeClockwise) {
      px = 791 - x;
      py = 527 - y;
    }
    auto& byte = framebuffer[py * 99 + px / 8];
    const auto mask = uint8_t(0x80 >> (px % 8));
    if (black)
      byte &= ~mask;
    else
      byte |= mask;
  }

  int getTextAdvanceX(int, const char* s, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const {
    int n = 0;
    const auto* p = reinterpret_cast<const unsigned char*>(s);
    while (*p) {
      utf8NextCodepoint(&p);
      n += (style & EpdFontFamily::SUP) ? 5 : 10;
    }
    return n;
  }
  template <class... T>
  int getTextWidth(int id, const char* s, EpdFontFamily::Style style, T...) const {
    return getTextAdvanceX(id, s, style);
  }
  template <class... T>
  int getKerning(T...) const {
    return 0;
  }
  template <class... T>
  int getSpaceAdvance(T...) const {
    return 10;
  }
  template <class... T>
  int getSpaceWidth(T...) const {
    return 10;
  }
  int getFontAscenderSize(int) const { return 16; }
  int getLineHeight(int, float compression = 1.0f) const { return int(20 * compression); }
  bool isSdCardFont(int) const { return false; }
  template <class... T>
  void ensureSdCardFontReady(T&&...) const {}
  bool isFontCacheScanning() const { return false; }
  template <class... T>
  void drawLine(T...) const {}
  template <class... T>
  void drawText(T...) const {}
};
