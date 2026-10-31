#pragma once

#include <cstdint>
#include <string_view>

// Identity and endpoint of the custom X3 zh-TW online update channel. Set from platformio.ini
// [env:x3_zh_tw_custom]; every other build leaves CROSSFADE_OTA_MANIFEST_URL undefined and keeps the
// stock behaviour (stock builds) or no online updates at all (a custom build without a channel).
//
// CROSSFADE_OTA_BUILD orders custom releases (see lib/FirmwareUpdate/OtaManifest.h); the device
// requires a strictly greater signed build on the same channel. Explicit channel switching
// also permits an older signed build after physical confirmation.

#ifndef CROSSFADE_OTA_BUILD
#define CROSSFADE_OTA_BUILD 0
#endif
#ifndef CROSSFADE_OTA_CHANNEL
#define CROSSFADE_OTA_CHANNEL "bookorbit-x3"
#endif

#define CROSSFADE_OTA_STR2(x) #x
#define CROSSFADE_OTA_STR(x) CROSSFADE_OTA_STR2(x)

namespace ota_channel {

inline constexpr char STABLE[] = "bookorbit-x3";  // Build 1 signed identity remains stable.
inline constexpr char NIGHTLY[] = "orbitink-x3-nightly";
inline constexpr char CHANNEL[] = CROSSFADE_OTA_CHANNEL;
inline constexpr char TARGET[] = "x3-zh-TW";
inline constexpr unsigned BUILD = CROSSFADE_OTA_BUILD;

#if defined(CROSSFADE_OTA_MANIFEST_URL) && defined(CROSSFADE_OTA_RELEASE_PREFIX)
inline constexpr char MANIFEST_URL[] = CROSSFADE_OTA_MANIFEST_URL;
inline constexpr char RELEASE_PREFIX[] = CROSSFADE_OTA_RELEASE_PREFIX;
#else
inline constexpr char MANIFEST_URL[] = "";
inline constexpr char RELEASE_PREFIX[] = "";
#endif

inline constexpr bool CONFIGURED = MANIFEST_URL[0] != '\0' && RELEASE_PREFIX[0] != '\0' && BUILD > 0;

inline const char* selectedChannel(const uint8_t value) { return value == 1 ? NIGHTLY : STABLE; }
inline const char* manifestUrl(const uint8_t value) {
  return value == 1 ? "https://github.com/SemonCat/OrbitInk/releases/download/nightly/manifest.json" : MANIFEST_URL;
}
inline uint8_t normalizeSelection(const uint64_t value) { return value == 1 ? 1 : 0; }

// Display only: a nightly image or offer is labelled exactly "nightly". Ordering, update detection
// and the signed identity keep using BUILD/CHANNEL/IDENTITY and the manifest's own fields.
inline constexpr char NIGHTLY_LABEL[] = "nightly";
constexpr bool isNightly(const char* channel) { return std::string_view(channel) == NIGHTLY; }
inline const char* displayVersion(const char* channel, const char* runtimeVersion) {
  return isNightly(channel) ? NIGHTLY_LABEL : runtimeVersion;
}

// Sent as the User-Agent of every custom-channel request, and the literal
// scripts/make_ota_manifest.py reads back out of firmware.bin to fill the manifest's
// channel/target/build -- so a manifest can only ever describe the image it was generated from.
// Must stay one NUL-terminated literal of exactly this shape.
inline constexpr char IDENTITY[] =
    "CrossFade-OTA/" CROSSFADE_OTA_CHANNEL "/x3-zh-TW/build=" CROSSFADE_OTA_STR(CROSSFADE_OTA_BUILD);

}  // namespace ota_channel
