// Tests for the REAL lib/KOReaderSync/BookOrbitOwnerId.cpp and BookOrbitStatsDeviceId.cpp.
//
// Both are privacy-relevant and both are digest-injected, so what is verified here is the part
// that actually carries the risk and that production shares byte-for-byte with these tests: the
// length-framed identity encoding, and the rule that spreads entropy across the eight characters
// BookOrbit slices a session id from. The SHA-256 itself is the firmware's existing mbedTLS
// primitive -- deliberately not reimplemented for this feature, and so not re-tested here.

#include <gtest/gtest.h>

#include <cstring>
#include <set>
#include <string>

#include "BookOrbitOwnerId.h"
#include "BookOrbitStats.h"

namespace {

// A deterministic stand-in for SHA-256. Not a hash function in any security sense -- it exists
// so these tests can assert how the identity BYTES are assembled and how the digest is consumed,
// which is exactly what the injectable seam is for.
void fakeDigest(const uint8_t* data, const size_t len, uint8_t out[32]) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    h ^= data[i];
    h *= 16777619u;
  }
  for (int i = 0; i < 32; i++) {
    h ^= static_cast<uint32_t>(i + 1) * 2654435761u;
    h *= 16777619u;
    out[i] = static_cast<uint8_t>((h >> 13) & 0xFF);
  }
}

std::string bytesOf(const std::string& s) { return s; }

// ---- Length-framed identity encoding -------------------------------------------------------

TEST(BookOrbitOwnerIdTest, IdentityEncodingIsDomainTaggedAndLengthFramed) {
  const std::string encoded = BookOrbitOwnerId::encodeIdentity("ab", "cd");

  const std::string tag = "crossfade/bookorbit/owner/v1";
  ASSERT_GT(encoded.size(), tag.size());
  EXPECT_EQ(encoded.substr(0, tag.size()), tag) << "the domain tag keeps this hash from colliding with another use";

  // tag | u32le(2) "ab" | u32le(2) "cd"
  const std::string expected =
      tag + std::string("\x02\x00\x00\x00", 4) + "ab" + std::string("\x02\x00\x00\x00", 4) + "cd";
  EXPECT_EQ(bytesOf(encoded), bytesOf(expected));
}

TEST(BookOrbitOwnerIdTest, FramingSeparatesSplitsThatASeparatorCharacterWouldNot) {
  // The whole reason for length framing: ("ab","c") and ("a","bc") concatenate identically, so
  // any scheme that relies on a delimiter is one delimiter-in-the-input away from collapsing two
  // accounts into one identity.
  EXPECT_NE(BookOrbitOwnerId::encodeIdentity("ab", "c"), BookOrbitOwnerId::encodeIdentity("a", "bc"));
  EXPECT_NE(BookOrbitOwnerId::encodeIdentity("", "abc"), BookOrbitOwnerId::encodeIdentity("abc", ""));

  // And a delimiter appearing INSIDE a field must not create a collision either.
  const std::string sep(1, '\x1f');
  EXPECT_NE(BookOrbitOwnerId::encodeIdentity("https://a/" + sep, "b"),
            BookOrbitOwnerId::encodeIdentity("https://a/", sep + "b"));
}

TEST(BookOrbitOwnerIdTest, DifferentServersAndAccountsProduceDifferentFingerprints) {
  const auto a = BookOrbitOwnerId::derive("https://a.example", "alice", &fakeDigest);
  const auto b = BookOrbitOwnerId::derive("https://b.example", "alice", &fakeDigest);
  const auto c = BookOrbitOwnerId::derive("https://a.example", "bob", &fakeDigest);

  EXPECT_NE(a, b) << "same account, different server";
  EXPECT_NE(a, c) << "same server, different account";
  EXPECT_EQ(a, BookOrbitOwnerId::derive("https://a.example", "alice", &fakeDigest)) << "must be deterministic";
}

TEST(BookOrbitOwnerIdTest, FingerprintIsAFullOneHundredAndTwentyEightBits) {
  // The predecessor was a 32-bit FNV-1a, whose collisions are findable in seconds -- and that
  // same value gated both queue ownership and whether capture was enabled at all.
  EXPECT_EQ(BookOrbitOwnerId::BYTES, 16u);
  const auto id = BookOrbitOwnerId::derive("https://books.example", "reader", &fakeDigest);
  EXPECT_EQ(id.toHex().size(), 32u);
  EXPECT_FALSE(id.isZero());
}

TEST(BookOrbitOwnerIdTest, ANullDigestYieldsTheZeroIdentityRatherThanAGuess) {
  const auto id = BookOrbitOwnerId::derive("https://a.example", "alice", nullptr);
  EXPECT_TRUE(id.isZero()) << "no digest means no identity, never a fabricated one";
}

TEST(BookOrbitOwnerIdTest, HexRoundTripsExactly) {
  const auto original = BookOrbitOwnerId::derive("https://books.example", "reader", &fakeDigest);
  BookOrbitOwnerId parsed;
  ASSERT_TRUE(BookOrbitOwnerId::fromHex(original.toHex(), parsed));
  EXPECT_EQ(parsed, original);

  // Upper case parses too, since the value round-trips through JSON written by other builds.
  std::string upper = original.toHex();
  for (char& c : upper) {
    if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
  }
  ASSERT_TRUE(BookOrbitOwnerId::fromHex(upper, parsed));
  EXPECT_EQ(parsed, original);
}

