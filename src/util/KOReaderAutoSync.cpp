#include "KOReaderAutoSync.h"

#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <WiFi.h>

#include <atomic>
#include <cmath>
#include <memory>
#include <optional>

#include "BookOrbitStatsSync.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "Epub.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "KOReaderSyncClient.h"
#include "KOSyncStatus.h"
#include "KOSyncWifiPowerSaveGuard.h"
#include "ProgressMapper.h"
#include "ProgressPullPolicy.h"
#include "ProgressSyncDeferred.h"
#include "ProgressSyncRetry.h"
#include "ProgressSyncPosition.h"
#include "ProgressSyncNetwork.h"
#include "WifiCredentialStore.h"
#include "activities/reader/EpubReaderUtils.h"

namespace KOReaderAutoSync {

namespace {

// Direct connect to the last-known network, no scan -- a scan alone can cost several seconds,
// which this path can't afford. Bounded and abortable so a sleep transition or a book-open can
// never be stuck waiting on a network that isn't there.
std::atomic<bool> pendingSuccessNotice{false};

bool userRequestedAbort() {
  gpio.update();
  return gpio.wasAnyPressed();
}

// Returns true once actually connected. Callers only reach here after confirming a saved
// credential exists, so a false return here always means "attempted and failed/timed out/aborted"
// -- never "never tried."
bool connectToSavedWifi() {
  if (!ProgressSyncNetwork::connect()) return false;
  return true;
}

void disconnectWifi() { ProgressSyncNetwork::disconnect(); }

// buildIfMissing=true: a book being pulled for the first time on this device has no book.bin cache
// yet, and a real spine build is the only way calculateProgress()/ProgressMapper get usable spine
// sizes (loadMetadataOnly() explicitly skips spine entries -- see Epub::loadMetadataOnly()). A book
// being pushed always already has one, from the reading session that just ended.
bool loadEpubForSync(Epub& epub, bool buildIfMissing) {
  if (buildIfMissing) epub.setupCacheDir();
  return epub.load(buildIfMissing, /*skipLoadingCss=*/true);
}

std::string computeDocumentHash(const std::string& path) {
  return KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME
             ? KOReaderDocumentId::calculateForProgress(path, true)
             : ProgressSyncNetwork::binaryDocument(path);
}

void recordSuccessfulSync(bool queueNotice) {
  uint32_t nowMinutes = 0;
  if (halClock.getEpochMinutes(nowMinutes)) {
    APP_STATE.lastKoSyncEpochMinutes = nowMinutes;
    if (!APP_STATE.saveToFile()) {
      LOG_ERR("KOAuto", "Could not persist last successful sync time");
    }
  }
  if (queueNotice) pendingSuccessNotice.store(true, std::memory_order_release);
}

// Drain this book's queued BookOrbit reading events, strictly AFTER the normal KOSync progress
// transaction has succeeded.
//
// Opens its own BookOrbitSyncClient::Session rather than reusing the KOSync session's connection
// (unlike e51b383): BookOrbit is now an explicit, standalone provider with its own credential
// store and commonly a different host entirely, so there is no shared TLS connection to reuse.
// Calling this only after the KOSync transaction succeeds is a scheduling convenience (Wi-Fi
// already happens to be up right here), not a protocol dependency -- see
// docs/bookorbit-feature-matrix.md's Phase 3 plan for giving BookOrbit its own independent sync
// trigger so a device with no KOSync configured still gets automatic stats uploads.
//
// Deliberately returns void: every failure inside is soft. The queue is retained, the progress
// sync that already succeeded is untouched, and the caller's own return value -- which decides
// whether a success notice and timestamp are recorded -- never sees this. The scoped
// KOSyncWifiPowerSaveGuard and the transient-font-cache release mirror exactly what the GET and
// PUT above already do, so the same modem-sleep and heap hazards are covered.
//
// A no-op with zero network traffic unless reading stats are on AND BookOrbitCredentialStore has
// credentials configured -- see BookOrbitStats::uploadQueued().
// `progressHash` is NOT what the stats go out under. BookOrbit resolves a page-stats hash only
// against an intrinsic binary partial MD5, a hash-history entry, or a manual link that already
// exists -- and its page-stats endpoint never records an unmatched hash anywhere a user could
// link it from (only match-check does that, which this firmware does not implement). So a
// filename-mode progress hash would come back unmatched forever. The stats identity is derived
// separately, and only once it is known the drain could do something.
void uploadBookOrbitStats(GfxRenderer& renderer, const std::string& epubPath) {
  // Checked before deriving the identity: calculateForStats() reads chunks from the book file,
  // and a device with no BookOrbit credentials configured should not pay for that.
  if (!BookOrbitStats::drainPossible()) return;
  const std::string statsHash = ProgressSyncNetwork::binaryDocument(epubPath);
  if (statsHash.empty()) return;

  if (auto* fcm = renderer.getFontCacheManager()) fcm->forceReleaseTransientCaches();
  const KOSyncWifiPowerSaveGuard wifiPowerSaveGuard;
  BookOrbitStats::uploadQueued(statsHash, userRequestedAbort);
}

ProgressSyncRetry::Outcome synchronizeProgress(const std::string& epubPath, GfxRenderer* renderer,
                                               bool buildIfMissing, bool progressOnly) {
  using ProgressSyncRetry::Outcome;
  using ProgressPullPolicy::RemoteState;
  using ProgressPullPolicy::SyncAction;
  const auto finish = [&epubPath](Outcome outcome) {
    ProgressSyncRetry::record(ProgressSyncRetry::Provider::KOSync, epubPath, outcome);
    return outcome;
  };
  if (!KOREADER_STORE.getAutoSyncEnabled()) {
    LOG_DBG("KOAuto", "Sync: gated out -- auto-sync disabled in settings");
    return finish(Outcome::NoRetry);
  }
  if (epubPath.empty() || !FsHelpers::hasEpubExtension(epubPath)) {
    LOG_DBG("KOAuto", "Sync: gated out -- no EPUB target (path='%s')", epubPath.c_str());
    return finish(Outcome::NoRetry);
  }
  if (WIFI_STORE.getCredentialCount() == 0 || !KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOAuto", "Sync: gated out -- %zu saved WiFi network(s), KOReader credentials %s",
            WIFI_STORE.getCredentialCount(), KOREADER_STORE.hasCredentials() ? "present" : "MISSING");
    return finish(Outcome::NoRetry);
  }

