// mbedTLS ECDSA is not part of the simulator's mbedtls shim (sha256/base64 only); the simulator
// never reaches the online OTA path (OtaUpdater.cpp is excluded there), so this translation unit
// compiles to nothing for it.
#if !defined(SIMULATOR)

#include "OtaSignature.h"

#include <Memory.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/sha256.h>

namespace ota_signature {

bool verify(const uint8_t publicKey[PUBLIC_KEY_LEN], const uint8_t* payload, const size_t payloadLen,
            const uint8_t sig[64]) {
  uint8_t hash[32];
  if (mbedtls_sha256(payload, payloadLen, hash, /*is224=*/0) != 0) return false;

  mbedtls_ecp_group grp;
  mbedtls_ecp_point q;
  mbedtls_mpi r;
  mbedtls_mpi s;
  mbedtls_ecp_group_init(&grp);
  mbedtls_ecp_point_init(&q);
  mbedtls_mpi_init(&r);
  mbedtls_mpi_init(&s);

  // mbedtls_ecdsa_verify range-checks r and s (1 <= r,s < n) itself.
  const bool ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
                  mbedtls_ecp_point_read_binary(&grp, &q, publicKey, PUBLIC_KEY_LEN) == 0 &&
                  mbedtls_ecp_check_pubkey(&grp, &q) == 0 && mbedtls_mpi_read_binary(&r, sig, 32) == 0 &&
                  mbedtls_mpi_read_binary(&s, sig + 32, 32) == 0 &&
                  mbedtls_ecdsa_verify(&grp, hash, sizeof(hash), &q, &r, &s) == 0;

  mbedtls_mpi_free(&s);
  mbedtls_mpi_free(&r);
  mbedtls_ecp_point_free(&q);
  mbedtls_ecp_group_free(&grp);
  return ok;
}

ota_manifest::Result verifyManifest(const ota_manifest::Manifest& m, const uint8_t publicKey[PUBLIC_KEY_LEN]) {
  // Keep the 512-byte canonical buffer off the constrained device task stack.
  auto payload = makeUniqueNoThrow<char[]>(ota_manifest::CANONICAL_MAX);
  if (!payload) return ota_manifest::Result::BAD_SIGNATURE;
  const size_t len = ota_manifest::canonicalPayload(m, payload.get(), ota_manifest::CANONICAL_MAX);
  if (len == 0) return ota_manifest::Result::BAD_FIELD;
  return verify(publicKey, reinterpret_cast<const uint8_t*>(payload.get()), len, m.sig)
             ? ota_manifest::Result::OK
             : ota_manifest::Result::BAD_SIGNATURE;
}

}  // namespace ota_signature

#endif  // !SIMULATOR