TEST(BookOrbitOwnerIdTest, MalformedHexIsRejectedAndZeroed) {
  BookOrbitOwnerId parsed;
  parsed.bytes[0] = 0xAB;
  for (const char* bad : {"", "abc", "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz", "0123456789abcdef0123456789abcde",
                          "0123456789abcdef0123456789abcdef0"}) {
    EXPECT_FALSE(BookOrbitOwnerId::fromHex(bad, parsed)) << bad;
    EXPECT_TRUE(parsed.isZero()) << "a rejected parse must not leave a partial identity: " << bad;
  }
}

// ---- Per-install BookOrbit identifier ---------------------------------------------------------

TEST(BookOrbitStatsDeviceIdTest, IndependentInstallationsDifferInTheFirstEightCharacters) {
  // BookOrbit builds its reading-session id from `deviceId.slice(0, 8)`
  // (koreader-stats.util.ts buildDeviceSessionIdPrefix), so two devices whose ids merely SHARE a
  // prefix have their sessions merged into one on the server.
  //
  uint8_t entropyA[BookOrbitLimits::STATS_DEVICE_ID_ENTROPY_BYTES] = {};
  uint8_t entropyB[BookOrbitLimits::STATS_DEVICE_ID_ENTROPY_BYTES] = {};
  entropyA[0] = 0x11;
  entropyB[0] = 0x12;
  const std::string a = bookOrbitStatsDeviceId(entropyA);
  const std::string b = bookOrbitStatsDeviceId(entropyB);

  ASSERT_GE(a.size(), 8u);
  ASSERT_GE(b.size(), 8u);
  EXPECT_NE(a, b);
  EXPECT_NE(a.substr(0, 8), b.substr(0, 8)) << "the session-id prefix must differ, not just the tail";
}

TEST(BookOrbitStatsDeviceIdTest, InstallationEntropySpansTheServerSessionPrefix) {
  std::set<std::string> prefixes;
  for (uint8_t i = 0; i < 64; i++) {
    uint8_t entropy[BookOrbitLimits::STATS_DEVICE_ID_ENTROPY_BYTES] = {};
    entropy[0] = i;
    entropy[1] = static_cast<uint8_t>(i * 29u);
    const std::string id = bookOrbitStatsDeviceId(entropy);
    ASSERT_GE(id.size(), 8u);
    prefixes.insert(id.substr(0, 8));
  }
  EXPECT_EQ(prefixes.size(), 64u) << "independent installation entropy must reach the session-id prefix";
}

TEST(BookOrbitStatsDeviceIdTest, IsStableForPersistedEntropyAndSatisfiesTheServersRegex) {
  uint8_t entropy[BookOrbitLimits::STATS_DEVICE_ID_ENTROPY_BYTES] = {};
  for (size_t i = 0; i < sizeof(entropy); i++) entropy[i] = static_cast<uint8_t>(i * 17u + 3u);
  const std::string id = bookOrbitStatsDeviceId(entropy);
  EXPECT_EQ(id, bookOrbitStatsDeviceId(entropy)) << "the same persisted installation must keep one id";
  ASSERT_FALSE(id.empty());
  EXPECT_LE(id.size(), 100u);
  for (const char c : id) {
    const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    EXPECT_TRUE(allowed) << "KOREADER_DEVICE_ID_REGEX rejects '" << c << "'";
  }
}

TEST(BookOrbitStatsDeviceIdTest, RejectsMissingEntropyRatherThanReturningASharedFallback) {
  EXPECT_TRUE(bookOrbitStatsDeviceId(nullptr).empty());
}

TEST(BookOrbitStatsDeviceIdTest, IsDistinctFromTheGenericKoSyncDeviceId) {
  // The sibling generic-KOSync client's device_id remains the historical shared constant.
  // BookOrbit's verified per-device operations must not reuse it.
  uint8_t entropy[BookOrbitLimits::STATS_DEVICE_ID_ENTROPY_BYTES] = {};
  EXPECT_NE(bookOrbitStatsDeviceId(entropy), std::string(BookOrbitLimits::DEVICE_ID));
}

TEST(BookOrbitStatsDeviceIdTest, LegacyHardwareAndSharedIdsRequireMigration) {
  EXPECT_FALSE(bookOrbitStatsDeviceIdIsCurrent("a1b2c3d4e5f60718"));  // old 64-bit MAC digest
  EXPECT_FALSE(bookOrbitStatsDeviceIdIsCurrent("crossfade0000"));     // old shared fallback
  EXPECT_FALSE(bookOrbitStatsDeviceIdIsCurrent(""));

  uint8_t entropy[BookOrbitLimits::STATS_DEVICE_ID_ENTROPY_BYTES] = {};
  EXPECT_TRUE(bookOrbitStatsDeviceIdIsCurrent(bookOrbitStatsDeviceId(entropy)));
}

}  // namespace