  const auto matchMethod = KOREADER_STORE.getMatchMethod();
  const std::string hash = computeDocumentHash(epubPath);
  LOG_DBG("KOAuto", "Sync: doc='%s' match=%s hash=%s", epubPath.c_str(),
          matchMethod == DocumentMatchMethod::FILENAME ? "filename" : "binary",
          hash.empty() ? "(empty)" : hash.c_str());
  if (hash.empty()) {
    LOG_DBG("KOAuto", "Sync: gated out -- empty document hash");
    return finish(Outcome::NoRetry);
  }

  if (renderer) {
    if (auto* fcm = renderer->getFontCacheManager()) fcm->forceReleaseTransientCaches();
  }

  auto epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
  if (!loadEpubForSync(*epub, buildIfMissing)) {
    LOG_DBG("KOAuto", "Sync: could not load EPUB%s, skipping", buildIfMissing ? "/build cache" : " from cache");
    return finish(Outcome::NoRetry);
  }

  int spineIndex = 0;
  int pageNumber = 0;
  int pageCount = 0;
  uint64_t localTimestamp = 0;
  bool localLegacy = false;
  bool hasLocal = EpubReaderUtils::loadProgress(epub->getCachePath(), spineIndex, pageNumber, pageCount,
                                                localTimestamp, localLegacy);
  const auto chapterProgress = hasLocal ? ProgressSyncPosition::chapterProgress(
                                             {spineIndex, pageNumber, pageCount}, epub->getSpineItemsCount())
                                       : std::nullopt;
  hasLocal = chapterProgress.has_value();
  float localPercentage = 0.0f;
  SavedProgressPosition saved;
  if (hasLocal) {
    localPercentage = epub->calculateProgress(spineIndex, *chapterProgress);
    const std::string binaryDocument =
        matchMethod == DocumentMatchMethod::FILENAME ? ProgressSyncNetwork::binaryDocument(epubPath) : hash;
    saved = ProgressSyncPosition::resolve(epub, CrossPointPosition{spineIndex, pageNumber, pageCount},
                                           binaryDocument);
    if (!std::isfinite(localPercentage) || localPercentage < 0.0f || localPercentage > 1.0f || saved.xpath.empty()) {
      LOG_ERR("KOAuto", "Sync: invalid local progress; treating it as absent");
      hasLocal = false;
      localPercentage = 0.0f;
      saved = {};
    }
  }

