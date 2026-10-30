#pragma once

#include <string>
#include <vector>

#include "FontInstaller.h"
#include "IdleBaselineLatch.h"
#include "SdCardFont.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// JSON schema version of the fonts.json manifest. The canonical version for
// the build tooling lives in lib/EpdFont/scripts/cpfont_version.py. This
// firmware-side copy must be bumped manually when the firmware is updated to
// support a new manifest schema.
#define FONTS_MANIFEST_VERSION 1

#ifndef FONT_MANIFEST_URL
// Manifest + .cpfont assets are published by .github/workflows/release-fonts.yml
// to the crosspoint-fonts repo under the "sd-fonts-m<META>-b<BIN>" tag. The tag
// pattern must stay in sync with the workflow; it derives its version numbers
// from lib/EpdFont/scripts/cpfont_version.py.
#define FONT_MANIFEST_URL_STRINGIFY_INNER(x) #x
#define FONT_MANIFEST_URL_STRINGIFY(x) FONT_MANIFEST_URL_STRINGIFY_INNER(x)
#define FONT_MANIFEST_URL                                                                                           \
  "https://github.com/crosspoint-reader/crosspoint-fonts/releases/download/sd-fonts-m" FONT_MANIFEST_URL_STRINGIFY( \
      FONTS_MANIFEST_VERSION) "-b" FONT_MANIFEST_URL_STRINGIFY(CPFONT_VERSION) "/fonts.json"
#endif

class FontDownloadActivity : public Activity {
 public:
  explicit FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Only while work is actually running. The result screens (COMPLETE/ERROR) used to be
  // included here because the download is synchronous and blocks the main loop until it
  // completes, so preventAutoSleep() is never polled during it -- the inactivity baseline is
  // stale by the whole download when it ends, and the device would sleep the instant the
  // result appeared. That kept the result screen awake forever instead, never sleeping and
  // never releasing WiFi. loop() now re-bases the inactivity timer once when the download
  // ends (see requestIdleTimerReset), so the result can be read and then sleeps on the normal
  // timeout, which tears WiFi down on the way into deep sleep.
  bool preventAutoSleep() override { return state_ == LOADING_MANIFEST || state_ == DOWNLOADING; }
  // Same scope: full speed while fetching/downloading, normal power-saving loop delay on the
  // selection lists and the result screens (which do nothing but wait for a keypress).
  bool skipLoopDelay() override { return state_ == LOADING_MANIFEST || state_ == DOWNLOADING; }

 private:
  enum State {
    WIFI_SELECTION,
    LOADING_MANIFEST,
    FAMILY_LIST,
    DOWNLOADING,
    COMPLETE,
    ERROR,
  };

  struct ManifestFile {
    std::string name;
    size_t size = 0;
    uint32_t crc32 = 0;
  };

  struct ManifestFamily {
    std::string name;
    std::string description;
    std::vector<std::string> styles;
    std::vector<ManifestFile> files;
    size_t totalSize = 0;
    bool installed = false;
    bool hasUpdate = false;
  };

  State state_ = WIFI_SELECTION;
  FontInstaller fontInstaller_;
  ButtonNavigator buttonNavigator_;

  // Manifest data
  std::string baseUrl_;
  std::vector<ManifestFamily> families_;
  int selectedIndex_ = 0;

  // Download progress
  size_t currentFileIndex_ = 0;
  size_t currentFileTotal_ = 0;
  size_t fileProgress_ = 0;
  size_t fileTotal_ = 0;
  int downloadingFamilyIndex_ = 0;
  std::string errorMessage_;
  bool cancelRequested_ = false;
  // One-shot guard for the inactivity re-base above. Armed where blocking work starts, fired on
  // the first terminal state after it -- NOT inferred from having seen a non-terminal state,
  // which misses a retry launched straight from the ERROR screen. See IdleBaselineLatch.h.
  idlebaseline::Latch idleBaselineLatch_;

  // Runs after every loop() body, on every return path: fires the one-shot inactivity re-base
  // the first time a terminal state is seen after blocking work. Placed here rather than at each
  // of the many `state_ = COMPLETE/ERROR` assignments so no transition can be missed.
  void refreshIdleBaselineOnTerminalState();
  // The real loop() body; loop() wraps it so refreshIdleBaselineOnTerminalState() runs on
  // every return path out of it.
  void loopBody();
  void onWifiSelectionComplete(bool success);
  bool fetchAndParseManifest();
  void downloadFamily(ManifestFamily& family);
  void downloadAll();
  void updateAll();
  static bool computeFileCrc32(const char* path, uint32_t& outCrc);
  bool showDownloadAllRow() const;
  bool showUpdateAllRow() const;
  int specialRowCount() const;
  bool isDownloadAllRow(int index) const;
  bool isUpdateAllRow(int index) const;
  bool isSelectedFamilyDeletable() const;
  void promptDeleteSelectedFamily();
  void onDeleteConfirmationResult(const ActivityResult& result);
  int familyIndexFromList(int listIndex) const { return listIndex - specialRowCount(); }
  int listItemCount() const;
  size_t totalDownloadSize() const;
  size_t totalUpdateSize() const;
  static std::string formatSize(size_t bytes);
};
