#pragma once
constexpr int WL_CONNECTED = 3;
struct HostWiFi {
  int connection = WL_CONNECTED;
  int status() const { return connection; }
};
inline HostWiFi WiFi;
