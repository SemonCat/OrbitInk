#include "HalPowerManager.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <PowerManager.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <soc/soc_caps.h>

#include <cassert>

#include "HalGPIO.h"

HalPowerManager powerManager;  // Singleton instance

namespace {
// RTC_DATA_ATTR is reinitialized (to 0) by every boot except a deep-sleep wake.
RTC_DATA_ATTR uint64_t rtcArmedTimerDelayMicros = 0;
RTC_DATA_ATTR uint32_t rtcSleepCycles = 0;
}  // namespace

uint64_t HalPowerManager::retainedTimerDelayMicros() { return rtcArmedTimerDelayMicros; }
uint32_t HalPowerManager::retainedSleepCycles() { return rtcSleepCycles; }

void HalPowerManager::begin() {
  if (BoardConfig::ACTIVE.batteryAdc >= 0) {
    pinMode(BoardConfig::ACTIVE.batteryAdc, INPUT);
  }
  normalFreq = getCpuFrequencyMhz();
  modeMutex = xSemaphoreCreateMutex();
  assert(modeMutex != nullptr);
}

void HalPowerManager::setPowerSaving(bool enabled) {
  if (normalFreq <= 0 || !modeMutex) return;
  xSemaphoreTake(modeMutex, portMAX_DELAY);
  updateFrequency(enabled);
  xSemaphoreGive(modeMutex);
}

void HalPowerManager::updateFrequency(bool idle) {
  bool protectedWork = false;
  for (auto* lock = locks; lock; lock = lock->next) {
    if (busyDepth == 0 || lock->owner != busyTask) protectedWork = true;
  }
  const bool enabled = (idle || busyDepth > 0) && !protectedWork && WiFi.getMode() == WIFI_MODE_NULL;
  if (enabled && !isLowPower) {
    LOG_DBG("PWR", "Going to low-power mode");
    if (!setCpuFrequencyMhz(LOW_POWER_FREQ)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", LOW_POWER_FREQ);
      return;
    }
    isLowPower = true;

  } else if (!enabled && isLowPower) {
    LOG_DBG("PWR", "Restoring normal CPU frequency");
    if (!setCpuFrequencyMhz(normalFreq)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", normalFreq);
      return;
    }
    isLowPower = false;
  }

  // Otherwise, no change needed
}

void HalPowerManager::beginDisplayBusyWait() {
  auto& pm = powerManager;
  if (!pm.modeMutex) return;
  xSemaphoreTake(pm.modeMutex, portMAX_DELAY);
  if (pm.busyDepth++ == 0) pm.busyTask = xTaskGetCurrentTaskHandle();
  pm.updateFrequency(false);
  xSemaphoreGive(pm.modeMutex);
}

void HalPowerManager::endDisplayBusyWait() {
  auto& pm = powerManager;
  if (!pm.modeMutex) return;
  xSemaphoreTake(pm.modeMutex, portMAX_DELAY);
  if (pm.busyDepth > 0 && --pm.busyDepth == 0) pm.busyTask = nullptr;
  pm.updateFrequency(false);
  xSemaphoreGive(pm.modeMutex);
}

void HalPowerManager::startDeepSleep(HalGPIO& gpio, uint64_t retryDelayMicros) const {
#ifdef ENABLE_SERIAL_LOG
  // Tear down HWCDC so the host sees a clean disconnect and the peripheral
  // doesn't hold power domains that interfere with USB-powered GPIO wake.
  // logSerial is the raw HWCDC reference; Serial is the MySerialImpl proxy
  // (which doesn't expose end()).
  logSerial.end();
#endif

#if !SOC_PM_SUPPORT_EXT1_WAKEUP
  if (gpio.isXteinkDevice() && !gpio.deviceIsX3()) {
    // X4 GPIO13 is connected to the battery latch MOSFET. Keeping it low powers
    // the MCU off on battery, while the SDK wake source still handles USB power.
    constexpr gpio_num_t GPIO_SPIWP = GPIO_NUM_13;
    gpio_set_direction(GPIO_SPIWP, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_SPIWP, 0);
    gpio_hold_en(GPIO_SPIWP);
  }
#endif

  // Cut the gated peripheral rails (touch/SD/EPD on boards like the Sticky) and
  // hold the enables off through deep sleep — otherwise the GT911 and SD card
  // stay powered all through "off" and drain the battery. No-op on boards with
  // no switched rails (X4/X3). Trade-off: no touch-to-wake; wake is the power
  // button. Must run after display.deepSleep() so the panel controller gets its
  // deep-sleep command while its rail is still up (enterDeepSleep() in main.cpp
  // guarantees that ordering).
  freeink::PowerManager::powerDownRailsForSleep();

  // The SDK adds button wake without clearing other wake sources. Explicitly
  // remove any old timer when all progress has been confirmed. X4 cuts MCU power
  // on battery, so this retry mechanism is enabled only on the X3.
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
  rtcArmedTimerDelayMicros = 0;
  if (gpio.deviceIsX3() && retryDelayMicros != 0 && esp_sleep_enable_timer_wakeup(retryDelayMicros) == ESP_OK) {
    rtcArmedTimerDelayMicros = retryDelayMicros;
  }
  rtcSleepCycles++;

  // Waits for the power button to be physically released (so holding it doesn't
  // immediately wake the device again), then arms the wake source and sleeps.
  freeink::PowerManager::deepSleepUntilPowerButton();
}

uint16_t HalPowerManager::getBatteryPercentage() const {
  static const BatteryMonitor battery;
  if (BoardConfig::ACTIVE.batteryGauge.gaugeAddr != 0) {
    const unsigned long now = millis();
    if (_batteryLastPollMs != 0 && (now - _batteryLastPollMs) < BATTERY_POLL_MS) {
      return _batteryCachedPercent;
    }

    _batteryLastPollMs = now;
    uint16_t percent = 0;
    if (!battery.readPercentageChecked(percent)) {
      return _batteryCachedPercent;
    }
    _batteryCachedPercent = percent;
    return _batteryCachedPercent;
  }

  // smooth the battery %.
  if (_batteryCachedPercent == 0) {
    _batteryCachedPercent = 10 * battery.readPercentage();
  } else {
    _batteryCachedPercent = (_batteryCachedPercent * 9 + battery.readPercentage() * 10) / 10;
  }
  return _batteryCachedPercent / 10;
}

HalPowerManager::Lock::Lock() {
  owner = xTaskGetCurrentTaskHandle();
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  next = powerManager.locks;
  powerManager.locks = this;
  powerManager.updateFrequency(false);
  xSemaphoreGive(powerManager.modeMutex);
}

HalPowerManager::Lock::~Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  auto** link = &powerManager.locks;
  while (*link && *link != this) link = &(*link)->next;
  if (*link) *link = next;
  powerManager.updateFrequency(false);
  xSemaphoreGive(powerManager.modeMutex);
}
