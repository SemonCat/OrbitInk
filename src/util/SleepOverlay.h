#pragma once
#include <cstdint>
class HalFile;
class GfxRenderer;
namespace SleepOverlay {
struct BitmapPlacement {
  int x = 0, y = 0;
  float cropX = 0, cropY = 0;
};
struct OverlayBmpInfo {
  int width = 0, height = 0;
  bool topDown = false;
  uint32_t dataOffset = 0, rowBytes = 0;
};
enum class TransparentOverlayPass : uint8_t { BW, GrayscaleLsb, GrayscaleMsb };
enum class AlphaOverlayResult : uint8_t { Rendered, NotAlphaOverlay, Error };
bool restoreBaseFrame(const char* path, GfxRenderer& renderer);
bool isValidPngHeader(HalFile& file);
bool parseOverlayBmpHeader(HalFile& file, OverlayBmpInfo& info, bool logErrors = false);
BitmapPlacement calculateBitmapPlacement(int width, int height, const GfxRenderer& renderer);
uint8_t bayerThreshold4x4(int x, int y);
bool renderTransparentOverlayPass(HalFile& file, const OverlayBmpInfo& info, const BitmapPlacement& placement,
                                  const GfxRenderer& renderer, uint8_t* row, TransparentOverlayPass pass);
AlphaOverlayResult tryRenderTransparentOverlayBmp(HalFile& file, GfxRenderer& renderer, const char* pathForLog);
}  // namespace SleepOverlay
