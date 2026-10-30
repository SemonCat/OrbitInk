#pragma once
constexpr int WIFI_OFF = 0, WIFI_STA = 1, WL_CONNECTED = 3;
struct HostSyncWifi {
  bool succeeds = true, connected = false;
  int associations = 0, stops = 0;
  void mode(int mode) { if (mode == WIFI_OFF) ++stops; }
  void begin(const char*, const char* = nullptr) { ++associations; connected = succeeds; }
  int status() { return connected ? WL_CONNECTED : 0; }
  void disconnect(bool) { connected = false; }
};
inline HostSyncWifi WiFi;
