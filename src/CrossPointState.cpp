#include "CrossPointState.h"

#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <ProgressTimestamp.h>

bool CrossPointState::isRecentSleep(uint16_t idx, uint8_t checkCount) const {
  const uint8_t effectiveCount = std::min(checkCount, recentSleepFill);
  for (uint8_t i = 0; i < effectiveCount; i++) {
    const uint8_t slot = (recentSleepPos + SLEEP_RECENT_COUNT - 1 - i) % SLEEP_RECENT_COUNT;
    if (recentSleepImages[slot] == idx) return true;
  }
  return false;
}

void CrossPointState::pushRecentSleep(uint16_t idx) {
  recentSleepImages[recentSleepPos] = idx;
  recentSleepPos = (recentSleepPos + 1) % SLEEP_RECENT_COUNT;
  if (recentSleepFill < SLEEP_RECENT_COUNT) recentSleepFill++;
}

void CrossPointState::toJson(JsonDocument& doc) const {
  doc["openEpubPath"] = openEpubPath;
  doc["progressRetryPath"] = progressSyncRetry.path;
  doc["progressRetryProviders"] = progressSyncRetry.providers;
  if (deferredProgress.valid()) {
    doc["deferredProgressProvider"] = deferredProgress.provider;
    doc["deferredProgressPath"] = deferredProgress.path;
    doc["deferredProgressDocument"] = deferredProgress.document;
    doc["deferredProgressXpath"] = deferredProgress.xpath;
    doc["deferredProgressPercentage"] = deferredProgress.percentage;
    doc["deferredProgressTimestamp"] = deferredProgress.timestamp;
    doc["deferredProgressBookOrbitReset"] = deferredProgress.bookOrbitReset;
    doc["deferredProgressHasRich"] = deferredProgress.hasRichPosition;
    if (deferredProgress.hasRichPosition) {
      doc["deferredProgressRichPctQ"] = deferredProgress.richPctQ;
      doc["deferredProgressRichSpine"] = deferredProgress.richSpineIndex;
      doc["deferredProgressRichPage"] = deferredProgress.richPageNumber;
      doc["deferredProgressRichTotal"] = deferredProgress.richTotalPages;
      doc["deferredProgressRichHasParagraph"] = deferredProgress.richHasParagraphIndex;
      if (deferredProgress.richHasParagraphIndex) {
        doc["deferredProgressRichParagraph"] = deferredProgress.richParagraphIndex;
      }
    }
  }
  if (!bookOrbitResetPath.empty() && bookOrbitResetDocument.size() == 32 &&
      ProgressTimestamp::valid(bookOrbitResetTimestamp)) {
    doc["bookOrbitResetPath"] = bookOrbitResetPath;
    doc["bookOrbitResetDocument"] = bookOrbitResetDocument;
    doc["bookOrbitResetTimestamp"] = bookOrbitResetTimestamp;
    doc["bookOrbitResetAcknowledged"] = bookOrbitResetAcknowledged;
  }
  JsonArray recentArr = doc["recentSleepImages"].to<JsonArray>();
  for (int i = 0; i < SLEEP_RECENT_COUNT; i++) recentArr.add(recentSleepImages[i]);
  doc["recentSleepPos"] = recentSleepPos;
  doc["recentSleepFill"] = recentSleepFill;
  doc["readerActivityLoadCount"] = readerActivityLoadCount;
  doc["lastSleepFromReader"] = lastSleepFromReader;
  doc["showBootScreen"] = showBootScreen;
  doc["quickResumeSkipRenderEligible"] = quickResumeSkipRenderEligible;
  doc["lastKoSyncEpochMinutes"] = lastKoSyncEpochMinutes;
  doc["koStatsServerId"] = koStatsServerId;
  doc["koStatsServerKind"] = koStatsServerKind;
  doc["koStatsCapBookmarkSync"] = koStatsCapBookmarkSync;
  doc["koStatsCapCatalogBulkManifest"] = koStatsCapCatalogBulkManifest;
  doc["koStatsCapCatalogDashboardSections"] = koStatsCapCatalogDashboardSections;
  doc["koStatsDeviceId"] = koStatsDeviceId;
  doc["boMigrationAttempted"] = boMigrationAttempted;
  doc["boCredentialsMigrated"] = boCredentialsMigrated;
  doc["boMigrationTargetOwner"] = boMigrationTargetOwner;
}

