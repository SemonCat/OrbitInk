#include "ProgressSyncRetry.h"

#include <BookOrbitCredentialStore.h>
#include <FsHelpers.h>
#include <KOReaderCredentialStore.h>
#include <Logging.h>
#include <WifiCredentialStore.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "ProgressSyncSchedule.h"

namespace ProgressSyncRetry {
namespace {
void persistIfChanged(const State& before) {
  const auto& after = APP_STATE.progressSyncRetry;
  if (before.providers == after.providers && before.path == after.path) return;
  if (!APP_STATE.saveToFile()) LOG_ERR("SyncRetry", "Could not persist progress retry state");
}
}  // namespace

void record(Provider provider, const std::string& path, Outcome outcome) {
  const auto before = APP_STATE.progressSyncRetry;
  APP_STATE.progressSyncRetry.observe(provider, path, outcome);
  persistIfChanged(before);
}

void confirm(Provider provider, const std::string& path) {
  // A manual/open transaction for another book must not cancel pending work.
  if (APP_STATE.progressSyncRetry.path == path) record(provider, path, Outcome::Confirmed);
}

uint64_t sleepDelayMicros() {
  const auto before = APP_STATE.progressSyncRetry;
  const uint8_t enabled = ProgressSyncSchedule::enabledProviders(
      WIFI_STORE.getCredentialCount() != 0, FsHelpers::hasEpubExtension(APP_STATE.openEpubPath),
      KOREADER_STORE.hasCredentials(), KOREADER_STORE.getAutoSyncEnabled(), BOOKORBIT_STORE.hasCredentials());
  APP_STATE.progressSyncRetry.prune(APP_STATE.openEpubPath, enabled);
  persistIfChanged(before);
  return ProgressSyncSchedule::sleepDelayMicros(SETTINGS.periodicSyncInterval, enabled);
}
}  // namespace ProgressSyncRetry
