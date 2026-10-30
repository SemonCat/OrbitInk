#include "KOReaderSyncActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <esp_wifi.h>

#include <algorithm>
#include <cassert>
#include <cmath>

#include "Epub/Section.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "ProgressPullPolicy.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookOrbitStatsSync.h"
#include "util/KOReaderAutoSync.h"
#include "util/KOSyncWifiPowerSaveGuard.h"
#include "util/ProgressSyncRetry.h"

namespace {
std::string calculateDocumentHashForMethod(const std::string& path, const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? KOReaderDocumentId::calculateFromFilename(path)
                                                 : KOReaderDocumentId::calculate(path);
}

DocumentMatchMethod alternateMatchMethod(const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? DocumentMatchMethod::BINARY : DocumentMatchMethod::FILENAME;
}

const char* matchMethodName(const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? "filename" : "binary";
}

// RTC-equipped devices (X3's persistent DS3231) get their one-per-device-lifetime NTP sync from
// WifiSelectionActivity's own successful-connect path (see its halClock.syncFromNTP() call) --
// repeating that here on every KOSync attempt was pure duplication. A device without a persistent
// RTC has no other NTP sync path anywhere in the firmware, though (its system clock is volatile
// across sleep/reboot, so WifiSelectionActivity's own sync there wouldn't even help next boot):
// this keeps only that fallback, gated so RTC-equipped devices return immediately.
void syncTimeWithNtpFallbackForRtcLessDevices() {
  if (halClock.isAvailable()) return;

  // Stop SNTP if already running (can't reconfigure while running)
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }

  // Configure SNTP
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_init();

  // Wait for time to sync (with timeout)
  int retry = 0;
  const int maxRetries = 50;  // 5 seconds max
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && retry < maxRetries) {
    vTaskDelay(100 / portTICK_PERIOD_MS);
    retry++;
  }

  if (retry < maxRetries) {
    LOG_DBG("KOSync", "NTP time synced");
  } else {
    LOG_DBG("KOSync", "NTP sync timeout, using fallback");
  }
}

}  // namespace

void KOReaderSyncActivity::ensureEpubLoaded() {
  if (!epub) {
    // Force-release retained SD-font mini-bitmap/kern/overflow data before this memory-heavy
    // reload -- see FontCacheManager::forceReleaseTransientCaches() and the auto-sync path's
    // matching call in KOReaderAutoSync.cpp for the same rationale.
    if (auto* fcm = renderer.getFontCacheManager()) fcm->forceReleaseTransientCaches();
    LOG_DBG("KOSync", "Loading epub for progress mapping (heap: %u)", (unsigned)ESP.getFreeHeap());
    epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
    epub->setupCacheDir();
    // Load metadata only (no CSS needed for progress mapping, don't rebuild if cache is missing).
    if (!epub->load(false, true)) {
      LOG_ERR("KOSync", "Failed to load epub for progress mapping");
      epub.reset();
      return;
    }
    LOG_DBG("KOSync", "Epub loaded (heap: %u)", (unsigned)ESP.getFreeHeap());
  }
}

void KOReaderSyncActivity::saveProgressAndReturn(int spineIndex, int page, uint64_t timestamp) {
  // epub is guaranteed non-null here: ensureEpubLoaded() was called in performSync() before
  // SHOWING_RESULT state is entered, and this method is only called from that state.
  assert(epub);
  if (!EpubReaderUtils::saveProgress(*epub, spineIndex, page, 0, timestamp)) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SAVE_PROGRESS_FAILED);
    }
    requestUpdate(true);
    return;
  }
  KOReaderAutoSync::recordManualSuccess();
  returnToReader();
}

void KOReaderSyncActivity::returnToReader() { activityManager.goToReader(epubPath); }

bool KOReaderSyncActivity::smartSyncEnabled() const {
  return KOREADER_STORE.getSyncBehavior() == KOReaderSyncBehavior::SMART;
}

void KOReaderSyncActivity::markAutoReturn() { autoReturnAt = millis() + AUTO_RETURN_DELAY_MS; }

void KOReaderSyncActivity::completeAlreadySynced() {
  KOReaderAutoSync::recordManualSuccess();
  {
    RenderLock lock(*this);
    state = SYNC_COMPLETE;
  }
  markAutoReturn();
  requestUpdate(true);
}

void KOReaderSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_DBG("KOSync", "WiFi connection failed, exiting");
    returnToReader();
    return;
  }

  LOG_DBG("KOSync", "WiFi connected, starting sync");

  {
    RenderLock lock(*this);
    state = SYNCING;
    statusMessage = tr(STR_SYNCING_TIME);
  }
  requestUpdate(true);

  // RTC-equipped devices already get their one NTP sync from WifiSelectionActivity's own
  // successful-connect path (see its halClock.syncFromNTP() call) -- repeating it here on every
  // KOSync attempt was the redundant part. Devices without a persistent RTC have no other NTP
  // sync path anywhere in the firmware (the system clock is volatile across sleep/reboot), so
  // this keeps only that fallback; it returns immediately when halClock IS available.
  syncTimeWithNtpFallbackForRtcLessDevices();

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_CALC_HASH);
  }
  requestUpdate(true);

  performSync();
}

void KOReaderSyncActivity::drainBookOrbitStats() {
  if (bookOrbitStatsDrained) return;
  bookOrbitStatsDrained = true;

  // Gated before the identity is derived: calculateForStats() reads chunks from the book file,
  // which is not worth doing for a server that will never accept stats.
  if (!BookOrbitStats::drainPossible()) return;
  if (bookOrbitStatsHash.empty()) bookOrbitStatsHash = KOReaderDocumentId::calculateForStats(epubPath);
  if (bookOrbitStatsHash.empty()) return;

  // Opens its own BookOrbitSyncClient::Session rather than reusing `syncSession` (the KOSync
  // session): BookOrbit is now an explicit, standalone provider with its own credential store and
  // commonly a different host, so there is nothing to reuse. Running this after a manual KOSync
  // sync -- rather than only from BookOrbit's own trigger -- is a scheduling convenience (Wi-Fi
  // already happens to be up here); see docs/bookorbit-feature-matrix.md's Phase 3 plan.
  //
  // Manual sync matters here beyond convenience: a user who only ever syncs by hand would
  // otherwise never run the capability probe, so a freshly configured BookOrbit server would sit
  // unconfirmed forever, capture would stay off, and nothing would ever be queued or drained.
  //
  // The identity is deliberately NOT `documentHash`. That one follows the user's progress match
  // method and may additionally have been repointed at the alternate-method hash by smart sync
  // after finding further remote progress there. Neither is a stats identity: BookOrbit resolves
  // a page-stats hash only against an intrinsic binary partial MD5, a hash-history entry, or an
  // already-existing manual link, and its page-stats endpoint never records an unmatched hash
  // anywhere a user could link it from.
  if (auto* fcm = renderer.getFontCacheManager()) fcm->forceReleaseTransientCaches();
  const KOSyncWifiPowerSaveGuard wifiPowerSaveGuard;
  BookOrbitStats::uploadQueued(bookOrbitStatsHash);
}

