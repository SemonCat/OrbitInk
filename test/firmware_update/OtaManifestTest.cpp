#include <gtest/gtest.h>

#include <array>
#include <string>
#include <cstring>

#include "lib/FirmwareUpdate/OtaManifest.h"
#include "lib/FirmwareUpdate/OtaPublicKey.h"
#include "lib/FirmwareUpdate/OtaSignature.h"
#include "lib/FirmwareUpdate/OtaDiagnostics.h"
#include "src/network/OtaChannel.h"

using ota_manifest::Manifest;
using ota_manifest::Result;

namespace {
// Fixture signed by a throwaway test key (generated with openssl for this test only; the release
// key's public half is OtaPublicKey.h and its private half never enters the repository).
constexpr char TEST_PUB_HEX[] =
    "04a9d1e97c667c598f674755db317de11bfea603032a3a3a182440889b43da3a2411504f842c6eb6f29436223bf23f5a1c82bea0da6d"
    "2ccb91d3b24e103a8079d1";
constexpr char SHA[] = "f16d05ec6b29248d2c61adb1e9263f78e4f7bace1b955014a2d17872cfe4064d";
constexpr char SIG[] =
    "0e28cabe43d49fe43597b617d55883a7fc9dcb456758f023fa79d9c90cacf0a20af64862a59ff8ab49aa224d739e139a026e58f901eb28"
    "403a2cc4ea62916d79";
constexpr char URL[] = "https://github.com/SemonCat/BookOrbit-X3/releases/download/x3-b2/firmware.bin";
constexpr char PREFIX[] = "https://github.com/SemonCat/BookOrbit-X3/releases/download/";

std::string manifestJson(const std::string& overrides = "", const std::string& sig = SIG) {
  std::string json = R"({"schema":1,"channel":"bookorbit-x3","target":"x3-zh-TW","version":"1.3.3","build":2,)"
                     R"("size":123456,"sha256":")" +
                     std::string(SHA) + R"(","url":")" + URL + R"(","sig":")" + sig + "\"" + overrides + "}";
  return json;
}

std::array<uint8_t, 65> testPub() {
  std::array<uint8_t, 65> out{};
  for (size_t i = 0; i < 65; i++)
    out[i] = static_cast<uint8_t>(std::stoi(std::string(TEST_PUB_HEX + i * 2, 2), nullptr, 16));
  return out;
}

ota_manifest::Policy policy(uint32_t runningBuild = 1) {
  return {"bookorbit-x3", "x3-zh-TW", runningBuild, 64 * 1024, 0x640000, PREFIX};
}

Result parse(const std::string& json, Manifest& m) { return ota_manifest::parse(json.data(), json.size(), m); }
}  // namespace

TEST(OtaManifest, ValidManifestParsesVerifiesAndPasses) {
  Manifest m;
  ASSERT_EQ(parse(manifestJson(), m), Result::OK);
  EXPECT_STREQ(m.channel, "bookorbit-x3");
  EXPECT_EQ(m.build, 2u);
  EXPECT_EQ(m.size, 123456u);
  EXPECT_EQ(ota_manifest::checkPolicy(m, policy()), Result::OK);
  EXPECT_EQ(ota_signature::verifyManifest(m, testPub().data()), Result::OK);
}

TEST(OtaManifest, CanonicalPayloadMatchesGenerator) {
  Manifest m;
  ASSERT_EQ(parse(manifestJson(), m), Result::OK);
  char buf[ota_manifest::CANONICAL_MAX];
  const size_t n = ota_manifest::canonicalPayload(m, buf, sizeof(buf));
  // Byte-for-byte what scripts/make_ota_manifest.py canonical_payload() emits for the fixture.
  EXPECT_EQ(std::string(buf, n),
            std::string("crossfade-ota-v1\nchannel=bookorbit-x3\ntarget=x3-zh-TW\nversion=1.3.3\nbuild=2\n"
                        "size=123456\nsha256=") +
                SHA + "\nurl=" + URL + "\n");
  EXPECT_EQ(ota_manifest::canonicalPayload(m, buf, 10), 0u);
}

TEST(OtaManifest, JsonFormattingAndExtraKeysDoNotMatter) {
  Manifest m;
  std::string spaced = manifestJson(R"(,"notes":"ignored")");
  spaced.insert(1, "\n  ");
  ASSERT_EQ(parse(spaced, m), Result::OK);
  EXPECT_EQ(ota_signature::verifyManifest(m, testPub().data()), Result::OK);
}

