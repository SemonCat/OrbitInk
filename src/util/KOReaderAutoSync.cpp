#include "KOReaderAutoSync.h"

#include <HalClock.h>
#include <Logging.h>

#include <atomic>

#include "CrossPointState.h"
#include "KOSyncStatus.h"

namespace KOReaderAutoSync {

namespace {
std::atomic<bool> pendingSuccessNotice{false};
}  // namespace

bool consumeSuccessNotice() { return pendingSuccessNotice.exchange(false, std::memory_order_acq_rel); }

bool formatLastSuccessRelative(char* output, size_t outputSize) {
  uint32_t nowMinutes = 0;
  if (!halClock.getEpochMinutes(nowMinutes)) return false;
  return KOSyncStatus::formatRelative(nowMinutes, APP_STATE.lastKoSyncEpochMinutes, output, outputSize);
}

void recordProviderSuccess(bool queueNotice) {
  uint32_t nowMinutes = 0;
  if (halClock.getEpochMinutes(nowMinutes)) {
    APP_STATE.lastKoSyncEpochMinutes = nowMinutes;
    if (!APP_STATE.saveToFile()) {
      LOG_ERR("KOAuto", "Could not persist last successful sync time");
    }
  }
  if (queueNotice) pendingSuccessNotice.store(true, std::memory_order_release);
}

}  // namespace KOReaderAutoSync