void KOReaderSyncActivity::performSync() {
  const DocumentMatchMethod primaryMethod = KOREADER_STORE.getMatchMethod();
  documentHash = calculateDocumentHashForMethod(epubPath, primaryMethod);
  if (documentHash.empty()) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_HASH_FAILED);
    }
    requestUpdate(true);
    return;
  }
  const std::string primaryHash = documentHash;

  LOG_DBG("KOSync", "Document hash (%s): %s", matchMethodName(primaryMethod), documentHash.c_str());

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_FETCH_PROGRESS);
  }
  requestUpdateAndWait();

  // Force-release retained SD-font mini-bitmap/kern/overflow data before the TLS request below:
  // requestUpdateAndWait() just rendered the status text above, which may itself have allocated a
  // small transient UI/glyph cache; this clears both that and any page-turn-retained mini bitmap.
  if (auto* fcm = renderer.getFontCacheManager()) fcm->forceReleaseTransientCaches();

  // Fetch remote progress. In smart mode, also probe the alternate document-id
  // method and use the latest remote state we can find. This avoids a stale
  // local upload when another KOReader device synced the same book with a
  // different document matching method.
  KOReaderSyncClient::Error result;
  {
    // Keep modem sleep disabled for just these GET(s): their latency has been observed to make
    // the wolfSSL TLS handshake fail with http=-1. Scoped tightly around the requests themselves,
    // not the SHOWING_RESULT/NO_REMOTE_PROGRESS indefinite user-choice wait that can follow this
    // function -- see performUpload()'s own matching guard for its PUT.
    const KOSyncWifiPowerSaveGuard wifiPowerSaveGuard;
    result = syncSession.getProgress(documentHash, remoteProgress, nullptr, "primary");
    LOG_DBG("KOSync", "Primary remote (%s): result=%d http=%d doc=%s local=%.6f remote=%.6f xpath=%s",
            matchMethodName(primaryMethod), result, syncSession.lastHttpCode(), documentHash.c_str(),
            localProgress.percentage, remoteProgress.percentage, remoteProgress.progress.c_str());

    if (smartSyncEnabled()) {
      const DocumentMatchMethod altMethod = alternateMatchMethod(primaryMethod);
      const std::string altHash = calculateDocumentHashForMethod(epubPath, altMethod);
      if (!altHash.empty() && altHash != documentHash) {
        KOReaderProgress altProgress;
        const auto altResult = syncSession.getProgress(altHash, altProgress, nullptr, "alternate");
        LOG_DBG("KOSync", "Alternate remote (%s): result=%d http=%d doc=%s local=%.6f remote=%.6f xpath=%s",
                matchMethodName(altMethod), altResult, syncSession.lastHttpCode(), altHash.c_str(),
                localProgress.percentage, altProgress.percentage, altProgress.progress.c_str());

        if (altResult == KOReaderSyncClient::OK &&
            (result == KOReaderSyncClient::NOT_FOUND || altProgress.timestamp > remoteProgress.timestamp)) {
          documentHash = altHash;
          remoteProgress = std::move(altProgress);
          result = KOReaderSyncClient::OK;
        }
      }
    }
  }

  if (result == KOReaderSyncClient::NOT_FOUND) {
    if (smartSyncEnabled() && localProgressLegacy) {
      {
        RenderLock lock(*this);
        state = SYNC_FAILED;
        statusMessage = "No valid server progress yet; local position was preserved";
      }
      requestUpdate(true);
      return;
    }
    if (smartSyncEnabled() && ProgressTimestamp::valid(localEventTimestamp)) {
      LOG_DBG("KOSync", "Smart sync: no remote progress found; uploading known-time local event");
      performUpload();
      return;
    }

    // No remote progress - offer to upload. The GET itself succeeded (the server answered, it
    // simply holds nothing for this document), and this path may sit on a user prompt
    // indefinitely or be cancelled outright, so drain here rather than hoping for a later PUT.
    drainBookOrbitStats();

    {
      RenderLock lock(*this);
      state = NO_REMOTE_PROGRESS;
      hasRemoteProgress = false;
    }
    requestUpdate(true);
    return;
  }

  if (result != KOReaderSyncClient::OK) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = KOReaderSyncClient::errorString(result);
    }
    requestUpdate(true);
    return;
  }

  // Drain queued stats before the Epub reload below: this is a confirmed-successful sync
  // transaction, the radio is still up, and doing it here means the memory-heavy reload has not
  // yet eaten the heap the TLS request needs. The apply/ignore decision that follows is a user
  // prompt that may never resolve, so waiting for it would strand the queue.
  drainBookOrbitStats();

  // Epub was released before sync to free RAM for the TLS handshake — reload it now.
  hasRemoteProgress = true;
  ensureEpubLoaded();
  if (!epub) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = "";
    }
    requestUpdate(true);
    return;
  }

  // Prefer the exact spine/page supplied by the CrossPoint sync server; fall
  // back to the approximate XPath mapping when it cannot be applied.
  std::optional<CrossPointPosition> richMapped;
  if (remoteProgress.position.has_value()) {
    richMapped = ProgressMapper::fromRichPosition(epub, *remoteProgress.position, renderer);
  }
  if (richMapped.has_value()) {
    remotePosition = *richMapped;
  } else {
    SavedProgressPosition koPos = {remoteProgress.progress, remoteProgress.percentage};
    remotePosition = ProgressMapper::toCrossPoint(epub, koPos, renderer, currentSpineIndex, totalPagesInSpine);
  }

  if (smartSyncEnabled()) {
    if (localProgressLegacy) {
      ProgressSyncRetry::confirm(ProgressSyncRetry::Provider::KOSync, epubPath);
      saveProgressAndReturn(remotePosition.spineIndex, remotePosition.pageNumber,
                            static_cast<uint64_t>(remoteProgress.timestamp));
      return;
    }
    const bool samePosition = localProgress.xpath == remoteProgress.progress ||
                              (remotePosition.spineIndex == currentSpineIndex &&
                               remotePosition.pageNumber == currentPage);
    const auto action = ProgressPullPolicy::decide(
        {true, localEventTimestamp, ProgressPullPolicy::RemoteState::Found,
         static_cast<uint64_t>(remoteProgress.timestamp), samePosition});
    LOG_DBG("KOSync", "Smart decision: doc=%s local=%.6f@%llu remote=%.6f@%lld action=%u remoteXpath=%s mapped=%d/%d",
            documentHash.c_str(), localProgress.percentage, static_cast<unsigned long long>(localEventTimestamp),
            remoteProgress.percentage, static_cast<long long>(remoteProgress.timestamp), static_cast<unsigned>(action),
            remoteProgress.progress.c_str(), remotePosition.spineIndex, remotePosition.pageNumber);
    if (action == ProgressPullPolicy::SyncAction::AlreadySynced) {
      ProgressSyncRetry::confirm(ProgressSyncRetry::Provider::KOSync, epubPath);
      completeAlreadySynced();
      return;
    }
    if (action == ProgressPullPolicy::SyncAction::PushLocal) {
      // Alternate hashes are only probes for newer remote state. Keep uploads
      // on the user's configured matching method so its primary record heals.
      documentHash = primaryHash;
      performUpload();
      return;
    }
    if (action == ProgressPullPolicy::SyncAction::PullRemote) {
      ProgressSyncRetry::confirm(ProgressSyncRetry::Provider::KOSync, epubPath);
      saveProgressAndReturn(remotePosition.spineIndex, remotePosition.pageNumber,
                            static_cast<uint64_t>(remoteProgress.timestamp));
      return;
    }
    if (action == ProgressPullPolicy::SyncAction::HoldUnknown ||
        action == ProgressPullPolicy::SyncAction::Abort) {
      {
        RenderLock lock(*this);
        state = SYNC_FAILED;
        statusMessage = "Progress timestamp unavailable; local position was preserved";
      }
      requestUpdate(true);
      return;
    }
  }

  // localProgress was pre-computed in EpubReaderActivity before the Epub was released.
  {
    RenderLock lock(*this);
    state = SHOWING_RESULT;

    // Default to the option selected by known chronology; ambiguous states default to remote but
    // still require the user's explicit confirmation.
    if (ProgressTimestamp::valid(localEventTimestamp) && localEventTimestamp > remoteProgress.timestamp) {
      selectedOption = 1;  // Upload local progress
    } else {
      selectedOption = 0;  // Apply remote progress
    }
  }
  requestUpdate(true);
}

