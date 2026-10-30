#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

/**
 * SHA-256 of `len` bytes at `data` into `out`. Exists so the identity logic below can be
 * host-tested without dragging a crypto stack into the test build; production always supplies
 * the firmware's existing mbedTLS implementation (see BookOrbitOwnerIdSha256.cpp).
 *
 * A plain function pointer, not std::function: this codebase forbids the latter outside cold
 * paths for its per-signature binary cost and closure allocation.
 */
using BookOrbitSha256Fn = void (*)(const uint8_t* data, size_t len, uint8_t out[32]);

/**
 * The SHA-256 the firmware links (mbedTLS), for callers that need the primitive directly rather
 * than through derive(). Defined only in the firmware build (BookOrbitOwnerIdSha256.cpp); host
 * targets supply their own.
 */
BookOrbitSha256Fn bookOrbitFirmwareSha256();

/**
 * 128-bit fingerprint of the sync server + account that owns a queue of reading events.
 *
 * This is a PRIVACY BOUNDARY, not a cache key. It is what stops one account's reading history
 * (timestamps, durations, positions, and the document hash) from being uploaded under another
 * account's credentials after the user repoints KOSync somewhere else. A 32-bit hash is not
 * adequate for that job: collisions are findable by brute force in seconds, and the same value
 * also gates whether capture is enabled at all, so one collision would both switch the feature
 * on and expose the previous account's queue.
 *
 * Derivation: the first 16 bytes of SHA-256 over encodeIdentity()'s length-framed encoding.
 * The hashing itself is the firmware's existing mbedTLS primitive -- deliberately not a second
 * SHA-256 carried by this feature, which would add flash and an extra thing to audit.
 *
 * Deliberately derived from NON-SECRET identity only -- base URL and username. The password and
 * its MD5 never enter this hash: the fingerprint is persisted in plain state.json and written
 * into every queue header, so it must not be credential material even in hashed form.
 *
 * Never logged. Neither the fingerprint nor its inputs appear in any log line; a queue's owner
 * is only ever compared, never printed.
 */
struct BookOrbitOwnerId {
  static constexpr size_t BYTES = 16;
  static constexpr size_t HEX_CHARS = BYTES * 2;

  uint8_t bytes[BYTES] = {};

  /** All-zero: "no configured account". Never a legitimate derived value in practice. */
  bool isZero() const;

  bool operator==(const BookOrbitOwnerId& other) const;
  bool operator!=(const BookOrbitOwnerId& other) const { return !(*this == other); }

  /**
   * The exact bytes that get hashed: a domain tag, then each field as a 32-bit little-endian
   * length followed by its raw bytes.
   *
   * LENGTH FRAMING rather than a separator character, because any separator can itself occur in
   * a URL or a username -- ("ab", "c") and ("a", "bc") must be structurally distinct, not
   * distinct-unless-the-delimiter-appears. The domain tag keeps this fingerprint from ever
   * coinciding with some other SHA-256 of the same two strings computed for another purpose.
   *
   * Pure, portable, and part of the contract: the host tests assert these bytes directly, which
   * is what makes the framing verifiable without a host crypto stack.
   */
  static std::string encodeIdentity(const std::string& baseUrl, const std::string& username);

  /**
   * Fingerprint for one server+account, hashing with the supplied primitive. An empty username
   * or base URL still produces a well-defined value; callers decide whether an unconfigured
   * store should be asked at all.
   */
  static BookOrbitOwnerId derive(const std::string& baseUrl, const std::string& username, BookOrbitSha256Fn sha256);

  /**
   * As above, using the firmware's mbedTLS SHA-256. Defined only in the firmware build
   * (BookOrbitOwnerIdSha256.cpp); host targets call the three-argument form instead.
   */
  static BookOrbitOwnerId derive(const std::string& baseUrl, const std::string& username);

  /** Lower-case hex, exactly HEX_CHARS characters. Exact, round-trips through fromHex(). */
  std::string toHex() const;

  /** Parses exactly HEX_CHARS hex characters. False (leaving `out` zeroed) for anything else. */
  static bool fromHex(const std::string& hex, BookOrbitOwnerId& out);
};
