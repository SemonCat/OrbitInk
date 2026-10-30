#include "ImageToFramebufferDecoder.h"

#include <Logging.h>

#include <cstdint>

bool ImageToFramebufferDecoder::validateImageDimensions(int width, int height, const std::string& format) {
  // width/height come straight from the decoder library's own header parse (e.g. PNG IHDR's 4-byte
  // fields via PNGdec's getWidth()/getHeight(), returned as plain `int`), not through
  // ImageDimsProbe's own int16_t-range check -- a corrupt or crafted header can hand this a
  // negative value (32-bit wraparound) or one large enough that `width * height` as 32-bit `int`
  // overflows before ever reaching the MAX_SOURCE_PIXELS compare below (UB, and on this compiler
  // can wrap to a small/negative value that passes the check).
  if (width <= 0 || height <= 0) {
    LOG_ERR("IMG", "Image has invalid dimensions (%dx%d %s)", width, height, format.c_str());
    return false;
  }
  const int64_t pixels = static_cast<int64_t>(width) * static_cast<int64_t>(height);
  if (pixels > MAX_SOURCE_PIXELS) {
    LOG_ERR("IMG", "Image too large (%dx%d %s), max supported: %d pixels", width, height, format.c_str(),
            MAX_SOURCE_PIXELS);
    return false;
  }
  return true;
}

void ImageToFramebufferDecoder::warnUnsupportedFeature(const std::string& feature, const std::string& imagePath) {
  LOG_ERR("IMG", "Warning: Unsupported feature '%s' in image '%s'. Image may not display correctly.", feature.c_str(),
          imagePath.c_str());
}

bool ImageToFramebufferDecoder::clampToImageDimensions(int width, int height, const char* formatTag,
                                                        ImageDimensions& out) {
  if (width <= 0 || height <= 0 || width > INT16_MAX || height > INT16_MAX) {
    LOG_ERR("IMG", "Invalid or out-of-range %s dimensions: %dx%d", formatTag, width, height);
    return false;
  }
  out.width = static_cast<int16_t>(width);
  out.height = static_cast<int16_t>(height);
  return true;
}