void KOReaderSyncActivity::performUpload() {
  {
    RenderLock lock(*this);
    state = UPLOADING;
    statusMessage = tr(STR_UPLOAD_PROGRESS);
  }
  requestUpdateAndWait();

  // localProgress was pre-computed in EpubReaderActivity before the Epub was released.
  KOReaderProgress progress;
  progress.document = documentHash;
  progress.progress = localProgress.xpath;
  progress.percentage = localProgress.percentage;
  progress.device = SETTINGS.getEffectiveDeviceName();

  // Rich CrossPoint position for the default CrossPoint sync server (lossless
  // CrossPoint<->CrossPoint sync). The HTTP client also enforces this boundary
  // before serializing the extension.
  if (KOREADER_STORE.usesCrossPointSyncServer()) {
    KOReaderRichPosition pos;
    const float pct = localProgress.percentage < 0.0f   ? 0.0f
                      : localProgress.percentage > 1.0f ? 1.0f
                                                        : localProgress.percentage;
    pos.pctQ = static_cast<uint32_t>(pct * 1000000.0f + 0.5f);
    pos.spineIndex = static_cast<uint16_t>(currentSpineIndex);
    pos.pageNumber = static_cast<uint16_t>(currentPage);
    pos.totalPages = static_cast<uint16_t>(totalPagesInSpine > 0 ? totalPagesInSpine : 1);
    pos.paragraphIndex = currentParagraphIndex;
    pos.xpath = localProgress.xpath;
    progress.position = std::move(pos);
  }

  // Optionally include document metadata (KOReader PR #15306)
  if (KOREADER_STORE.getSendMetadata()) {
    // The Epub is released before the sync network calls and is only reloaded on the
    // remote-progress path (performSync). When uploading from NO_REMOTE_PROGRESS the
    // Epub is still null, so reload it here and guard the title/author reads to avoid
    // dereferencing a null Epub. Filename is derived from the path and is always safe.
    ensureEpubLoaded();
    KOReaderMetadata meta;
    const auto lastSlash = epubPath.rfind('/');
    meta.filename = (lastSlash != std::string::npos) ? epubPath.substr(lastSlash + 1) : epubPath;
    if (epub) {
      meta.title = epub->getTitle();
      meta.authors = epub->getAuthor();
    } else {
      LOG_ERR("KOSync", "Epub unavailable for metadata; sending filename only");
    }
    progress.metadata = std::move(meta);
  }

  // Release the Epub before the network call so the TLS handshake has enough free heap
  // (consistent with the release-before-sync pattern in performSync); nothing below needs it.
  epub.reset();

  // Force-release retained SD-font mini-bitmap/kern/overflow data before this TLS request too --
  // same rationale as performSync()'s call above; the UPLOADING status render just above (and any
  // ensureEpubLoaded() metadata reload above it) may have rebuilt a page's worth of mini data.
  if (auto* fcm = renderer.getFontCacheManager()) fcm->forceReleaseTransientCaches();

  // Same session as performSync()'s GET(s) above -- reuses the connection when this PUT follows
  // directly (smart-sync no-remote-progress path) rather than paying a second TLS handshake.
  KOReaderSyncClient::Error result;
  {
    // Keep modem sleep disabled for just this PUT -- same reasoning as performSync()'s own guard
    // around its GET(s).
    const KOSyncWifiPowerSaveGuard wifiPowerSaveGuard;
    result = syncSession.updateProgress(progress, nullptr, "primary");
  }

  // Stats ride the same session, strictly after the progress upload is confirmed. Soft: it
  // cannot change `result`, the state machine, or anything the user sees.
  if (result == KOReaderSyncClient::OK) {
    ProgressSyncRetry::confirm(ProgressSyncRetry::Provider::KOSync, epubPath);
    drainBookOrbitStats();
  }

  // Drop the radio while user reads the result; full teardown happens at silent reboot.
  esp_wifi_stop();

  if (result != KOReaderSyncClient::OK) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = KOReaderSyncClient::errorString(result);
    }
    requestUpdate();
    return;
  }

  KOReaderAutoSync::recordManualSuccess();
  {
    RenderLock lock(*this);
    state = UPLOAD_COMPLETE;
  }
  markAutoReturn();
  requestUpdate(true);
}

