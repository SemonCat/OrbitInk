#pragma once
constexpr int WIFI_MODE_NULL = 0;
struct HostWiFi {
  int mode = WIFI_MODE_NULL;
  int getMode() const { return mode; }
};
inline HostWiFi WiFi;
