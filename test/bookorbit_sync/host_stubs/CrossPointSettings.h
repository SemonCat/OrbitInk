#pragma once
// Host stand-in for src/CrossPointSettings.h, reduced to the two accessors
// src/util/BookOrbitStatsSync.cpp actually reads.
#include <string>

class CrossPointSettings {
 public:
  static CrossPointSettings& getInstance() {
    static CrossPointSettings instance;
    return instance;
  }
  bool shouldTrackReadingStats() const { return trackReadingStats; }
  std::string getEffectiveDeviceName() const { return deviceName; }

  // Test-only.
  bool trackReadingStats = true;
  std::string deviceName = "Xteink X3";
  void resetToDefaults() {
    trackReadingStats = true;
    deviceName = "Xteink X3";
  }
};

#define SETTINGS CrossPointSettings::getInstance()
