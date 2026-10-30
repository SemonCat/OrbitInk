#pragma once
// Host-test stand-in for lib/EpdFont/FontDecompressor.h. This test never sets
// a decompressor on FontCacheManager (fontDecompressor_ stays nullptr, so
// FontCacheManager's `if (fontDecompressor_) ...` guards never actually call
// into this type at runtime) -- but FontCacheManager.cpp still needs a
// complete, source-compatible declaration to compile and link against, and
// the real FontDecompressor pulls in uzlib + an explicit Arduino.h include
// that this narrow font-cache-release test has no need for.

#include "EpdFontData.h"

class FontDecompressor {
 public:
  void clearCache() {}
  int prewarmCache(const EpdFontData* /*fontData*/, const char* /*utf8Text*/) { return 0; }
  void logStats(const char* /*label*/ = "FDC") {}
  void resetStats() {}
};