  DeferredProgress deferred;
  const bool hasDeferred = ProgressSyncDeferred::pendingForBook(epubPath, deferred);
  epub.reset();
  if (renderer) {
    if (auto* fcm = renderer->getFontCacheManager()) fcm->forceReleaseTransientCaches();
  }

  if (!connectToSavedWifi()) {
    LOG_DBG("KOAuto", "Sync: WiFi connect failed or timed out");
    disconnectWifi();
    return finish(Outcome::Retry);
  }
  LOG_DBG("KOAuto", "Sync: WiFi connected (%s)", WiFi.SSID().c_str());

  // One session for the whole transaction (GET then PUT below): KOReaderSyncClient::Session
  // reuses its underlying connection across both requests when scheme/host/port match (always
  // true here -- both hit the same configured server), so this normally pays for one TLS
  // handshake instead of two. See KOReaderSyncClient.h's Session doc comment.
  KOReaderSyncClient::Session session;
  KOReaderProgress remote;
  KOReaderSyncClient::Error getResult;
  {
    // Keep modem sleep disabled for just this GET: its latency has been observed to make the
    // wolfSSL TLS handshake fail with http=-1. Scoped tightly around the request itself, not the
    // local work, which doesn't need the radio quiescent (see
    // pullFurthestOnOpen()'s matching guard for the same reasoning).
    const KOSyncWifiPowerSaveGuard wifiPowerSaveGuard;
    getResult = session.getProgress(hash, remote, userRequestedAbort);
  }
  LOG_DBG("KOAuto", "Sync: remote fetch -> %s (http=%d)", KOReaderSyncClient::errorString(getResult),
          session.lastHttpCode());
  if (getResult != KOReaderSyncClient::OK && getResult != KOReaderSyncClient::NOT_FOUND) {
    disconnectWifi();
    return finish(ProgressSyncRetry::classify<KOReaderSyncClient>(getResult, session.lastHttpCode()));
  }

  const bool validRich = remote.position.has_value() && remote.position->pctQ <= 1000000U &&
                         remote.position->totalPages > 0;
  const bool validRemote = getResult == KOReaderSyncClient::OK && std::isfinite(remote.percentage) &&
                           remote.percentage >= 0.0f && remote.percentage <= 1.0f &&
                           (!remote.progress.empty() || validRich);
  if (getResult == KOReaderSyncClient::OK && !validRemote) {
    LOG_ERR("KOAuto", "Sync: rejecting invalid/empty remote progress");
    disconnectWifi();
    return finish(Outcome::Retry);
  }