void KOReaderSyncActivity::onEnter() {
  Activity::onEnter();
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  // Check for credentials first
  if (!KOREADER_STORE.hasCredentials()) {
    state = NO_CREDENTIALS;
    requestUpdate();
    return;
  }

  // Past this point every path uses WiFi.
  wifiActivated = true;

  // Check if already connected (e.g. from settings page auth)
  if (WiFi.status() == WL_CONNECTED) {
    LOG_DBG("KOSync", "Already connected to WiFi");
    onWifiSelectionComplete(true);
    return;
  }

  // Launch WiFi selection subactivity
  LOG_DBG("KOSync", "Launching WifiSelectionActivity...");
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void KOReaderSyncActivity::onExit() {
  Activity::onExit();

  if (wifiActivated) {
    WiFi.disconnect(false);
    delay(30);
    silentRestartToReader();
  }
}

void KOReaderSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_KOREADER_SYNC));

  int top = screen.y + screen.height / 2 - 40;
  if (state == NO_CREDENTIALS) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_NO_CREDENTIALS_MSG), true,
                              EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, tr(STR_KOREADER_SETUP_HINT), true,
                              EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNCING || state == UPLOADING) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, statusMessage.c_str(), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == SHOWING_RESULT) {
    // Show comparison
    top = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_PROGRESS_FOUND), true, EpdFontFamily::BOLD);

    // Remote chapter name requires Epub (loaded lazily in performSync before this state).
    const int remoteTocIndex = epub->getTocIndexForSpineIndex(remotePosition.spineIndex);
    const std::string remoteChapter =
        (remoteTocIndex >= 0) ? epub->getTocItem(remoteTocIndex).title
                              : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(remotePosition.spineIndex + 1));
    // Local chapter name was pre-computed before Epub was released.
    const std::string localChapter =
        !localChapterName.empty() ? localChapterName
                                  : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(currentSpineIndex + 1));

    // Remote progress - chapter and page
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 40, tr(STR_REMOTE_LABEL), true);
    char remoteChapterStr[128];
    snprintf(remoteChapterStr, sizeof(remoteChapterStr), "  %s", remoteChapter.c_str());
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 65, remoteChapterStr);
    char remotePageStr[64];
    snprintf(remotePageStr, sizeof(remotePageStr), tr(STR_PAGE_OVERALL_FORMAT), remotePosition.pageNumber + 1,
             remoteProgress.percentage * 100);
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 90, remotePageStr);

    if (!remoteProgress.device.empty()) {
      char deviceStr[64];
      snprintf(deviceStr, sizeof(deviceStr), tr(STR_DEVICE_FROM_FORMAT), remoteProgress.device.c_str());
      renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 115, deviceStr);
    }

    // Local progress - chapter and page
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 150, tr(STR_LOCAL_LABEL), true);
    char localChapterStr[128];
    snprintf(localChapterStr, sizeof(localChapterStr), "  %s", localChapter.c_str());
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 175, localChapterStr);
    char localPageStr[64];
    snprintf(localPageStr, sizeof(localPageStr), tr(STR_PAGE_TOTAL_OVERALL_FORMAT), currentPage + 1, totalPagesInSpine,
             localProgress.percentage * 100);
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 200, localPageStr);

    const int optionY = top + 230;
    const int optionHeight = 30;

    // Apply option
    if (selectedOption == 0) {
      renderer.fillRect(screen.x, optionY - 2, screen.width - 1, optionHeight);
    }
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, optionY, tr(STR_APPLY_REMOTE),
                      selectedOption != 0);

    // Upload option
    if (selectedOption == 1) {
      renderer.fillRect(screen.x, optionY + optionHeight - 2, screen.width - 1, optionHeight);
    }
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, optionY + optionHeight,
                      tr(STR_UPLOAD_LOCAL), selectedOption != 1);

    // Bottom button hints
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_NO_REMOTE_MSG), true, EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, tr(STR_UPLOAD_PROMPT));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_UPLOAD), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == UPLOAD_COMPLETE || state == SYNC_COMPLETE) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top,
                              state == UPLOAD_COMPLETE ? tr(STR_UPLOAD_SUCCESS) : tr(STR_ALREADY_SYNCED), true,
                              EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNC_FAILED) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_SYNC_FAILED_MSG), true, EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, statusMessage.c_str());

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

