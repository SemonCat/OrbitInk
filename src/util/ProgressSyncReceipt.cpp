#include "ProgressSyncReceipt.h"

#include <Arduino.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <KOSyncRequestHelpers.h>
#include <Logging.h>

namespace ProgressSyncReceipt {
namespace {
constexpr char RECEIPT_FILE[] = "/.crosspoint/sync-receipts.log";
constexpr char RECEIPT_PREVIOUS[] = "/.crosspoint/sync-receipts.1.log";
constexpr size_t ROTATE_BYTES = 8 * 1024;

Record current;

bool append(const char* line, size_t length) {
  HalFile file = Storage.open(RECEIPT_FILE, O_WRONLY | O_CREAT | O_APPEND);
  if (!file.isOpen()) return false;
  if (file.fileSize() >= ROTATE_BYTES) {
    file.close();
    Storage.remove(RECEIPT_PREVIOUS);
    if (!Storage.rename(RECEIPT_FILE, RECEIPT_PREVIOUS)) Storage.remove(RECEIPT_FILE);
    file = Storage.open(RECEIPT_FILE, O_WRONLY | O_CREAT | O_APPEND);
    if (!file.isOpen()) return false;
  }
  const bool written = file.write(line, length) == length;
  return file.close() && written;
}
}  // namespace

void begin(const Trigger trigger) {
  current = Record{};
  current.trigger = trigger;
  KOSyncRequest::heapGateStats = {};
}

void noteOutcome(const ProgressSyncRetry::Provider provider, const ProgressSyncRetry::Outcome outcome) {
  (provider == ProgressSyncRetry::Provider::KOSync ? current.kosync : current.bookOrbit) = static_cast<int8_t>(outcome);
}

void noteWifi(const bool connected, const uint32_t elapsedMs) {
  current.wifi = connected ? Wifi::Connected : Wifi::Failed;
  current.wifiMs = elapsedMs;
}

void noteStoreFailure() { current.storeFailed = true; }

void commit(const uint64_t nextDelayMicros) {
  const auto& gate = KOSyncRequest::heapGateStats;
  current.gateChecks = gate.checks;
  current.gateRejections = gate.rejections;
  current.gateMinFree = gate.checks ? gate.minFree : 0;
  current.gateMinLargest = gate.checks ? gate.minLargest : 0;
  current.freeHeap = ESP.getFreeHeap();
  current.largestBlock = ESP.getMaxAllocHeap();
  current.minFreeHeap = ESP.getMinFreeHeap();
  current.nextDelayMicros = nextDelayMicros;
  current.usb = gpio.isUsbConnected() ? 1 : 0;
  current.cycle = HalPowerManager::retainedSleepCycles();
  current.prevArmedMicros = HalPowerManager::retainedTimerDelayMicros();
  current.hasTime = halClock.getEpochMinutes(current.epochMinutes);

  char line[256];
  const size_t length = format(current, line, sizeof(line));
  LOG_INF("SyncReceipt", "%.*s", static_cast<int>(length ? length - 1 : 0), line);
  if (length == 0 || !append(line, length)) LOG_ERR("SyncReceipt", "Could not append sync receipt");
}

}  // namespace ProgressSyncReceipt
