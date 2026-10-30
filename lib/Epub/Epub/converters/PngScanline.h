#pragma once
#include <PNGdec.h>

#include <cstdint>
#include <cstring>
namespace pngScanline {
inline uint8_t readPackedSample(const uint8_t* pixels, int x, int bitsPerSample) {
  if (bitsPerSample == 8) return pixels[x];

  const int bitOffset = x * bitsPerSample;
  const int shift = 8 - bitsPerSample - (bitOffset & 7);
  const uint8_t mask = (1U << bitsPerSample) - 1;
  return (pixels[bitOffset >> 3] >> shift) & mask;
}

inline uint8_t expandSampleToByte(uint8_t sample, int bitsPerSample) {
  if (bitsPerSample == 8) return sample;
  const uint8_t maxSample = (1U << bitsPerSample) - 1;
  return static_cast<uint8_t>((sample * 255U) / maxSample);
}

inline uint8_t alphaThreshold4x4(const int x, const int y) {
  static constexpr uint8_t BAYER_4X4[16] = {0, 128, 32, 160, 192, 64, 224, 96, 48, 176, 16, 144, 240, 112, 208, 80};
  return BAYER_4X4[((y & 0x03) << 2) | (x & 0x03)];
}

// Convert an entire source line to grayscale and optionally retain alpha.
// Low-bit-depth grayscale/indexed scanlines are packed most-significant sample first.
// For indexed PNGs with tRNS chunk, alpha values are stored at palette[768] onwards.
// Processing the whole line at once improves cache locality and reduces per-pixel overhead.
inline void convertLineToGray(const uint8_t* pPixels, uint8_t* grayLine, int width, int pixelType, int bitsPerSample,
                              uint8_t* palette, int hasAlpha, uint32_t transparentColor, uint8_t* alphaLine) {
  const auto store = [grayLine, alphaLine](const int x, const uint8_t gray, const uint8_t alpha) {
    grayLine[x] = alphaLine ? gray : static_cast<uint8_t>((gray * alpha + 255u * (255u - alpha)) / 255u);
    if (alphaLine) alphaLine[x] = alpha;
  };

  switch (pixelType) {
    case PNG_PIXEL_GRAYSCALE:
      if (bitsPerSample == 8 && !hasAlpha && !alphaLine) {
        memcpy(grayLine, pPixels, width);
      } else {
        for (int x = 0; x < width; x++) {
          const uint8_t sample = readPackedSample(pPixels, x, bitsPerSample);
          const uint8_t gray = expandSampleToByte(sample, bitsPerSample);
          store(x, gray, hasAlpha && sample == static_cast<uint8_t>(transparentColor) ? 0 : 255);
        }
      }
      break;

    case PNG_PIXEL_TRUECOLOR:
      for (int x = 0; x < width; x++) {
        const uint8_t* p = &pPixels[x * 3];
        const uint8_t gray = static_cast<uint8_t>((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
        const uint32_t color = (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2];
        store(x, gray, hasAlpha && color == transparentColor ? 0 : 255);
      }
      break;

    case PNG_PIXEL_INDEXED:
      if (palette) {
        for (int x = 0; x < width; x++) {
          const uint8_t idx = readPackedSample(pPixels, x, bitsPerSample);
          const uint8_t* p = &palette[idx * 3];
          const uint8_t gray = static_cast<uint8_t>((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
          store(x, gray, hasAlpha ? palette[768 + idx] : 255);
        }
      } else {
        for (int x = 0; x < width; x++) {
          store(x, expandSampleToByte(readPackedSample(pPixels, x, bitsPerSample), bitsPerSample), 255);
        }
      }
      break;

    case PNG_PIXEL_GRAY_ALPHA:
      for (int x = 0; x < width; x++) {
        store(x, pPixels[x * 2], pPixels[x * 2 + 1]);
      }
      break;

    case PNG_PIXEL_TRUECOLOR_ALPHA:
      for (int x = 0; x < width; x++) {
        const uint8_t* p = &pPixels[x * 4];
        const uint8_t gray = static_cast<uint8_t>((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
        store(x, gray, p[3]);
      }
      break;

    default:
      memset(grayLine, 128, width);
      if (alphaLine) memset(alphaLine, 255, width);
      break;
  }
}

}  // namespace pngScanline