void KOReaderSyncActivity::loop() {
  if (state == NO_CREDENTIALS || state == SYNC_FAILED || state == UPLOAD_COMPLETE || state == SYNC_COMPLETE) {
    if (autoReturnAt != 0 && millis() >= autoReturnAt) {
      returnToReader();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      returnToReader();
    }
    return;
  }

  if (state == SHOWING_RESULT) {
    auto chooseSelected = [this] {
      if (selectedOption == 0) {
        saveProgressAndReturn(remotePosition.spineIndex, remotePosition.pageNumber,
                              static_cast<uint64_t>(remoteProgress.timestamp));
      } else if (selectedOption == 1) {
        performUpload();
      }
    };

    {
      const auto& metrics = UITheme::getInstance().getMetrics();
      const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
      const int top = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
      constexpr int optionHeight = 30;
      int touchedOption = -1;
      const auto touch = mappedInput.rowTouch(touchedOption, top + 230 - 2, optionHeight, 2);
      if (touch == MappedInputManager::RowTouch::Down) {
        if (selectedOption != touchedOption) {
          selectedOption = touchedOption;
          requestUpdate();
        }
        return;
      }
      if (touch == MappedInputManager::RowTouch::Tap) {
        selectedOption = touchedOption;
        chooseSelected();
        return;
      }
    }

    // Navigate options
    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      selectedOption = (selectedOption + 1) % 2;  // Wrap around among 2 options
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
               mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      selectedOption = (selectedOption + 1) % 2;  // Wrap around among 2 options
      requestUpdate();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      chooseSelected();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
    }
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty) && ty > renderer.getScreenHeight() / 3 &&
        ty < renderer.getScreenHeight() * 2 / 3) {
      if (documentHash.empty()) {
        if (KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME) {
          documentHash = KOReaderDocumentId::calculateFromFilename(epubPath);
        } else {
          documentHash = KOReaderDocumentId::calculate(epubPath);
        }
      }
      performUpload();
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // Calculate hash if not done yet
      if (documentHash.empty()) {
        if (KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME) {
          documentHash = KOReaderDocumentId::calculateFromFilename(epubPath);
        } else {
          documentHash = KOReaderDocumentId::calculate(epubPath);
        }
      }
      performUpload();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
    }
    return;
  }
}