bool CrossPointState::fromJson(JsonVariantConst doc) {
  openEpubPath = doc["openEpubPath"] | "";
  progressSyncRetry.path = doc["progressRetryPath"] | "";
  progressSyncRetry.providers = doc["progressRetryProviders"] | static_cast<uint8_t>(0);
  progressSyncRetry.prune(openEpubPath, 3);
  deferredProgress.clear();
  const uint64_t deferredProvider = doc["deferredProgressProvider"] | static_cast<uint64_t>(0);
  const double deferredPercentage = doc["deferredProgressPercentage"] | -1.0;
  const uint64_t deferredTimestamp = doc["deferredProgressTimestamp"] | static_cast<uint64_t>(0);
  const uint64_t richPctQ = doc["deferredProgressRichPctQ"] | static_cast<uint64_t>(0);
  const uint64_t richSpine = doc["deferredProgressRichSpine"] | static_cast<uint64_t>(0);
  const uint64_t richPage = doc["deferredProgressRichPage"] | static_cast<uint64_t>(0);
  const uint64_t richTotal = doc["deferredProgressRichTotal"] | static_cast<uint64_t>(1);
  const uint64_t richParagraph = doc["deferredProgressRichParagraph"] | static_cast<uint64_t>(0);
  if (deferredProvider <= UINT8_MAX && std::isfinite(deferredPercentage) && richPctQ <= UINT32_MAX &&
      richSpine <= UINT16_MAX && richPage <= UINT16_MAX && richTotal <= UINT16_MAX && richParagraph <= UINT16_MAX) {
    deferredProgress.provider = static_cast<uint8_t>(deferredProvider);
    deferredProgress.path = doc["deferredProgressPath"] | "";
    deferredProgress.document = doc["deferredProgressDocument"] | "";
    deferredProgress.xpath = doc["deferredProgressXpath"] | "";
    deferredProgress.percentage = static_cast<float>(deferredPercentage);
    deferredProgress.timestamp = deferredTimestamp;
    deferredProgress.bookOrbitReset = doc["deferredProgressBookOrbitReset"] | false;
    deferredProgress.hasRichPosition = doc["deferredProgressHasRich"] | false;
    deferredProgress.richPctQ = static_cast<uint32_t>(richPctQ);
    deferredProgress.richSpineIndex = static_cast<uint16_t>(richSpine);
    deferredProgress.richPageNumber = static_cast<uint16_t>(richPage);
    deferredProgress.richTotalPages = static_cast<uint16_t>(richTotal);
    deferredProgress.richHasParagraphIndex = doc["deferredProgressRichHasParagraph"] | false;
    deferredProgress.richParagraphIndex = static_cast<uint16_t>(richParagraph);
    if (!deferredProgress.valid()) deferredProgress.clear();
  }
  bookOrbitResetPath = doc["bookOrbitResetPath"] | "";
  bookOrbitResetDocument = doc["bookOrbitResetDocument"] | "";
  bookOrbitResetTimestamp = doc["bookOrbitResetTimestamp"] | static_cast<uint64_t>(0);
  bookOrbitResetAcknowledged = doc["bookOrbitResetAcknowledged"] | false;
  if (bookOrbitResetPath.empty() || bookOrbitResetDocument.size() != 32 ||
      !ProgressTimestamp::valid(bookOrbitResetTimestamp)) {
    bookOrbitResetPath.clear();
    bookOrbitResetDocument.clear();
    bookOrbitResetTimestamp = 0;
    bookOrbitResetAcknowledged = false;
  }
  memset(recentSleepImages, 0, sizeof(recentSleepImages));
  JsonArrayConst recentArr = doc["recentSleepImages"];
  const int actualCount =
      recentArr.isNull() ? 0 : std::min(static_cast<int>(recentArr.size()), static_cast<int>(SLEEP_RECENT_COUNT));
  for (int i = 0; i < actualCount; i++) recentSleepImages[i] = recentArr[i] | static_cast<uint16_t>(0);
  recentSleepPos = doc["recentSleepPos"] | static_cast<uint8_t>(0);
  if (recentSleepPos >= SLEEP_RECENT_COUNT) recentSleepPos = actualCount > 0 ? recentSleepPos % SLEEP_RECENT_COUNT : 0;
  recentSleepFill = doc["recentSleepFill"] | static_cast<uint8_t>(0);
  recentSleepFill = static_cast<uint8_t>(std::min(static_cast<int>(recentSleepFill), actualCount));
  // Migrate legacy single-image field from old state.json (pre-recency-buffer).
  // Only seeds the buffer if the new buffer is empty (fresh migration, not a resave).
  if (recentSleepFill == 0 && !doc["lastSleepImage"].isNull()) {
    const uint8_t legacy = doc["lastSleepImage"] | static_cast<uint8_t>(UINT8_MAX);
    if (legacy != UINT8_MAX) pushRecentSleep(static_cast<uint16_t>(legacy));
  }
  readerActivityLoadCount = doc["readerActivityLoadCount"] | static_cast<uint8_t>(0);
  lastSleepFromReader = doc["lastSleepFromReader"] | false;
  showBootScreen = doc["showBootScreen"] | true;
  quickResumeSkipRenderEligible = doc["quickResumeSkipRenderEligible"] | false;
  lastKoSyncEpochMinutes = doc["lastKoSyncEpochMinutes"] | static_cast<uint32_t>(0);
  // A pre-release state.json carrying the old 32-bit koStatsServerKey simply has no
  // koStatsServerId, so detection resets to UNKNOWN and re-probes -- the safe direction.
  koStatsServerId = doc["koStatsServerId"] | "";
  koStatsServerKind = doc["koStatsServerKind"] | static_cast<uint8_t>(0);
  // A pre-migration state.json can carry the old ServerKind::OTHER value (2); that value is no
  // longer meaningful (see the field's doc comment in CrossPointState.h) and must not be read
  // back as "confirmed BookOrbit" (1) -- collapse anything but exactly 1 to unconfirmed.
  if (koStatsServerKind != 1) koStatsServerKind = 0;
  koStatsCapBookmarkSync = doc["koStatsCapBookmarkSync"] | false;
  koStatsCapCatalogBulkManifest = doc["koStatsCapCatalogBulkManifest"] | false;
  koStatsCapCatalogDashboardSections = doc["koStatsCapCatalogDashboardSections"] | false;
  koStatsDeviceId = doc["koStatsDeviceId"] | "";
  boMigrationAttempted = doc["boMigrationAttempted"] | false;
  boCredentialsMigrated = doc["boCredentialsMigrated"] | false;
  boMigrationTargetOwner = doc["boMigrationTargetOwner"] | "";
  return true;
}