TEST(OtaManifest, AnySignedFieldChangeBreaksSignature) {
  const char* tampered[] = {
      R"({"schema":1,"channel":"bookorbit-x3","target":"x3-zh-TW","version":"1.3.3","build":3,"size":123456,)",
      R"({"schema":1,"channel":"bookorbit-x3","target":"x3-zh-TW","version":"1.3.3","build":2,"size":123457,)",
      R"({"schema":1,"channel":"bookorbit-x3","target":"x3-zh-TW","version":"1.3.4","build":2,"size":123456,)",
  };
  for (const char* head : tampered) {
    const std::string json =
        std::string(head) + R"("sha256":")" + SHA + R"(","url":")" + URL + R"(","sig":")" + SIG + "\"}";
    Manifest m;
    ASSERT_EQ(parse(json, m), Result::OK) << head;
    EXPECT_EQ(ota_signature::verifyManifest(m, testPub().data()), Result::BAD_SIGNATURE) << head;
  }
  Manifest m;
  ASSERT_EQ(parse(manifestJson(), m), Result::OK);
  m.sha256[31] ^= 1;
  EXPECT_EQ(ota_signature::verifyManifest(m, testPub().data()), Result::BAD_SIGNATURE);
}

TEST(OtaManifest, OtherKeyAndBadSignaturesRejected) {
  Manifest m;
  ASSERT_EQ(parse(manifestJson(), m), Result::OK);
  // The release key did not sign the fixture.
  EXPECT_EQ(ota_signature::verifyManifest(m, ota_public_key::OTA_PUBLIC_KEY), Result::BAD_SIGNATURE);
  std::string sig(SIG);
  sig[5] = sig[5] == '0' ? '1' : '0';
  ASSERT_EQ(parse(manifestJson("", sig), m), Result::OK);
  EXPECT_EQ(ota_signature::verifyManifest(m, testPub().data()), Result::BAD_SIGNATURE);
  ASSERT_EQ(parse(manifestJson("", std::string(128, '0')), m), Result::OK);  // r = s = 0
  EXPECT_EQ(ota_signature::verifyManifest(m, testPub().data()), Result::BAD_SIGNATURE);
  auto badPoint = testPub();
  badPoint[64] ^= 1;  // not on the curve
  ASSERT_EQ(parse(manifestJson(), m), Result::OK);
  EXPECT_EQ(ota_signature::verifyManifest(m, badPoint.data()), Result::BAD_SIGNATURE);
}

TEST(OtaManifest, PolicyChecksIdentityOrderingSizeAndUrl) {
  Manifest m;
  ASSERT_EQ(parse(manifestJson(), m), Result::OK);
  auto p = policy();
  p.channel = "stock";
  EXPECT_EQ(ota_manifest::checkPolicy(m, p), Result::WRONG_CHANNEL);
  p = policy();
  p.target = "x4";
  EXPECT_EQ(ota_manifest::checkPolicy(m, p), Result::WRONG_TARGET);
  EXPECT_EQ(ota_manifest::checkPolicy(m, policy(2)), Result::NOT_NEWER);  // same build: no reinstall
  EXPECT_EQ(ota_manifest::checkPolicy(m, policy(9)), Result::NOT_NEWER);  // replayed older manifest
  p = policy();
  p.maxSize = 100000;
  EXPECT_EQ(ota_manifest::checkPolicy(m, p), Result::BAD_SIZE);
  p = policy();
  p.minSize = 200000;
  EXPECT_EQ(ota_manifest::checkPolicy(m, p), Result::BAD_SIZE);
  p = policy();
  p.urlPrefix = "https://github.com/Chuckthe5th/crossfade/releases/download/";  // never the stock feed
  EXPECT_EQ(ota_manifest::checkPolicy(m, p), Result::BAD_URL);
}

TEST(OtaManifest, UrlPolicy) {
  EXPECT_TRUE(ota_manifest::urlUnderPrefix(URL, PREFIX));
  const char* bad[] = {
      "http://github.com/SemonCat/BookOrbit-X3/releases/download/x3-b2/firmware.bin",
      "https://github.com/SemonCat/BookOrbit-X3/releases/download/x3-b2/other.bin",
      "https://github.com/SemonCat/BookOrbit-X3/releases/download/../x/firmware.bin",
      "https://github.com/SemonCat/BookOrbit-X3/releases/download/x3-b2/a/firmware.bin",
      "https://github.com/SemonCat/BookOrbit-X3/releases/download//firmware.bin",
      "https://github.com/SemonCat/BookOrbit-X3/releases/download/x3-b2/firmware.bin?x=1",
      "https://github.com/SemonCat/BookOrbit-X3-evil/releases/download/x3-b2/firmware.bin",
      "https://github.com/SemonCat/BookOrbit-X3/releases/download/x3-b2",
  };
  for (const char* u : bad) EXPECT_FALSE(ota_manifest::urlUnderPrefix(u, PREFIX)) << u;
  EXPECT_FALSE(ota_manifest::urlUnderPrefix(URL, "http://github.com/"));
  EXPECT_FALSE(ota_manifest::urlUnderPrefix(URL, "https://github.com/SemonCat/BookOrbit-X3/releases/download"));
}

