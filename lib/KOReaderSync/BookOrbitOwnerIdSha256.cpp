#include <esp_random.h>
#include <mbedtls/sha256.h>

#include <cstring>

#include "BookOrbitOwnerId.h"
#include "BookOrbitStats.h"

// Firmware-only: binds identity primitives to the SHA-256 and hardware RNG the firmware already
// links.
//
// Kept in its own translation unit purely so the portable identity logic in BookOrbitOwnerId.cpp
// stays host-buildable. mbedTLS is not available in the host test environment, so a host target
// links that file and supplies its own digest to the three-argument derive(); nothing here is
// reimplemented on either side, and the shipped firmware has exactly one SHA-256 in it.
//
// mbedTLS is already a dependency of this build -- see src/network/FirmwareFlasher.cpp, which
// uses the same primitive to validate OTA images.

namespace {

void mbedtlsSha256(const uint8_t* data, const size_t len, uint8_t out[32]) {
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  // 0 = SHA-256 rather than SHA-224. Every call is checked: mbedtls_sha256_* can fail on a
  // hardware-accelerated backend, and a silently half-computed digest here would be an owner
  // fingerprint that does not isolate anything.
  if (mbedtls_sha256_starts(&ctx, 0) == 0 && mbedtls_sha256_update(&ctx, data, len) == 0) {
    if (mbedtls_sha256_finish(&ctx, out) != 0) {
      memset(out, 0, 32);
    }
  } else {
    memset(out, 0, 32);
  }
  mbedtls_sha256_free(&ctx);
}

}  // namespace

BookOrbitSha256Fn bookOrbitFirmwareSha256() { return &mbedtlsSha256; }

void bookOrbitFirmwareRandom(uint8_t* out, const size_t len) { esp_fill_random(out, len); }

BookOrbitOwnerId BookOrbitOwnerId::derive(const std::string& baseUrl, const std::string& username) {
  return derive(baseUrl, username, &mbedtlsSha256);
}
