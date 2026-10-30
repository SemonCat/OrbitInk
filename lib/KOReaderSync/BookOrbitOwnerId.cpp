#include "BookOrbitOwnerId.h"

#include <cstring>

// Portable half of the owner fingerprint: the identity encoding, hex conversion, and comparison.
// No crypto here at all -- the SHA-256 arrives as a function pointer, supplied in the firmware
// by mbedTLS (BookOrbitOwnerIdSha256.cpp) and in host tests by a deterministic stand-in. That
// split is what lets the framing, which is the part carrying the ambiguity risk, be asserted
// byte-for-byte on the host without this feature shipping a second SHA-256 to audit.

namespace {

// Domain separator. Not named DOMAIN: that is a macro in <math.h>, which some translation units
// pull in transitively.
constexpr char OWNER_DOMAIN_TAG[] = "crossfade/bookorbit/owner/v1";

void appendLengthFramed(std::string& out, const std::string& value) {
  const uint32_t len = static_cast<uint32_t>(value.size());
  out.push_back(static_cast<char>(len & 0xFF));
  out.push_back(static_cast<char>((len >> 8) & 0xFF));
  out.push_back(static_cast<char>((len >> 16) & 0xFF));
  out.push_back(static_cast<char>((len >> 24) & 0xFF));
  out.append(value);
}

}  // namespace

std::string BookOrbitOwnerId::encodeIdentity(const std::string& baseUrl, const std::string& username) {
  // One short-lived buffer on a cold path (config change, book open, sync), not the render loop.
  // Reserved up front so the appends below cannot trigger a reallocation chain.
  std::string encoded;
  encoded.reserve(sizeof(OWNER_DOMAIN_TAG) - 1 + 8 + baseUrl.size() + username.size());
  encoded.append(OWNER_DOMAIN_TAG, sizeof(OWNER_DOMAIN_TAG) - 1);
  appendLengthFramed(encoded, baseUrl);
  appendLengthFramed(encoded, username);
  return encoded;
}

BookOrbitOwnerId BookOrbitOwnerId::derive(const std::string& baseUrl, const std::string& username,
                                          const BookOrbitSha256Fn sha256) {
  BookOrbitOwnerId id;
  if (sha256 == nullptr) return id;  // zeroed: treated as "no configured account" by callers

  const std::string encoded = encodeIdentity(baseUrl, username);
  uint8_t digest[32] = {};
  sha256(reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size(), digest);
  memcpy(id.bytes, digest, BYTES);  // truncation to 128 bits: standard, and ample here
  return id;
}

bool BookOrbitOwnerId::isZero() const {
  for (const uint8_t b : bytes) {
    if (b != 0) return false;
  }
  return true;
}

bool BookOrbitOwnerId::operator==(const BookOrbitOwnerId& other) const {
  return memcmp(bytes, other.bytes, BYTES) == 0;
}

std::string BookOrbitOwnerId::toHex() const {
  static constexpr char HEX[] = "0123456789abcdef";
  std::string out;
  out.reserve(HEX_CHARS);
  for (const uint8_t b : bytes) {
    out.push_back(HEX[(b >> 4) & 0x0F]);
    out.push_back(HEX[b & 0x0F]);
  }
  return out;
}

bool BookOrbitOwnerId::fromHex(const std::string& hex, BookOrbitOwnerId& out) {
  out = BookOrbitOwnerId{};
  if (hex.size() != HEX_CHARS) return false;

  BookOrbitOwnerId parsed;
  for (size_t i = 0; i < BYTES; i++) {
    uint8_t value = 0;
    for (int nibble = 0; nibble < 2; nibble++) {
      const char c = hex[i * 2 + nibble];
      uint8_t digit;
      if (c >= '0' && c <= '9') {
        digit = static_cast<uint8_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        digit = static_cast<uint8_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        digit = static_cast<uint8_t>(c - 'A' + 10);
      } else {
        return false;
      }
      value = static_cast<uint8_t>((value << 4) | digit);
    }
    parsed.bytes[i] = value;
  }
  out = parsed;
  return true;
}
