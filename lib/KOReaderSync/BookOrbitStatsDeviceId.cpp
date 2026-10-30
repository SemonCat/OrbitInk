#include "BookOrbitStats.h"

// Portable formatting of the per-installation BookOrbit identifier. Hardware RNG access stays in
// BookOrbitOwnerIdSha256.cpp so this file remains directly host-testable.

std::string bookOrbitStatsDeviceId(const uint8_t* entropy) {
  if (!entropy) return {};
  static constexpr char HEX[] = "0123456789abcdef";
  std::string id;
  id.reserve(BookOrbitLimits::STATS_DEVICE_ID_CHARS);
  for (size_t i = 0; i < BookOrbitLimits::STATS_DEVICE_ID_ENTROPY_BYTES; i++) {
    id.push_back(HEX[(entropy[i] >> 4) & 0x0F]);
    id.push_back(HEX[entropy[i] & 0x0F]);
  }
  return id;
}

bool bookOrbitStatsDeviceIdIsCurrent(const std::string& id) {
  if (id.size() != BookOrbitLimits::STATS_DEVICE_ID_CHARS) return false;
  for (const char c : id) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}
