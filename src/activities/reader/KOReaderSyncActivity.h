#pragma once
#include <Epub.h>

#include <functional>
#include <memory>
#include <optional>

#include "KOReaderSyncClient.h"
#include "ProgressMapper.h"
#include "activities/Activity.h"

/**
 * Activity for syncing reading progress with KOReader sync server.
 *
 * Flow:
 * 1. Connect to WiFi (if not connected)
 * 2. Calculate document hash
 * 3. Fetch remote progress
 * 4. Show comparison and options (Apply/Upload)
 * 5. Apply or upload progress
 */
class KOReaderSyncActivity final : public Activity {
 public:
  explicit KOReaderSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& epubPath,
                                int currentSpineIndex, int currentPage, int totalPagesInSpine,
                                SavedProgressPosition localKoPos, uint64_t localEventTimestamp, bool localProgressLegacy,
                                std::string localChapterName,
                                std::optional<uint16_t> currentParagraphIndex = std::nullopt)
      : Activity("KOReaderSync", renderer, mappedInput),
        epubPath(epubPath),
        currentSpineIndex(currentSpineIndex),
        currentPage(currentPage),
        totalPagesInSpine(totalPagesInSpine),
        localEventTimestamp(localEventTimestamp),
        localProgressLegacy(localProgressLegacy),
        currentParagraphIndex(currentParagraphIndex),
        localChapterName(std::move(localChapterName)),
        remoteProgress{},
        remotePosition{},
        localProgress(std::move(localKoPos)) {}

  // Probe BookOrbitCredentialStore's configured server and drain this book's queued reading
  // events, on BookOrbit's own session (independent of this activity's KOSync `syncSession`).
  // Soft in every direction: it cannot change the progress result or the UI, and it is a no-op
  // when no BookOrbit credentials are configured.
  void drainBookOrbitStats();

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == SYNCING || state == UPLOADING; }

 private:
  enum State {
    WIFI_SELECTION,
    CONNECTING,
    SYNCING,
    SHOWING_RESULT,
    UPLOADING,
    UPLOAD_COMPLETE,
    SYNC_COMPLETE,
    NO_REMOTE_PROGRESS,
    SYNC_FAILED,
    NO_CREDENTIALS
  };

  std::shared_ptr<Epub> epub;  // null until lazy-loaded after TLS in performSync()
  std::string epubPath;
  // Stats identity for this book: KOReader's binary partial MD5, derived independently of the
  // progress match method and of smart sync's alternate-hash probing, both of which can point
  // `documentHash` somewhere BookOrbit could never resolve a page-stats upload. Also the key of
  // the stats queue itself, so no separate cache path has to be carried in from the reader.
  // Derived lazily in drainBookOrbitStats(), and only when a drain could achieve anything.
  std::string bookOrbitStatsHash;
  // One stats drain per sync, whichever success path gets there first.
  bool bookOrbitStatsDrained = false;
  std::string localChapterName;
  int currentSpineIndex;
  int currentPage;
  int totalPagesInSpine;
  uint64_t localEventTimestamp = 0;
  bool localProgressLegacy = false;
  std::optional<uint16_t> currentParagraphIndex;

  State state = WIFI_SELECTION;
  std::string statusMessage;
  std::string documentHash;

  // One session for this whole manual-sync attempt: performSync()'s primary GET, its optional
  // smart-sync alternate-hash GET, and performUpload()'s PUT (called either from within
  // performSync() or later, from a user tap/button after SHOWING_RESULT/NO_REMOTE_PROGRESS is
  // displayed) all reuse this same underlying connection when possible, instead of paying a
  // fresh TLS handshake per request. Lives for the activity's lifetime (destroyed, closing any
  // open connection, when the activity itself is destroyed on exit) -- see
  // KOReaderSyncClient.h's Session doc comment for the reuse contract this relies on.
  KOReaderSyncClient::Session syncSession;

  // Remote progress data
  bool hasRemoteProgress = false;
  KOReaderProgress remoteProgress;
  CrossPointPosition remotePosition;

  // Local progress as KOReader format (pre-computed before Epub was released)
  SavedProgressPosition localProgress;

  // Selection in result screen (0=Apply, 1=Upload)
  int selectedOption = 0;

  // Timed return for successful smart-sync terminal states.
  unsigned long autoReturnAt = 0;
  static constexpr unsigned long AUTO_RETURN_DELAY_MS = 1200;

  // Tracks whether this session activated WiFi. Set in onEnter past the credentials
  // check; checked in onExit to decide whether to silent-reboot. Can't rely on
  // WiFi.getMode() because performUpload() calls esp_wifi_stop() on the way out,
  // which makes WiFi.getMode() return WIFI_MODE_NULL.
  bool wifiActivated = false;

  void onWifiSelectionComplete(bool success);
  void performSync();
  void performUpload();
  bool smartSyncEnabled() const;
  void markAutoReturn();
  void completeAlreadySynced();
  void ensureEpubLoaded();
  void saveProgressAndReturn(int spineIndex, int page, uint64_t timestamp);
  void returnToReader();
};