TEST(OtaManifest, StructuralRejections) {
  Manifest m;
  EXPECT_EQ(ota_manifest::parse("x", ota_manifest::MAX_MANIFEST_BYTES + 1, m), Result::TOO_LARGE);
  EXPECT_EQ(parse("{not json", m), Result::BAD_JSON);
  EXPECT_EQ(parse("[1,2]", m), Result::BAD_JSON);
  std::string j = manifestJson();
  EXPECT_EQ(parse(std::string(j).replace(j.find("\"schema\":1"), 10, "\"schema\":2"), m), Result::BAD_SCHEMA);
  EXPECT_EQ(parse(std::string(j).replace(j.find("\"build\":2"), 9, "\"build\":-2"), m), Result::BAD_FIELD);
  EXPECT_EQ(parse(std::string(j).replace(j.find("\"build\":2"), 9, "\"build\":2.5"), m), Result::BAD_FIELD);
  EXPECT_EQ(parse(std::string(j).replace(j.find("\"build\":2"), 9, "\"build\":\"2\""), m), Result::BAD_FIELD);
  EXPECT_EQ(parse(std::string(j).replace(j.find("1.3.3"), 5, "1.3\\n3"), m), Result::BAD_FIELD);
  EXPECT_EQ(parse(std::string(j).replace(j.find("1.3.3"), 5, "a=b"), m), Result::BAD_FIELD);
  EXPECT_EQ(parse(std::string(j).replace(j.find("bookorbit-x3"), 12, std::string(40, 'a')), m), Result::BAD_FIELD);
  std::string upperSha(SHA);
  for (auto& c : upperSha) c = static_cast<char>(toupper(c));
  EXPECT_EQ(parse(std::string(j).replace(j.find(SHA), 64, upperSha), m), Result::BAD_FIELD);
  EXPECT_EQ(parse(std::string(j).replace(j.find(SHA), 64, "abc"), m), Result::BAD_FIELD);
  EXPECT_EQ(parse(manifestJson("", "00"), m), Result::BAD_FIELD);
  const size_t urlAt = j.find(URL);
  EXPECT_EQ(parse(std::string(j).replace(urlAt, strlen(URL), std::string(URL) + "%20"), m), Result::BAD_FIELD);
  const std::string noSig = R"({"schema":1,"channel":"bookorbit-x3","target":"x3-zh-TW","version":"1.3.3","build":2,)"
                            R"("size":123456,"sha256":")" +
                            std::string(SHA) + R"(","url":")" + URL + "\"}";
  EXPECT_EQ(parse(noSig, m), Result::BAD_FIELD);
}

TEST(OtaManifest, ExplicitSwitchAllowsOlderStableWithoutPinningNightlyHighWater) {
  Manifest m;
  ASSERT_EQ(parse(manifestJson(), m), Result::OK);
  auto p = policy(4000);
  p.runningChannel = "orbitink-x3-nightly";
  EXPECT_EQ(ota_manifest::checkPolicy(m, p), Result::OK);
  p.runningChannel = "bookorbit-x3";
  EXPECT_EQ(ota_manifest::checkPolicy(m, p), Result::NOT_NEWER);
  p.runningChannel = "unknown";
  EXPECT_EQ(ota_manifest::checkPolicy(m, p), Result::NOT_NEWER);
  p.runningChannel = "orbitink-x3-nightly";
  m.build = 0;
  EXPECT_EQ(ota_manifest::checkPolicy(m, p), Result::NOT_NEWER);
}

TEST(OtaManifest, ChannelSwitchNeverBypassesSignatureIdentitySizeOrUrl) {
  Manifest m;
  ASSERT_EQ(parse(manifestJson(), m), Result::OK);
  auto p = policy(4000);
  p.runningChannel = "orbitink-x3-nightly";
  p.channel = "orbitink-x3-nightly";
  EXPECT_EQ(ota_manifest::checkPolicy(m, p), Result::WRONG_CHANNEL);
  p.channel = "bookorbit-x3";
  m.size = 1;
  EXPECT_EQ(ota_manifest::checkPolicy(m, p), Result::BAD_SIZE);
  m.size = 123456;
  m.url[8] = 'x';
  EXPECT_EQ(ota_manifest::checkPolicy(m, p), Result::BAD_URL);
  EXPECT_EQ(ota_signature::verifyManifest(m, testPub().data()), Result::BAD_SIGNATURE);
}

