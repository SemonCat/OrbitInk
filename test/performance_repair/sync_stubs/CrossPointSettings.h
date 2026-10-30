#pragma once
#include <Epub/ReaderRenderSpec.h>
struct CrossPointSettings {
  enum { PORTRAIT, LANDSCAPE_CW, INVERTED, LANDSCAPE_CCW };
  uint8_t orientation = PORTRAIT;
  uint8_t screenMargin = 10;
  uint8_t embeddedStyle = 1;
  ReaderRenderSpec readerRenderSpec(uint16_t w, uint16_t h) {
    ReaderRenderSpec spec;
    spec.viewportWidth = w;
    spec.viewportHeight = h;
    return spec;
  }
};
inline CrossPointSettings SETTINGS;