  const RemoteState remoteState = getResult == KOReaderSyncClient::NOT_FOUND ? RemoteState::Missing
                                                                             : RemoteState::Found;
  const bool localSameRemote =
      hasLocal && validRemote &&
      ((!remote.progress.empty() && saved.xpath == remote.progress) ||
       (validRich && remote.position->spineIndex == spineIndex && remote.position->pageNumber == pageNumber));
  const bool localSameDeferred =
      hasLocal && hasDeferred &&
      ((!deferred.xpath.empty() && saved.xpath == deferred.xpath) ||
       (deferred.hasRichPosition && deferred.richSpineIndex == spineIndex && deferred.richPageNumber == pageNumber));
  DeferredProgress remoteCandidate;
  if (validRemote) {
    remoteCandidate.provider = static_cast<uint8_t>(ProgressSyncRetry::Provider::KOSync);
    remoteCandidate.path = epubPath;
    remoteCandidate.document = hash;
    remoteCandidate.xpath = remote.progress;
    remoteCandidate.percentage = remote.percentage;
    remoteCandidate.timestamp = static_cast<uint64_t>(remote.timestamp);
    if (remote.position.has_value()) {
      const auto& rich = *remote.position;
      remoteCandidate.hasRichPosition = true;
      remoteCandidate.richPctQ = rich.pctQ;
      remoteCandidate.richSpineIndex = rich.spineIndex;
      remoteCandidate.richPageNumber = rich.pageNumber;
      remoteCandidate.richTotalPages = rich.totalPages;
      remoteCandidate.richHasParagraphIndex = rich.paragraphIndex.has_value();
      remoteCandidate.richParagraphIndex = rich.paragraphIndex.value_or(0);
      if (remoteCandidate.xpath.empty()) remoteCandidate.xpath = rich.xpath;
    }
  }
  const bool remoteSameDeferred = validRemote && hasDeferred && deferred.samePositionAs(remoteCandidate);
  const SyncAction action = hasLocal && localLegacy
                                ? ProgressPullPolicy::decideLegacy(remoteState, hasDeferred)
                                : ProgressPullPolicy::decide(
                                      {hasLocal, localTimestamp, remoteState,
                                       validRemote ? static_cast<uint64_t>(remote.timestamp) : 0, localSameRemote,
                                       hasDeferred, hasDeferred ? deferred.timestamp : 0, localSameDeferred,
                                       remoteSameDeferred});
  LOG_DBG("KOAuto", "Sync: local=%s%.4f@%llu remote=%s%.4f@%lld deferred=%s%.4f@%llu action=%u",
          hasLocal ? "" : "none ", localPercentage, static_cast<unsigned long long>(localTimestamp),
          remoteState == RemoteState::Found ? "" : "none ", validRemote ? remote.percentage : 0.0f,
          static_cast<long long>(remote.timestamp), hasDeferred ? "" : "none ",
          hasDeferred ? deferred.percentage : 0.0f,
          static_cast<unsigned long long>(hasDeferred ? deferred.timestamp : 0),
          static_cast<unsigned>(action));

  if (action == SyncAction::PullRemote) {
    const bool staged = ProgressSyncDeferred::stage(remoteCandidate);
    session.end();
    if (!progressOnly) uploadBookOrbitStats(*renderer, epubPath);
    disconnectWifi();
    if (!staged) return finish(Outcome::Retry);
    return finish(Outcome::Confirmed);
  }

  if (action == SyncAction::HoldUnknown || action == SyncAction::Abort) {
    session.end();
    if (!progressOnly) uploadBookOrbitStats(*renderer, epubPath);
    disconnectWifi();
    return finish(Outcome::NoRetry);
  }

  if (action != SyncAction::PushLocal) {
    session.end();
    if (!progressOnly) uploadBookOrbitStats(*renderer, epubPath);
    disconnectWifi();
    return finish(Outcome::Confirmed);
  }

  KOReaderProgress toPush;
  toPush.document = hash;
  toPush.progress = saved.xpath;
  toPush.percentage = saved.percentage;
  toPush.device = SETTINGS.getEffectiveDeviceName();
  if (KOREADER_STORE.usesCrossPointSyncServer() && pageCount > 0) {
    KOReaderRichPosition rich;
    rich.pctQ = static_cast<uint32_t>(saved.percentage * 1000000.0f + 0.5f);
    rich.spineIndex = static_cast<uint16_t>(spineIndex);
    rich.pageNumber = static_cast<uint16_t>(pageNumber);
    rich.totalPages = static_cast<uint16_t>(pageCount);
    rich.xpath = saved.xpath;
    toPush.position = std::move(rich);
  }

  // Release transient caches again immediately before the PUT. The EPUB was
  // already freed before connecting; the GET may have used temporary buffers.
  if (renderer) {
    if (auto* fcm = renderer->getFontCacheManager()) fcm->forceReleaseTransientCaches();
  }