TEST(OtaManifest, NightlyIdentityIsSignedAndSameChannelCannotReplay) {
  Manifest m;
  ASSERT_EQ(parse(manifestJson(), m), Result::OK);
  strcpy(m.channel, "orbitink-x3-nightly");
  auto p = policy(4000);
  p.channel = "orbitink-x3-nightly";
  p.runningChannel = "bookorbit-x3";
  EXPECT_EQ(ota_manifest::checkPolicy(m, p), Result::OK);
  EXPECT_EQ(ota_signature::verifyManifest(m, testPub().data()), Result::BAD_SIGNATURE);
  p.runningChannel = "orbitink-x3-nightly";
  EXPECT_EQ(ota_manifest::checkPolicy(m, p), Result::NOT_NEWER);
  m.build = 4001;
  EXPECT_EQ(ota_manifest::checkPolicy(m, p), Result::OK);
}

TEST(OtaChannel, MissingOrCorruptPreferenceDefaultsStable) {
  for (const uint64_t value : std::array<uint64_t, 5>{0, 2, 255, 257, UINT64_MAX}) {
    EXPECT_EQ(ota_channel::normalizeSelection(value), 0);
    EXPECT_STREQ(ota_channel::selectedChannel(ota_channel::normalizeSelection(value)), "bookorbit-x3");
  }
  EXPECT_EQ(ota_channel::normalizeSelection(1), 1);
  EXPECT_STREQ(ota_channel::selectedChannel(1), "orbitink-x3-nightly");
  EXPECT_STREQ(ota_channel::manifestUrl(1),
              "https://github.com/SemonCat/OrbitInk/releases/download/nightly/manifest.json");
}

TEST(OtaChannel, NightlyDisplayIsExactlyNightlyWithoutTouchingSignedIdentity) {
  EXPECT_STREQ(ota_channel::displayVersion("orbitink-x3-nightly", "1.3.3"), "nightly");
  EXPECT_STREQ(ota_channel::displayVersion(ota_channel::selectedChannel(1), "1.3.3-rc+abc"), "nightly");
  EXPECT_STREQ(ota_channel::displayVersion("bookorbit-x3", "1.3.3"), "1.3.3");
  EXPECT_STREQ(ota_channel::displayVersion("orbitink-x3-nightly-old", "1.3.3"), "1.3.3");
  EXPECT_FALSE(ota_channel::isNightly(ota_channel::STABLE));
  // The display label never enters the machine identity the signer reads back out of the image.
  EXPECT_STREQ(ota_channel::IDENTITY, "CrossFade-OTA/" CROSSFADE_OTA_CHANNEL "/x3-zh-TW/build=" CROSSFADE_OTA_STR(
                                          CROSSFADE_OTA_BUILD));
  EXPECT_STREQ(ota_channel::NIGHTLY, "orbitink-x3-nightly");
}

TEST(OtaDiagnostics, FailureSummaryNamesPhaseErrorHopHostAndHeap) {
  char line[ota_diagnostics::SUMMARY_MAX];
  ota_diagnostics::Record none;
  ota_diagnostics::summary(none, line, sizeof(line));
  EXPECT_STREQ(line, "");
  ota_diagnostics::Record r;
  r.phase = ota_diagnostics::Phase::DOWNLOAD;
  r.error = "CONNECT_FAILED";
  r.hop = 2;
  r.setHost("release-assets.githubusercontent.com");
  r.freeHeap = 41234;
  r.maxAlloc = 18432;
  ota_diagnostics::summary(r, line, sizeof(line));
  EXPECT_STREQ(line, "DOWNLOAD CONNECT_FAILED hop2 release-assets.githubusercontent.com heap 41234/18432");
  r.detail = "IO";
  r.httpStatus = 200;
  r.bytesDone = 1048576;
  r.bytesTotal = 5918176;
  char text[ota_diagnostics::REPORT_MAX];
  const size_t len = ota_diagnostics::report(r, ota_channel::IDENTITY, "1.3.3", text, sizeof(text));
  const std::string report(text, len);
  EXPECT_EQ(len, strlen(text));
  EXPECT_NE(report.find("Summary: DOWNLOAD CONNECT_FAILED/IO HTTP 200 hop2"), std::string::npos);
  EXPECT_NE(report.find("Bytes: 1048576/5918176"), std::string::npos);
  EXPECT_NE(report.find("Largest free block: 18432"), std::string::npos);
  EXPECT_NE(report.find(ota_channel::IDENTITY), std::string::npos);
  // Bounded everywhere: long host, tiny buffers.
  r.setHost(std::string(200, 'h').c_str());
  EXPECT_EQ(std::string(r.host).size(), sizeof(r.host) - 1);
  char tiny[8];
  ota_diagnostics::summary(r, tiny, sizeof(tiny));
  EXPECT_EQ(strlen(tiny), sizeof(tiny) - 1);
  EXPECT_EQ(ota_diagnostics::report(r, "", "", tiny, sizeof(tiny)), sizeof(tiny) - 1);
  EXPECT_STREQ(ota_diagnostics::phaseName(ota_diagnostics::Phase::MANIFEST_POLICY), "MANIFEST_POLICY");
}
