#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>

#include "util/ProgressSyncRetry.h"
#include "util/DeferredProgress.h"

class CrossPointState : public PersistableStore<CrossPointState> {
  CrossPointState() = default;

  friend class PersistableStore<CrossPointState>;

 public:
  static constexpr uint8_t SLEEP_RECENT_COUNT = 16;

  std::string openEpubPath;
  ProgressSyncRetry::State progressSyncRetry;
  // Raw remote position fetched during a display-less timer/button wake. It is mapped only after
  // renderer/fonts/layout are initialized, and suppresses stale local pushes by origin timestamp
  // until successfully applied.
  DeferredProgress deferredProgress;
  // BookOrbit's web-reset GET is a standing synthetic start record with a fresh timestamp on
  // every read. After that start is durably applied, this marker drives exactly one same-device
  // start acknowledgement and prevents a later synthetic GET from erasing the reader's first
  // post-reset advance. Cleared after a successful non-start PUT or a normal remote response.
  std::string bookOrbitResetPath;
  std::string bookOrbitResetDocument;
  uint64_t bookOrbitResetTimestamp = 0;
  bool bookOrbitResetAcknowledged = false;
  uint16_t recentSleepImages[SLEEP_RECENT_COUNT] = {};  // circular buffer of recent wallpaper indices
  uint8_t recentSleepPos = 0;                           // next write slot
  uint8_t recentSleepFill = 0;                          // valid entries (0..SLEEP_RECENT_COUNT)
  uint8_t readerActivityLoadCount = 0;
  bool lastSleepFromReader = false;
  bool showBootScreen = true;
  // Set only when entering a quick-resume sleep from the reader's own clean base view (no menu,
  // popup, or footnote overlay on screen) -- see main.cpp's enterDeepSleep(). Consumed once at the
  // next QuickResume boot to additionally gate skipping the initial page render; a stale/false
  // value here only ever falls back to the existing full render, never skips one incorrectly.
  bool quickResumeSkipRenderEligible = false;
  // Despite the name (kept to avoid renaming a persisted JSON key), this now means "last
  // successful sync of any kind from any configured provider" -- KOReaderAutoSync's own
  // KOSync push/pull AND BookOrbitAutoSync's independent BookOrbit progress push/pull all update
  // it via KOReaderAutoSync::recordProviderSuccess(). A BookOrbit-only device's Home-screen
  // "last synced" readout is accurate under this broadened meaning; it just cannot say which
  // provider produced the most recent success.
  uint32_t lastKoSyncEpochMinutes = 0;
  // Cached BookOrbit capability probe (see src/util/BookOrbitStatsSync.h). koStatsServerId is the
  // 32-character hex form of the 128-bit owner fingerprint of the EXPLICITLY configured
  // BookOrbitCredentialStore base URL + account, so changing server or account invalidates the
  // answer automatically. koStatsServerKind is 0 (unconfirmed) or 1 (confirmed BookOrbit) --
  // unlike e51b383 there is no longer a permanent "2 = settled not-BookOrbit" value: since the
  // server is now explicitly configured by the user rather than guessed, a failed probe is a
  // configuration problem to surface (BookOrbitAuthActivity's test-connection), not a fact to
  // cache forever, so it is simply retried on the next sync-eligible moment rather than disabling
  // the feature. Field names kept from e51b383 rather than renamed: BookOrbitMigration.cpp reads
  // the pre-migration value of this same field (then keyed by the legacy KOReaderCredentialStore
  // owner) to recover the old server+account identity, then this field naturally starts meaning
  // "BookOrbitCredentialStore's owner" going forward with no separate legacy field to carry.
  // Not credential material: the fingerprint covers only the base URL and username, never the
  // password or its MD5.
  std::string koStatsServerId;
  uint8_t koStatsServerKind = 0;
  // Capability flags confirmed alongside koStatsServerKind == 1 (see
  // BookOrbitSyncClient::Capabilities). Meaningless when koStatsServerKind != 1.
  bool koStatsCapBookmarkSync = false;
  bool koStatsCapCatalogBulkManifest = false;
  bool koStatsCapCatalogDashboardSections = false;
  // This installation's BookOrbit identifier (see BookOrbitStats::statsDeviceId()). Originally
  // introduced for page stats, then reused by BookOrbit progress, bookmarks, and catalog calls
  // whose server-side state is per-device. Randomly generated and persisted before first use, so a
  // state reset/reinstall cannot inherit acknowledgements made by the prior local installation.
  // Legacy cycle-1 hardware-derived/shared values are replaced on first use. Distinct from
  // BookOrbitLimits::DEVICE_ID, retained only for the sibling generic-KOSync progress client.
  std::string koStatsDeviceId;
  // One-shot latch for BookOrbitMigration's legacy-detection upgrade (see
  // src/util/BookOrbitMigration.h). Set once the check completes, deliberately no-ops, or safely
  // abandons an ambiguous account; left false after retryable persistence/directory failures.
  // Once set, a user who explicitly clears BookOrbit credentials after migrating cannot have them
  // silently repopulated from the still-present legacy KOSync detection on the next boot.
  bool boMigrationAttempted = false;
  // Two-phase-commit marker for the SAME migration attempt (set durably right after the
  // credential copy, before the queue-owner reconciliation loop that follows it). Deliberately
  // NOT the same signal as BookOrbitCredentialStore::hasCredentials(): that would be ambiguous
  // between "this migration copied these credentials" and "the user independently configured an
  // unrelated BookOrbit account" -- the latter must never have its queues reassigned to the
  // legacy owner's fingerprint. Meaningless once boMigrationAttempted is true.
  bool boCredentialsMigrated = false;
  // Hex BookOrbitOwnerId of the account this in-progress migration is FOR, captured the moment
  // boCredentialsMigrated is set. Binds the migration to the exact account it started against: if
  // the user edits BookOrbitCredentialStore's credentials before an interrupted migration resumes
  // (e.g. switching to an unrelated self-hosted account between boots), BookOrbitMigration.cpp
  // recomputes the CURRENT owner and compares it against this field before touching the legacy
  // queue -- a mismatch means the migration is abandoned safely rather than re-owning a stranger
  // account's queue to the legacy fingerprint. Meaningless once boMigrationAttempted is true.
  std::string boMigrationTargetOwner;

  static const char* getFilePath() { return "/.crosspoint/state.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Returns true if idx was shown within the last checkCount picks.
  // Walks backwards from the most recently written slot.
  bool isRecentSleep(uint16_t idx, uint8_t checkCount) const;

  void pushRecentSleep(uint16_t idx);
};

// Helper macro to access state
#define APP_STATE CrossPointState::getInstance()
