#pragma once

#include <cstddef>
#include <cstdint>

// Custom OTA channel manifest (X3 zh-TW private profile). Pure parsing / policy, host-tested in
// test/ota_manifest; the signature check itself is OtaSignature.h.
//
// manifest.json (published as a release asset next to firmware.bin):
//   {"schema":1,"channel":"bookorbit-x3","target":"x3-zh-TW","version":"1.3.3","build":2,
//    "size":5853045,"sha256":"<64 hex>","url":"https://github.com/<owner>/<repo>/releases/download/<tag>/firmware.bin",
//    "sig":"<128 hex: ECDSA P-256 r||s over the canonical payload>"}
//
// The signature covers canonicalPayload(), never the JSON bytes, so JSON formatting is irrelevant:
//   "crossfade-ota-v1\nchannel=<channel>\ntarget=<target>\nversion=<version>\nbuild=<build>\n"
//   "size=<size>\nsha256=<lowercase hex>\nurl=<url>\n"
// scripts/make_ota_manifest.py produces exactly these bytes. Every field is charset-restricted on
// parse (no '\n', '=', whitespace, or control bytes) so the payload cannot be made ambiguous.
//
// Ordering is the monotonic `build` integer (CROSSFADE_OTA_BUILD), independent of the display
// version: every custom release shares runtime version 1.3.3 with upstream, so the version string
// cannot order them. Same-channel updates require a strictly greater build. An intentional switch
// to a different known channel may install an older signed build after physical confirmation.
namespace ota_manifest {

constexpr size_t MAX_MANIFEST_BYTES = 2048;
constexpr size_t MAX_URL_LEN = 256;
constexpr size_t MAX_FIELD_LEN = 32;
constexpr size_t CANONICAL_MAX = 512;

struct Manifest {
  char channel[MAX_FIELD_LEN + 1] = {};
  char target[MAX_FIELD_LEN + 1] = {};
  char version[MAX_FIELD_LEN + 1] = {};
  uint32_t build = 0;
  uint32_t size = 0;
  uint8_t sha256[32] = {};
  char url[MAX_URL_LEN + 1] = {};
  uint8_t sig[64] = {};  // r || s, big-endian
};

enum class Result : uint8_t {
  OK,
  TOO_LARGE,  // manifest body over MAX_MANIFEST_BYTES
  BAD_JSON,
  BAD_SCHEMA,
  BAD_FIELD,  // missing / wrong type / charset / length
  WRONG_CHANNEL,
  WRONG_TARGET,
  NOT_NEWER,  // build <= running build
  BAD_SIZE,   // outside [minSize, maxSize]
  BAD_URL,    // not under the allowed release-download prefix
  BAD_SIGNATURE,
};

const char* resultName(Result r);

struct Policy {
  const char* channel;
  const char* target;
  uint32_t runningBuild;
  size_t minSize;
  size_t maxSize;         // destination OTA partition size
  const char* urlPrefix;  // e.g. "https://github.com/SemonCat/BookOrbit-X3/releases/download/"
  // Compiled image identity. Only an explicitly selected different known channel permits older builds.
  const char* runningChannel = nullptr;
};

// Parse + structural/charset validation only (no policy, no signature).
Result parse(const char* json, size_t len, Manifest& out);

// Channel/target/build/size/url policy for an already-parsed manifest.
Result checkPolicy(const Manifest& m, const Policy& policy);

// Canonical signed payload; returns its length, or 0 if it would not fit in `cap`.
size_t canonicalPayload(const Manifest& m, char* out, size_t cap);

// True for an https URL under `prefix` whose remainder is "<tag>/<asset>" made only of
// [A-Za-z0-9._-] segments (no "..", query, fragment or extra slashes).
bool urlUnderPrefix(const char* url, const char* prefix);

}  // namespace ota_manifest
