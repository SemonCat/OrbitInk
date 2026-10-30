#pragma once
// Host stand-in for src/CrossPointState.h. Only the persisted BookOrbit detection fields matter
// here; saveToFile() records that a write happened so a test can assert the value-change guard.
#include <string>

class CrossPointState {
 public:
  static CrossPointState& getInstance() {
    static CrossPointState instance;
    return instance;
  }

  std::string koStatsServerId;
  uint8_t koStatsServerKind = 0;
  bool koStatsCapBookmarkSync = false;
  bool koStatsCapCatalogBulkManifest = false;
  bool koStatsCapCatalogDashboardSections = false;
  std::string koStatsDeviceId;
  bool boMigrationAttempted = false;
  bool boCredentialsMigrated = false;
  std::string boMigrationTargetOwner;

  int saveCount = 0;
  bool saveShouldFail = false;
  bool saveToFile() {
    saveCount++;
    return !saveShouldFail;
  }

  void resetToDefaults() {
    koStatsServerId.clear();
    koStatsServerKind = 0;
    koStatsCapBookmarkSync = false;
    koStatsCapCatalogBulkManifest = false;
    koStatsCapCatalogDashboardSections = false;
    koStatsDeviceId = "a1b2c3d4e5f60718a1b2c3d4e5f60718";  // current persisted installation-id format
    boMigrationAttempted = false;
    boCredentialsMigrated = false;
    boMigrationTargetOwner.clear();
    saveCount = 0;
    saveShouldFail = false;
  }
};

#define APP_STATE CrossPointState::getInstance()
