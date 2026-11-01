#pragma once
#include "activities/Activity.h"
#include "util/SleepScreenPolicy.h"

class Bitmap;
class HalFile;

class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const SleepScreenPolicy& policy)
      : Activity("Sleep", renderer, mappedInput), policy(policy) {}
  void onEnter() override;

 private:
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  void renderCoverSleepScreen() const;
  void renderBitmapSleepScreen(const Bitmap& bitmap, bool preserveBackground = false) const;
  bool renderSleepOverlayFile(HalFile& file, const char* path) const;
  bool renderTransparentOverlayPng(const std::string& path) const;
  bool renderSleepOverlayPath(const std::string& path) const;
  bool renderTransparentCustomSleepScreen() const;
  void renderLastScreenSleepScreen() const;
  void renderBlankSleepScreen() const;

  const SleepScreenPolicy policy;
};
