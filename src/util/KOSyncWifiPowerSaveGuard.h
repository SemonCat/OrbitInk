#pragma once
#include <esp_wifi.h>

// RAII guard that disables WiFi modem sleep for the lifetime of a bounded KOSync HTTP transaction
// (auth / GET progress / PUT progress, in any combination). Modem sleep introduces enough latency
// between packets that the wolfSSL TLS handshake used for KOSync (see
// lib/KOReaderSync/KOReaderSyncClient.cpp's heap-gate comment) has been observed to fail with
// http=-1 during the field. WIFI_PS_MIN_MODEM is this codebase's normal default power-save mode
// (see src/network/OtaUpdater.cpp's matching disable/restore around its own download transaction);
// restoring it on scope exit means every caller gets this for free on every return path (success,
// early return, or an aborted transaction) without a matching manual restore at each one.
//
// Firmware-only (esp_wifi.h, ESP-IDF): construct this from KOReaderAuthActivity,
// KOReaderSyncActivity, and KOReaderAutoSync.cpp, never from KOReaderSyncClient.cpp itself -- that
// file is deliberately kept free of any ESP-IDF/Arduino include so its PROTOCOL logic can be
// host-tested (see its own header comment).
class KOSyncWifiPowerSaveGuard {
 public:
  KOSyncWifiPowerSaveGuard() { esp_wifi_set_ps(WIFI_PS_NONE); }
  ~KOSyncWifiPowerSaveGuard() { esp_wifi_set_ps(WIFI_PS_MIN_MODEM); }
  KOSyncWifiPowerSaveGuard(const KOSyncWifiPowerSaveGuard&) = delete;
  KOSyncWifiPowerSaveGuard& operator=(const KOSyncWifiPowerSaveGuard&) = delete;
};
