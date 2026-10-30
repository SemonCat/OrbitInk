#pragma once
#include <FontCacheManager.h>
class GfxRenderer {
 public:
  enum Orientation { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };
  Orientation orientation = Portrait;
  FontCacheManager cache;
  FontCacheManager* getFontCacheManager() { return &cache; }
  Orientation getOrientation() const { return orientation; }
  void setOrientation(Orientation o) { orientation = o; }
  void getOrientedViewableTRBL(int* t, int* r, int* b, int* l) { *t = *r = *b = *l = 0; }
  int getScreenWidth() { return 528; }
  int getScreenHeight() { return 792; }
};