  LOG_DBG("KOAuto", "Push: sending document=%s device=%s percentage=%.4f xpath=%s", toPush.document.c_str(),
          toPush.device.c_str(), toPush.percentage, toPush.progress.c_str());

  KOReaderSyncClient::Error putResult;
  {
    // Keep modem sleep disabled for just this PUT -- same reasoning as the GET's own guard above.
    const KOSyncWifiPowerSaveGuard wifiPowerSaveGuard;
    putResult = session.updateProgress(toPush, userRequestedAbort);
  }
  LOG_DBG("KOAuto", "Push result: %s (http=%d, local=%.4f)", KOReaderSyncClient::errorString(putResult),
          session.lastHttpCode(), localPercentage);
  // Strictly after the progress upload has been confirmed, and on the same Session, so the
  // reused TLS connection is paid for once. A stats failure cannot reach the return value.
  const auto outcome = finish(ProgressSyncRetry::classify<KOReaderSyncClient>(putResult, session.lastHttpCode()));
  session.end();
  if (putResult == KOReaderSyncClient::OK && !progressOnly) uploadBookOrbitStats(*renderer, epubPath);
  disconnectWifi();
  return outcome;
}

}  // namespace

void syncOnSleep(GfxRenderer& renderer) {
  if (!KOREADER_STORE.getAutoSyncEnabled()) {
    LOG_DBG("KOAuto", "Push: gated out -- auto-sync disabled in settings");
    return;
  }
  if (!APP_STATE.lastSleepFromReader) {
    LOG_DBG("KOAuto", "Push: gated out -- last sleep wasn't from the reader");
    return;
  }
  const std::string path = APP_STATE.openEpubPath;
  if (synchronizeProgress(path, &renderer, /*buildIfMissing=*/false,
                          /*progressOnly=*/false) == ProgressSyncRetry::Outcome::Confirmed) {
    recordSuccessfulSync(false);
  }
  // synchronizeProgress's Session and TLS transport have been destroyed before layout work.
  if (!ProgressSyncNetwork::active()) ProgressSyncDeferred::applyForBook(path, renderer);
}

void syncHeadlessly(const std::string& epubPath) {
  if (synchronizeProgress(epubPath, nullptr, /*buildIfMissing=*/false,
                          /*progressOnly=*/true) == ProgressSyncRetry::Outcome::Confirmed) {
    recordSuccessfulSync(false);
  }
}

void syncOnClose(GfxRenderer& renderer) {
  const std::string path = APP_STATE.openEpubPath;
  if (synchronizeProgress(path, &renderer, /*buildIfMissing=*/false,
                          /*progressOnly=*/false) == ProgressSyncRetry::Outcome::Confirmed) {
    recordSuccessfulSync(true);
  }
  if (!ProgressSyncNetwork::active()) ProgressSyncDeferred::applyForBook(path, renderer);
}

void syncOnOpen(const std::string& epubPath, GfxRenderer& renderer) {
  if (synchronizeProgress(epubPath, &renderer, /*buildIfMissing=*/true,
                          /*progressOnly=*/false) == ProgressSyncRetry::Outcome::Confirmed) {
    recordSuccessfulSync(true);
  }
  if (!ProgressSyncNetwork::active()) ProgressSyncDeferred::applyForBook(epubPath, renderer);
}

bool consumeSuccessNotice() { return pendingSuccessNotice.exchange(false, std::memory_order_acq_rel); }

bool formatLastSuccessRelative(char* output, size_t outputSize) {
  uint32_t nowMinutes = 0;
  if (!halClock.getEpochMinutes(nowMinutes)) return false;
  return KOSyncStatus::formatRelative(nowMinutes, APP_STATE.lastKoSyncEpochMinutes, output, outputSize);
}

void recordManualSuccess() {
  ProgressSyncRetry::confirm(ProgressSyncRetry::Provider::KOSync, APP_STATE.openEpubPath);
  recordSuccessfulSync(false);
}

void recordProviderSuccess(bool queueNotice) { recordSuccessfulSync(queueNotice); }

}  // namespace KOReaderAutoSync
