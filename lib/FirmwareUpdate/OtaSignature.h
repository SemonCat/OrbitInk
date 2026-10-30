#pragma once

#include <cstddef>
#include <cstdint>

#include "OtaManifest.h"

// ECDSA P-256 / SHA-256 verification of an OTA manifest's canonical payload against an embedded
// public key (uncompressed SEC1 point, 65 bytes). Uses mbedTLS's ECP/ECDSA, which the device build
// already links (CONFIG_MBEDTLS_ECDSA_C / ECP_DP_SECP256R1 in the release sdkconfig). Firmware-only
// plus host tests: the simulator excludes the OTA network path and never calls this.
namespace ota_signature {

constexpr size_t PUBLIC_KEY_LEN = 65;

// True only when `sig` (r||s) is a valid signature by `publicKey` over `payload`.
bool verify(const uint8_t publicKey[PUBLIC_KEY_LEN], const uint8_t* payload, size_t payloadLen, const uint8_t sig[64]);

// Builds the canonical payload of `m` and verifies m.sig over it.
ota_manifest::Result verifyManifest(const ota_manifest::Manifest& m, const uint8_t publicKey[PUBLIC_KEY_LEN]);

}  // namespace ota_signature
