#include "BookOrbitAutoSync.h"

#include <BookOrbitCredentialStore.h>
#include <BookOrbitSyncClient.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <ProgressPullPolicy.h>
#include <WiFi.h>

#include <cmath>
#include <memory>
#include <optional>

#include "BookOrbitBookmarkSync.h"
#include "BookOrbitResetPolicy.h"
#include "BookOrbitStatsSync.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "Epub.h"
#include "KOReaderAutoSync.h"
#include "KOReaderDocumentId.h"
#include "ProgressMapper.h"
#include "ProgressSyncDeferred.h"
#include "ProgressSyncRetry.h"
#include "ProgressSyncPosition.h"
#include "ProgressSyncNetwork.h"
#include "WifiCredentialStore.h"
#include "activities/reader/EpubReaderUtils.h"

namespace BookOrbitAutoSync {

namespace {


bool userRequestedAbort() {
  gpio.update();
  return gpio.wasAnyPressed();
}

bool connectToSavedWifi() {
  if (!ProgressSyncNetwork::connect()) return false;

  uint64_t now = 0;
  if (halClock.isAvailable() && !halClock.getEpochSeconds(now)) {
    LOG_INF("BOAuto", "RTC unavailable; recovering time for subsequent reading events");
    if (!halClock.syncFromNTP()) LOG_INF("BOAuto", "RTC recovery failed; preserving unknown progress chronology");
  }
  return true;
}

void disconnectWifi() { ProgressSyncNetwork::disconnect(); }

bool loadEpubForSync(Epub& epub, bool buildIfMissing) {
  if (buildIfMissing) epub.setupCacheDir();
  return epub.load(buildIfMissing, /*skipLoadingCss=*/true);
}

// Always the binary partial MD5: BookOrbitCredentialStore.h's own doc comment notes BookOrbit has
// no document-matching-method setting, unlike KOReaderCredentialStore, so there is no configured
// preference to honour here the way computeDocumentHash() in KOReaderAutoSync.cpp honours one.
// This is an inference from that design, not independently verified against a live server's own
// /syncs/progress route (only its page-stats route has been) -- see
// BookOrbitSyncClient::Session::getProgress's own doc comment for the same caveat.
std::string computeDocumentHash(const std::string& path) { return ProgressSyncNetwork::binaryDocument(path); }

bool resetMarkerMatches(const std::string& path, const std::string& document) {
  return APP_STATE.bookOrbitResetPath == path && APP_STATE.bookOrbitResetDocument == document &&
         ProgressTimestamp::valid(APP_STATE.bookOrbitResetTimestamp);
}

bool persistResetMarkerAcknowledged() {
  const bool before = APP_STATE.bookOrbitResetAcknowledged;
  APP_STATE.bookOrbitResetAcknowledged = true;
  if (APP_STATE.saveToFile()) return true;
  APP_STATE.bookOrbitResetAcknowledged = before;
  return false;
}

bool clearResetMarker() {
  const std::string oldPath = APP_STATE.bookOrbitResetPath;
  const std::string oldDocument = APP_STATE.bookOrbitResetDocument;
  const uint64_t oldTimestamp = APP_STATE.bookOrbitResetTimestamp;
  const bool oldAcknowledged = APP_STATE.bookOrbitResetAcknowledged;
  APP_STATE.bookOrbitResetPath.clear();
  APP_STATE.bookOrbitResetDocument.clear();
  APP_STATE.bookOrbitResetTimestamp = 0;
  APP_STATE.bookOrbitResetAcknowledged = false;
  if (APP_STATE.saveToFile()) return true;
  APP_STATE.bookOrbitResetPath = oldPath;
  APP_STATE.bookOrbitResetDocument = oldDocument;
  APP_STATE.bookOrbitResetTimestamp = oldTimestamp;
  APP_STATE.bookOrbitResetAcknowledged = oldAcknowledged;
  return false;
}

enum class StandingResetResult { NotApplicable, Completed, HoldUnknown, RequestFailed, PersistenceFailed };

// BookOrbit represents one outstanding web reset as a fresh-timestamp start record on every GET.
// Once we have durably applied that start, handle the server's two-step convergence handshake
// explicitly so its synthetic timestamp can never masquerade as another reading event.
StandingResetResult handleStandingReset(BookOrbitSyncClient::Session& session, const BookOrbitProgress& remote,
                                        const std::string& path, const std::string& document, bool hasLocal,
                                        uint64_t localTimestamp, const SavedProgressPosition& local,
                                        BookOrbitSyncClient::Error& error, bool& pushedLocal) {
  error = BookOrbitSyncClient::OK;
  pushedLocal = false;
  const bool marked = resetMarkerMatches(path, document);
  if (!remote.isWebReset()) {
    if (marked && !clearResetMarker()) return StandingResetResult::PersistenceFailed;
    return StandingResetResult::NotApplicable;
  }
  const bool localIsStart = hasLocal && local.xpath == "/body/DocFragment[1]/body";
  const auto decision = BookOrbitResetPolicy::decide(remote.isWebReset(), marked, APP_STATE.bookOrbitResetAcknowledged,
                                                     hasLocal, localIsStart, ProgressTimestamp::valid(localTimestamp));
  if (!decision.standingReset) return StandingResetResult::NotApplicable;

  const auto makeProgress = [&](const std::string& xpath, float percentage, uint64_t timestamp) {
    BookOrbitProgress progress;
    progress.document = document;
    progress.progress = xpath;
    progress.percentage = percentage;
    progress.device = SETTINGS.getEffectiveDeviceName();
    progress.deviceId = BookOrbitStats::statsDeviceId();
    progress.timestamp = static_cast<int64_t>(timestamp);
    return progress;
  };

  if (decision.acknowledgeStart) {
    BookOrbitProgress ack = makeProgress("/body/DocFragment[1]/body", 0.0f, APP_STATE.bookOrbitResetTimestamp);
    error = session.updateProgress(ack, userRequestedAbort);
    if (error != BookOrbitSyncClient::OK) return StandingResetResult::RequestFailed;
    if (!persistResetMarkerAcknowledged()) return StandingResetResult::PersistenceFailed;
  }

  if (decision.holdUnknown || local.xpath.empty()) return StandingResetResult::HoldUnknown;
  if (!decision.pushAdvance) return StandingResetResult::Completed;

  BookOrbitProgress advance = makeProgress(local.xpath, local.percentage, localTimestamp);
  error = session.updateProgressAndVerify(advance, userRequestedAbort);
  if (error != BookOrbitSyncClient::OK) return StandingResetResult::RequestFailed;
  pushedLocal = true;
  if (!clearResetMarker()) return StandingResetResult::PersistenceFailed;
  return StandingResetResult::Completed;
}

// What runs on the progress session after the progress decision. Full: stats + bookmarks (open,
// close, sleep, manual). StatsOnly: periodic timer wake -- stats only; bookmarks and the catalog
// never run there (docs/periodic-progress-sync.md). Either way this happens exactly once per
// window, on the session that is already open, before the caller's single disconnectWifi().
enum class AfterProgress : uint8_t { Full, StatsOnly };

// Stats phase bounds per window (see BookOrbitStats::drainWindow()): other books' non-empty
// queues after the current book, and one deadline for the whole phase.
constexpr size_t OTHER_STATS_BOOKS_PER_WINDOW = 3;
constexpr uint32_t STATS_PHASE_BUDGET_MS = 15000;

// Drains the queued BookOrbit stats AND (Full only) runs a bookmark exchange round, both on the
// SAME session/connection as the progress request that just ran -- unlike KOReaderAutoSync.cpp's
// uploadBookOrbitStats(), which must open a second session because its own session is a
// different client type. Each half is independently idempotent/gated when it has nothing to do
// (e.g. a device with KOSync also configured, whose own window already drained the stats queue
// this wake; or a server that hasn't confirmed hasBookmarkSync), so calling both unconditionally
// here costs nothing extra in that case. The stats phase (current book, then up to
// OTHER_STATS_BOOKS_PER_WINDOW other owner-matching queued books, fairly rotated) runs once under
// one deadline and stops at the first failure that would only repeat -- see drainWindow().
void drainStats(BookOrbitSyncClient::Session& session, const std::string& epubPath, const AfterProgress after) {
  const std::string statsHash = ProgressSyncNetwork::binaryDocument(epubPath);
  if (BookOrbitStats::drainPossible() && !statsHash.empty()) {
    BookOrbitStats::WindowLimits limits;
    limits.budgetMs = STATS_PHASE_BUDGET_MS;
    limits.maxOtherBooks = OTHER_STATS_BOOKS_PER_WINDOW;
    const auto window = BookOrbitStats::drainWindow(session, statsHash, limits, userRequestedAbort);
    LOG_DBG("BOAuto", "Stats window: current=%u others=%u scanned=%u stopped=%d", static_cast<unsigned>(window.current),
            static_cast<unsigned>(window.otherAttempted), static_cast<unsigned>(window.scannedEntries),
            window.stopped ? 1 : 0);
  }
  if (after == AfterProgress::Full && !statsHash.empty()) {
    BookOrbitBookmarkSync::sync(session, epubPath, statsHash, userRequestedAbort);
  }
}

ProgressSyncRetry::Outcome synchronizeProgress(const std::string& epubPath, GfxRenderer* renderer, bool buildIfMissing,
                                               AfterProgress after) {
  using ProgressPullPolicy::RemoteState;
  using ProgressPullPolicy::SyncAction;
  using ProgressSyncRetry::Outcome;
  const auto finish = [&epubPath](Outcome outcome) {
    ProgressSyncRetry::record(ProgressSyncRetry::Provider::BookOrbit, epubPath, outcome);
    return outcome;
  };
  if (!BOOKORBIT_STORE.hasCredentials()) {
    LOG_DBG("BOAuto", "Sync: gated out -- no BookOrbit credentials configured");
    return finish(Outcome::NoRetry);
  }
  if (epubPath.empty() || !FsHelpers::hasEpubExtension(epubPath)) {
    LOG_DBG("BOAuto", "Sync: gated out -- no EPUB target (path='%s')", epubPath.c_str());
    return finish(Outcome::NoRetry);
  }
  if (WIFI_STORE.getCredentialCount() == 0) {
    LOG_DBG("BOAuto", "Sync: gated out -- no saved WiFi network");
    return finish(Outcome::NoRetry);
  }

  const std::string hash = computeDocumentHash(epubPath);
  LOG_DBG("BOAuto", "Sync: doc='%s' hash=%s", epubPath.c_str(), hash.empty() ? "(empty)" : hash.c_str());
  if (hash.empty()) {
    LOG_DBG("BOAuto", "Sync: gated out -- empty document hash");
    return finish(Outcome::NoRetry);
  }

  if (renderer) {
    if (auto* fcm = renderer->getFontCacheManager()) fcm->forceReleaseTransientCaches();
  }

  auto epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
  if (!loadEpubForSync(*epub, buildIfMissing)) {
    LOG_DBG("BOAuto", "Sync: could not load EPUB%s, skipping", buildIfMissing ? "/build cache" : " from cache");
    return finish(Outcome::NoRetry);
  }

  int spineIndex = 0;
  int pageNumber = 0;
  int pageCount = 0;
  uint64_t localTimestamp = 0;
  bool localLegacy = false;
  bool hasLocal = EpubReaderUtils::loadProgress(epub->getCachePath(), spineIndex, pageNumber, pageCount, localTimestamp,
                                                localLegacy);
  hasLocal = hasLocal && spineIndex >= 0 && spineIndex < epub->getSpineItemsCount() && pageNumber >= 0 &&
             pageCount > 0 && pageNumber < pageCount;
  float localPercentage = 0.0f;
  SavedProgressPosition saved;
  if (hasLocal) {
    localPercentage = epub->calculateProgress(spineIndex, static_cast<float>(pageNumber) / pageCount);
    saved = ProgressSyncPosition::resolve(epub, CrossPointPosition{spineIndex, pageNumber, pageCount}, hash, renderer);
    if (!std::isfinite(localPercentage) || localPercentage < 0.0f || localPercentage > 1.0f || saved.xpath.empty()) {
      LOG_ERR("BOAuto", "Sync: invalid local progress; treating it as absent");
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
    LOG_DBG("BOAuto", "Sync: WiFi connect failed or timed out");
    disconnectWifi();
    return finish(Outcome::Retry);
  }
  LOG_DBG("BOAuto", "Sync: WiFi connected (%s)", WiFi.SSID().c_str());

  BookOrbitSyncClient::Session session;

  // Gate progress GET/PUT on a confirmed BookOrbit server, same as stats/bookmark sync already
  // are -- otherwise a server that merely speaks the shared KOSync auth dialect but never
  // confirmed BookOrbit (e.g. a plain KOSync endpoint reachable at the same credentials) could
  // read or receive a progress push. After the first confirmation this is a cache hit with no
  // network cost, so only the very first sync after setup pays this extra round trip -- the same
  // cost already accepted for stats capture.
  BookOrbitSyncClient::Capabilities caps;
  const auto capsResult = BookOrbitStats::ensureCapabilitiesConfirmed(session, caps, userRequestedAbort);
  if (capsResult != BookOrbitSyncClient::OK) {
    LOG_DBG("BOAuto", "Sync: gated out -- server not yet confirmed BookOrbit");
    disconnectWifi();
    return finish(ProgressSyncRetry::classify<BookOrbitSyncClient>(capsResult, session.lastHttpCode()));
  }

  BookOrbitProgress remote;
  const BookOrbitSyncClient::Error getResult = session.getProgress(hash, remote, userRequestedAbort);
  LOG_DBG("BOAuto", "Sync: remote fetch -> %s (http=%d)", BookOrbitSyncClient::errorString(getResult),
          session.lastHttpCode());
  if (getResult != BookOrbitSyncClient::OK && getResult != BookOrbitSyncClient::NOT_FOUND) {
    disconnectWifi();
    return finish(ProgressSyncRetry::classify<BookOrbitSyncClient>(getResult, session.lastHttpCode()));
  }

  if (renderer) {
    if (auto* fcm = renderer->getFontCacheManager()) fcm->forceReleaseTransientCaches();
  }

  const bool validRemote = getResult == BookOrbitSyncClient::OK && std::isfinite(remote.percentage) &&
                           remote.percentage >= 0.0f && remote.percentage <= 1.0f && !remote.progress.empty();
  if (getResult == BookOrbitSyncClient::OK && !validRemote) {
    LOG_ERR("BOAuto", "Sync: rejecting invalid/empty remote progress");
    disconnectWifi();
    return finish(Outcome::Retry);
  }

  if (validRemote) {
    BookOrbitSyncClient::Error resetError = BookOrbitSyncClient::OK;
    bool pushedAfterReset = false;
    const StandingResetResult resetResult = handleStandingReset(session, remote, epubPath, hash, hasLocal,
                                                                localTimestamp, saved, resetError, pushedAfterReset);
    if (resetResult != StandingResetResult::NotApplicable) {
      drainStats(session, epubPath, after);
      disconnectWifi();
      if (resetResult == StandingResetResult::Completed) return finish(Outcome::Confirmed);
      if (resetResult == StandingResetResult::HoldUnknown) return finish(Outcome::NoRetry);
      if (resetResult == StandingResetResult::RequestFailed) {
        return finish(ProgressSyncRetry::classify<BookOrbitSyncClient>(resetError, session.lastHttpCode()));
      }
      return finish(Outcome::Retry);
    }
  }

  const RemoteState remoteState =
      getResult == BookOrbitSyncClient::NOT_FOUND ? RemoteState::Missing : RemoteState::Found;
  DeferredProgress remoteCandidate;
  if (validRemote) {
    remoteCandidate.provider = static_cast<uint8_t>(ProgressSyncRetry::Provider::BookOrbit);
    remoteCandidate.path = epubPath;
    remoteCandidate.document = hash;
    remoteCandidate.xpath = remote.progress;
    remoteCandidate.percentage = remote.percentage;
    remoteCandidate.timestamp = static_cast<uint64_t>(remote.timestamp);
    remoteCandidate.bookOrbitReset = remote.isWebReset();
  }
  const bool localSameRemote = hasLocal && validRemote && saved.xpath == remote.progress;
  const bool localSameDeferred = hasLocal && hasDeferred && !deferred.xpath.empty() && saved.xpath == deferred.xpath;
  const bool remoteSameDeferred = validRemote && hasDeferred && deferred.samePositionAs(remoteCandidate);
  const SyncAction action =
      hasLocal && localLegacy
          ? ProgressPullPolicy::decideLegacy(remoteState, hasDeferred)
          : ProgressPullPolicy::decide({hasLocal, localTimestamp, remoteState,
                                        validRemote ? static_cast<uint64_t>(remote.timestamp) : 0, localSameRemote,
                                        hasDeferred, hasDeferred ? deferred.timestamp : 0, localSameDeferred,
                                        remoteSameDeferred});
  LOG_DBG("BOAuto", "Sync: local=%s%.4f@%llu remote=%s%.4f@%lld deferred=%s%.4f@%llu action=%u",
          hasLocal ? "" : "none ", localPercentage, static_cast<unsigned long long>(localTimestamp),
          remoteState == RemoteState::Found ? "" : "none ", validRemote ? remote.percentage : 0.0f,
          static_cast<long long>(remote.timestamp), hasDeferred ? "" : "none ",
          hasDeferred ? deferred.percentage : 0.0f,
          static_cast<unsigned long long>(hasDeferred ? deferred.timestamp : 0), static_cast<unsigned>(action));

  if (action == SyncAction::PullRemote) {
    const bool staged = ProgressSyncDeferred::stage(remoteCandidate);
    drainStats(session, epubPath, after);
    disconnectWifi();
    if (!staged) return finish(Outcome::Retry);
    return finish(Outcome::Confirmed);
  }

  if (action == SyncAction::HoldUnknown || action == SyncAction::Abort) {
    drainStats(session, epubPath, after);
    disconnectWifi();
    return finish(Outcome::NoRetry);
  }

  if (action != SyncAction::PushLocal) {
    drainStats(session, epubPath, after);
    disconnectWifi();
    return finish(Outcome::Confirmed);
  }

  if (renderer) {
    if (auto* fcm = renderer->getFontCacheManager()) fcm->forceReleaseTransientCaches();
  }

  BookOrbitProgress toPush;
  toPush.document = hash;
  toPush.progress = saved.xpath;
  toPush.percentage = saved.percentage;
  toPush.device = SETTINGS.getEffectiveDeviceName();
  toPush.deviceId = BookOrbitStats::statsDeviceId();
  toPush.timestamp = static_cast<int64_t>(localTimestamp);

  LOG_DBG("BOAuto", "Push: sending document=%s device=%s percentage=%.4f xpath=%s", toPush.document.c_str(),
          toPush.device.c_str(), toPush.percentage, toPush.progress.c_str());

  const BookOrbitSyncClient::Error putResult = session.updateProgressAndVerify(toPush, userRequestedAbort);
  LOG_DBG("BOAuto", "Push result: %s (http=%d, local=%.4f)", BookOrbitSyncClient::errorString(putResult),
          session.lastHttpCode(), localPercentage);
  const auto outcome = finish(ProgressSyncRetry::classify<BookOrbitSyncClient>(putResult, session.lastHttpCode()));
  if (putResult == BookOrbitSyncClient::OK) drainStats(session, epubPath, after);
  disconnectWifi();
  return outcome;
}

}  // namespace

SyncNowResult syncNow(const std::string& epubPath, GfxRenderer& renderer, std::string& errorDetail) {
  errorDetail.clear();
  if (!BOOKORBIT_STORE.hasCredentials()) {
    LOG_DBG("BOAuto", "SyncNow: gated out -- no BookOrbit credentials configured");
    return SyncNowResult::NO_CREDENTIALS;
  }
  if (epubPath.empty() || !FsHelpers::hasEpubExtension(epubPath)) {
    LOG_DBG("BOAuto", "SyncNow: gated out -- no open EPUB (path='%s')", epubPath.c_str());
    return SyncNowResult::NO_OPEN_BOOK;
  }

  const std::string hash = computeDocumentHash(epubPath);
  if (hash.empty()) {
    LOG_DBG("BOAuto", "SyncNow: gated out -- empty document hash");
    return SyncNowResult::NO_OPEN_BOOK;
  }

  // Consume any earlier headless handoff before calculating the local side. There is no provider
  // session yet, so Section/layout work cannot overlap retained TLS buffers.
  if (!ProgressSyncNetwork::active()) ProgressSyncDeferred::applyForBook(epubPath, renderer);
  if (auto* fcm = renderer.getFontCacheManager()) fcm->forceReleaseTransientCaches();

  // Prepare and release the local EPUB before connecting. This makes manual sync use the same
  // validated timestamp ordering as automatic sync and preserves heap for TLS.
  auto epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
  if (!loadEpubForSync(*epub, /*buildIfMissing=*/true)) {
    LOG_DBG("BOAuto", "SyncNow: could not load/build epub, giving up");
    return SyncNowResult::LOCAL_LOAD_FAILED;
  }

  int spineIndex = 0;
  int pageNumber = 0;
  int pageCount = 0;
  uint64_t localTimestamp = 0;
  bool localLegacy = false;
  bool hasLocal = EpubReaderUtils::loadProgress(epub->getCachePath(), spineIndex, pageNumber, pageCount, localTimestamp,
                                                localLegacy);
  hasLocal = hasLocal && spineIndex >= 0 && spineIndex < epub->getSpineItemsCount() && pageNumber >= 0 &&
             pageCount > 0 && pageNumber < pageCount;
  float localPercentage = 0.0f;
  SavedProgressPosition saved;
  if (hasLocal) {
    localPercentage = epub->calculateProgress(spineIndex, static_cast<float>(pageNumber) / pageCount);
    saved = ProgressSyncPosition::resolve(epub, CrossPointPosition{spineIndex, pageNumber, pageCount}, hash, &renderer);
    if (!std::isfinite(localPercentage) || localPercentage < 0.0f || localPercentage > 1.0f || saved.xpath.empty()) {
      hasLocal = false;
      localPercentage = 0.0f;
      saved = {};
    }
  }
  epub.reset();
  if (auto* fcm = renderer.getFontCacheManager()) fcm->forceReleaseTransientCaches();

  if (!connectToSavedWifi()) {
    LOG_DBG("BOAuto", "SyncNow: WiFi connect failed or timed out, giving up");
    disconnectWifi();
    return SyncNowResult::NO_WIFI;
  }
  LOG_DBG("BOAuto", "SyncNow: WiFi connected (%s)", WiFi.SSID().c_str());

  SyncNowResult result = SyncNowResult::ALREADY_SYNCED;
  bool applyDeferred = false;
  {
    BookOrbitSyncClient::Session session;
    BookOrbitSyncClient::Capabilities caps;
    const BookOrbitSyncClient::Error capsResult =
        BookOrbitStats::ensureCapabilitiesConfirmed(session, caps, userRequestedAbort);
    if (capsResult != BookOrbitSyncClient::OK) {
      LOG_DBG("BOAuto", "SyncNow: gated out -- server not yet confirmed BookOrbit");
      disconnectWifi();
      errorDetail = BookOrbitSyncClient::errorString(capsResult);
      return SyncNowResult::NETWORK_ERROR;
    }

    BookOrbitProgress remote;
    const BookOrbitSyncClient::Error getResult = session.getProgress(hash, remote, userRequestedAbort);
    if (getResult != BookOrbitSyncClient::OK && getResult != BookOrbitSyncClient::NOT_FOUND) {
      LOG_DBG("BOAuto", "SyncNow: remote fetch failed (%d), giving up", getResult);
      disconnectWifi();
      errorDetail = BookOrbitSyncClient::errorString(getResult);
      return SyncNowResult::NETWORK_ERROR;
    }
    const bool validRemote = getResult == BookOrbitSyncClient::OK && std::isfinite(remote.percentage) &&
                             remote.percentage >= 0.0f && remote.percentage <= 1.0f && !remote.progress.empty();
    if (getResult == BookOrbitSyncClient::OK && !validRemote) {
      disconnectWifi();
      errorDetail = BookOrbitSyncClient::errorString(BookOrbitSyncClient::JSON_ERROR);
      return SyncNowResult::NETWORK_ERROR;
    }

    if (validRemote) {
      BookOrbitSyncClient::Error resetError = BookOrbitSyncClient::OK;
      bool pushedAfterReset = false;
      const StandingResetResult resetResult = handleStandingReset(session, remote, epubPath, hash, hasLocal,
                                                                  localTimestamp, saved, resetError, pushedAfterReset);
      if (resetResult != StandingResetResult::NotApplicable) {
        if (resetResult == StandingResetResult::RequestFailed) {
          disconnectWifi();
          errorDetail = BookOrbitSyncClient::errorString(resetError);
          return SyncNowResult::NETWORK_ERROR;
        }
        if (resetResult == StandingResetResult::PersistenceFailed) {
          disconnectWifi();
          return SyncNowResult::LOCAL_LOAD_FAILED;
        }
        if (resetResult == StandingResetResult::HoldUnknown) {
          disconnectWifi();
          errorDetail = "Progress timestamp unavailable; local position was preserved";
          return SyncNowResult::CHRONOLOGY_UNAVAILABLE;
        }
        ProgressSyncRetry::confirm(ProgressSyncRetry::Provider::BookOrbit, epubPath);
        drainStats(session, epubPath, AfterProgress::Full);
        disconnectWifi();
        KOReaderAutoSync::recordProviderSuccess(true);
        return pushedAfterReset ? SyncNowResult::PUSHED : SyncNowResult::ALREADY_SYNCED;
      }
    }

    DeferredProgress deferred;
    const bool hasDeferred = ProgressSyncDeferred::pendingForBook(epubPath, deferred);
    const auto remoteState = getResult == BookOrbitSyncClient::NOT_FOUND ? ProgressPullPolicy::RemoteState::Missing
                                                                         : ProgressPullPolicy::RemoteState::Found;
    DeferredProgress remoteCandidate;
    if (validRemote) {
      remoteCandidate.provider = static_cast<uint8_t>(ProgressSyncRetry::Provider::BookOrbit);
      remoteCandidate.path = epubPath;
      remoteCandidate.document = hash;
      remoteCandidate.xpath = remote.progress;
      remoteCandidate.percentage = remote.percentage;
      remoteCandidate.timestamp = static_cast<uint64_t>(remote.timestamp);
      remoteCandidate.bookOrbitReset = remote.isWebReset();
    }
    const auto action =
        hasLocal && localLegacy
            ? ProgressPullPolicy::decideLegacy(remoteState, hasDeferred)
            : ProgressPullPolicy::decide(
                  {hasLocal, localTimestamp, remoteState, validRemote ? static_cast<uint64_t>(remote.timestamp) : 0,
                   hasLocal && validRemote && saved.xpath == remote.progress, hasDeferred,
                   hasDeferred ? deferred.timestamp : 0,
                   hasLocal && hasDeferred && !deferred.xpath.empty() && saved.xpath == deferred.xpath,
                   validRemote && hasDeferred && deferred.samePositionAs(remoteCandidate)});

    if (action == ProgressPullPolicy::SyncAction::PullRemote) {
      if (!validRemote || !ProgressSyncDeferred::stage(remoteCandidate)) {
        disconnectWifi();
        return SyncNowResult::LOCAL_LOAD_FAILED;
      }
      result = SyncNowResult::PULLED;
      applyDeferred = true;
    } else if (action == ProgressPullPolicy::SyncAction::PushLocal) {
      BookOrbitProgress toPush;
      toPush.document = hash;
      toPush.progress = saved.xpath;
      toPush.percentage = saved.percentage;
      toPush.device = SETTINGS.getEffectiveDeviceName();
      toPush.deviceId = BookOrbitStats::statsDeviceId();
      toPush.timestamp = static_cast<int64_t>(localTimestamp);
      const BookOrbitSyncClient::Error putResult = session.updateProgressAndVerify(toPush, userRequestedAbort);
      if (putResult != BookOrbitSyncClient::OK) {
        disconnectWifi();
        errorDetail = BookOrbitSyncClient::errorString(putResult);
        return SyncNowResult::NETWORK_ERROR;
      }
      result = SyncNowResult::PUSHED;
    } else if (action == ProgressPullPolicy::SyncAction::HoldDeferred) {
      applyDeferred = true;
    } else if (action == ProgressPullPolicy::SyncAction::HoldUnknown ||
               action == ProgressPullPolicy::SyncAction::Abort) {
      disconnectWifi();
      errorDetail = "Progress timestamp unavailable; local position was preserved";
      return SyncNowResult::CHRONOLOGY_UNAVAILABLE;
    }

    ProgressSyncRetry::confirm(ProgressSyncRetry::Provider::BookOrbit, epubPath);
    drainStats(session, epubPath, AfterProgress::Full);
    disconnectWifi();
  }

  // The scoped Session (including its SecureHttpClient transport) is destroyed before mapping.
  if (applyDeferred && !ProgressSyncDeferred::applyForBook(epubPath, renderer)) {
    DeferredProgress pending;
    if (!ProgressSyncDeferred::pendingForBook(epubPath, pending)) return SyncNowResult::LOCAL_LOAD_FAILED;
  }
  if (applyDeferred) result = SyncNowResult::PULLED;
  KOReaderAutoSync::recordProviderSuccess(true);
  return result;
}

void syncOnSleep(GfxRenderer& renderer) {
  if (!APP_STATE.lastSleepFromReader) {
    LOG_DBG("BOAuto", "Sync: gated out -- last sleep wasn't from the reader");
    return;
  }
  const std::string path = APP_STATE.openEpubPath;
  if (synchronizeProgress(path, &renderer, /*buildIfMissing=*/false, AfterProgress::Full) ==
      ProgressSyncRetry::Outcome::Confirmed) {
    KOReaderAutoSync::recordProviderSuccess(false);
  }
  // synchronizeProgress's Session and TLS transport have been destroyed before layout work.
  if (!ProgressSyncNetwork::active()) ProgressSyncDeferred::applyForBook(path, renderer);
}

void syncHeadlessly(const std::string& epubPath) {
  if (synchronizeProgress(epubPath, nullptr, /*buildIfMissing=*/false, AfterProgress::StatsOnly) ==
      ProgressSyncRetry::Outcome::Confirmed) {
    KOReaderAutoSync::recordProviderSuccess(false);
  }
}

void syncOnClose(GfxRenderer& renderer) {
  const std::string path = APP_STATE.openEpubPath;
  if (synchronizeProgress(path, &renderer, /*buildIfMissing=*/false, AfterProgress::Full) ==
      ProgressSyncRetry::Outcome::Confirmed) {
    KOReaderAutoSync::recordProviderSuccess(true);
  }
  if (!ProgressSyncNetwork::active()) ProgressSyncDeferred::applyForBook(path, renderer);
}

void syncOnOpen(const std::string& epubPath, GfxRenderer& renderer) {
  if (synchronizeProgress(epubPath, &renderer, /*buildIfMissing=*/true, AfterProgress::Full) ==
      ProgressSyncRetry::Outcome::Confirmed) {
    KOReaderAutoSync::recordProviderSuccess(true);
  }
  if (!ProgressSyncNetwork::active()) ProgressSyncDeferred::applyForBook(epubPath, renderer);
}

}  // namespace BookOrbitAutoSync
