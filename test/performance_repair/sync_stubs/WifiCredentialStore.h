#pragma once
#include <optional>
#include <string>
struct WifiCredential { std::string ssid, password; };
struct HostWifiStore {
  int count = 1;
  int getCredentialCount() { return count; }
  std::string getLastConnectedSsid() { return "test"; }
  std::optional<WifiCredential> findCredential(const std::string&) { return getCredentialAt(0); }
  std::optional<WifiCredential> getCredentialAt(int) {
    return count ? std::optional<WifiCredential>({"test", "fixture"}) : std::nullopt;
  }
};
inline HostWifiStore WIFI_STORE;
