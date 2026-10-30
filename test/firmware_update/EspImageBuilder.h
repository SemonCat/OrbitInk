#pragma once

#include <mbedtls/sha256.h>

#include <cstdint>
#include <cstring>
#include <vector>

// Builds a structurally valid ESP32 app image (header, segments, 16-byte aligned XOR checksum pad,
// optional SHA256 trailer) -- the same layout esptool emits and the bootloader verifies.
inline std::vector<uint8_t> buildEspImage(const std::vector<size_t>& segmentSizes, bool hashAppended = true,
                                          uint16_t chipId = 0x0005) {
  std::vector<uint8_t> img(24, 0);
  img[0] = 0xE9;
  img[1] = static_cast<uint8_t>(segmentSizes.size());
  std::memcpy(img.data() + 12, &chipId, sizeof(chipId));
  img[23] = hashAppended ? 1 : 0;

  uint8_t checksum = 0xEF;
  uint32_t seed = 0x12345678;
  for (const size_t size : segmentSizes) {
    uint8_t segHdr[8] = {};
    const uint32_t len = static_cast<uint32_t>(size);
    std::memcpy(segHdr + 4, &len, sizeof(len));
    img.insert(img.end(), segHdr, segHdr + 8);
    for (size_t i = 0; i < size; i++) {
      seed = seed * 1103515245u + 12345u;
      const uint8_t b = static_cast<uint8_t>(seed >> 16);
      checksum ^= b;
      img.push_back(b);
    }
  }
  const size_t padEnd = (img.size() + 16) & ~static_cast<size_t>(15);
  img.resize(padEnd, 0);
  img[padEnd - 1] = checksum;
  if (hashAppended) {
    uint8_t digest[32];
    mbedtls_sha256(img.data(), img.size(), digest, 0);
    img.insert(img.end(), digest, digest + 32);
  }
  return img;
}

inline std::vector<uint8_t> sha256Of(const std::vector<uint8_t>& data) {
  std::vector<uint8_t> out(32);
  mbedtls_sha256(data.data(), data.size(), out.data(), 0);
  return out;
}
