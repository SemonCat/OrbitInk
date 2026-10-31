#include "OtaUpdater.h"

// clang-format off
// HttpDownloader.h pulls Arduino/SdFat, whose macros collide with lwip's
// ip4_addr.h unless seen first. Pin this order; clang-format would otherwise sort
// the local header last and break the build.
#include "HttpDownloader.h"
#include "CrossPointSettings.h"
#include "FirmwareFlasher.h"
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <OtaVersion.h>
#include <ReleaseJsonParser.h>
#include <esp_wifi.h>
#include <FirmwareUploadSession.h>
#include <OtaManifest.h>
#include <OtaPublicKey.h>
#include <OtaSignature.h>
#include "FirmwareStaging.h"
#include "OtaChannel.h"
#include "OtaFetch.h"
// clang-format on

#include <cstdio>
#include <cstring>
#include <string>

namespace {
constexpr char latestReleaseUrl[] = "https://api.github.com/repos/Chuckthe5th/crossfade/releases/latest";
// Same directory every book cache already lives under -- guaranteed to exist (or cheaply
// recreated by the mkdir below) rather than a new top-level convention.
constexpr char kOtaStagingDir[] = "/.crosspoint";
constexpr char kOtaStagingPath[] = "/.crosspoint/ota_download.bin";
}  // namespace

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
#if CROSSFADE_CUSTOM_ZH_TW
  // Never the public CrossFade feed: it would offer an incompatible stock image, and following its
  // redirects through SecureHttpClient is what the X3 heap-abort decoded to. Only the custom signed
  // channel (bounded fetch, see OtaFetch.h) is consulted, and only when this build configures one.
  return checkCustomChannel();
#endif
  LOG_DBG("OTA", "Checking for update (current: %s)", CROSSPOINT_VERSION);

  // Stream the ~32KB release JSON straight into the parser as it arrives.
  // Buffering the whole body in a std::string would add a growing allocation
  // on top of the TLS session's heap during the fetch; with -fno-exceptions an
  // OOM there aborts. fetchUrl handles the verified-https GET, redirects, and
  // User-Agent (see HttpDownloader).
  ReleaseJsonParser releaseParser;
  const bool ok = HttpDownloader::fetchUrl(latestReleaseUrl, [&releaseParser](const uint8_t* data, size_t len) {
    releaseParser.feed(reinterpret_cast<const char*>(data), len);
    return true;
  });
  if (!ok) {
    LOG_ERR("OTA", "Release check fetch failed");
    return HTTP_ERROR;
  }

  LOG_DBG("OTA", "Parser results: tag=%s firmware=%s", releaseParser.foundTag() ? "yes" : "no",
          releaseParser.foundFirmware() ? "yes" : "no");

  if (!releaseParser.foundTag()) {
    LOG_ERR("OTA", "No tag_name in release JSON");
    return JSON_PARSE_ERROR;
  }

  if (!releaseParser.foundFirmware()) {
    LOG_ERR("OTA", "No firmware.bin asset found");
    return NO_UPDATE;
  }

  latestVersion = releaseParser.getTagName();
  otaUrl = releaseParser.getFirmwareUrl();
  otaSize = releaseParser.getFirmwareSize();
  totalSize = otaSize;
  updateAvailable = true;

  LOG_DBG("OTA", "Found update: tag=%s size=%zu", latestVersion.c_str(), otaSize);
  LOG_DBG("OTA", "Firmware URL: %s", otaUrl.c_str());
  return OK;
}

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty()) {
    return false;
  }
#if CROSSFADE_CUSTOM_ZH_TW
  // checkCustomChannel applies ordering or the explicit channel-switch exception.
  return checkedChannel == SETTINGS.otaChannel;
#endif

  // Release tags are "vMAJOR.MINOR.PATCH" -- ota_version::parse() skips the leading 'v' and
  // compares each field numerically (see its header comment for why a plain sscanf on a
  // 'v'-prefixed string is unsafe). CROSSPOINT_VERSION itself has no 'v' prefix but may carry an
  // "-rc+<hash>" suffix, which parse() also ignores past the patch number.
  const bool currentIsRc = strstr(CROSSPOINT_VERSION, "-rc") != nullptr;
  return ota_version::isNewer(CROSSPOINT_VERSION, latestVersion.c_str(), currentIsRc);
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

void OtaUpdater::onFlashProgress(const size_t written, const size_t total, void* ctx) {
  auto* bridge = static_cast<ProgressBridge*>(ctx);
  bridge->self->processedSize = written;
  bridge->self->totalSize = total;
  if (!bridge->userCallback || total == 0) return;
  const int pct = static_cast<int>(static_cast<uint64_t>(written) * 100 / total);
  if (pct != bridge->lastReportedPct) {
    bridge->lastReportedPct = pct;
    bridge->userCallback(bridge->userCtx);
  }
}

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback onProgress, void* ctx) {
  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }
#if CROSSFADE_CUSTOM_ZH_TW
  return installCustom(onProgress, ctx);
#endif

  // Two phases: download to an SD staging file, then hand off to
  // firmware_flash::flashFromSdPath() -- the same validate-then-flash-then-switch path
  // SdFirmwareUpdateActivity already uses successfully on real X3/X4 hardware. The previous
  // version of this function streamed straight into esp_ota_write()/esp_ota_end()/
  // esp_ota_set_boot_partition(), which invokes esp_image_verify() -- already found (see
  // OtaBootSwitch.h) to reject this project's own valid images on this silicon with a bogus
  // efuse-blk-rev error, so that path would fail at the very last step on real hardware every
  // time. flashFromSdPath() validates the whole image (magic/segments/checksum/SHA256) before
  // ever touching the OTA partition, and switches boot via ota_boot::switchTo() -- a raw otadata
  // write that bypasses esp_image_verify entirely -- instead.
  //
  // esp_https_ota is hardwired to esp-tls/mbedTLS, whose precompiled build on this package can't
  // negotiate TLS 1.3 (see SecureClient.h), so the download itself still goes through
  // HttpDownloader (wolfSSL-backed when FREEINK_NET_WOLFSSL is set), not esp_https_ota.
  Storage.mkdir(kOtaStagingDir, /*pFlag=*/true);

  HalFile file;
  if (!Storage.openFileForWrite("OTA", kOtaStagingPath, file) || !file) {
    LOG_ERR("OTA", "Failed to open staging file: %s", kOtaStagingPath);
    return INTERNAL_UPDATE_ERROR;
  }

  /* For better timing and connectivity, we disable power saving for WiFi */
  esp_wifi_set_ps(WIFI_PS_NONE);

  processedSize = 0;
  int lastReportedDownloadPct = -1;
  bool writeOk = true;
  const bool fetchOk = HttpDownloader::fetchUrl(otaUrl, [&](const uint8_t* data, size_t len) {
    if (file.write(data, len) != len) {
      writeOk = false;
      return false;  // abort the transfer
    }
    processedSize += len;
    // Fire the callback only on whole-percent change. Per-chunk updates wake the
    // render task, whose framebuffer work contends with TLS on the internal arena,
    // and e-ink can't repaint faster than a percent tick anyway.
    if (onProgress && totalSize > 0) {
      const int pct = static_cast<int>(static_cast<uint64_t>(processedSize) * 100 / totalSize);
      if (pct != lastReportedDownloadPct) {
        lastReportedDownloadPct = pct;
        onProgress(ctx);
      }
    }
    return true;
  });
  file.close();

  /* Return back to default power saving for WiFi in case of failing */
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  if (!fetchOk || !writeOk) {
    LOG_ERR("OTA", "Firmware download failed (%s)", writeOk ? "http" : "sd write");
    Storage.remove(kOtaStagingPath);
    return HTTP_ERROR;
  }

  // Second phase: validate + flash the inactive OTA partition + switch boot. Reuses the same
  // onProgress callback via the bridge above -- the caller's progress bar resets to 0% for this
  // phase (a visible "downloading, then installing" two-phase sweep) rather than one continuous
  // bar, since flashFromSdPath reports its own written/total independent of the download's.
  ProgressBridge bridge{this, onProgress, ctx, -1};
  const firmware_flash::Result flashResult = firmware_flash::flashFromSdPath(
      kOtaStagingPath, &OtaUpdater::onFlashProgress, &bridge, /*alreadyValidated=*/false);
  Storage.remove(kOtaStagingPath);

  if (flashResult != firmware_flash::Result::OK) {
    LOG_ERR("OTA", "Install failed: %s", firmware_flash::resultName(flashResult));
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update completed");
  return OK;
}

// --- Custom X3 channel -------------------------------------------------------------------------
//
// check:   bounded GET of the manifest (<= 2 KB) -> parse -> ECDSA P-256 signature over the canonical
//          payload against OtaPublicKey.h -> selected channel/target, build policy, size fits partition,
//          firmware URL under the configured release prefix. Nothing unauthenticated drives a decision.
// install: bounded GET of firmware.bin with Content-Length == signed size, streamed through
//          firmware_upload::Session (ESP image check + SHA256) into the SD staging file; promoted only
//          when size and SHA256 equal the signed values; then the existing flashFromSdPath().

namespace {
struct ManifestBuffer {
  char data[ota_manifest::MAX_MANIFEST_BYTES];
  size_t len = 0;
};

bool appendManifest(const uint8_t* data, const size_t len, void* ctx) {
  auto* buf = static_cast<ManifestBuffer*>(ctx);
  if (len > sizeof(buf->data) - buf->len) return false;
  memcpy(buf->data + buf->len, data, len);
  buf->len += len;
  return true;
}

struct DownloadCtx {
  firmware_upload::Session* session;
  size_t* processed;
  size_t total;
  OtaUpdater::ProgressCallback onProgress;
  void* userCtx;
  int lastPct;
};

bool writeFirmware(const uint8_t* data, const size_t len, void* ctx) {
  auto* d = static_cast<DownloadCtx*>(ctx);
  if (!d->session->write(data, len)) return false;
  *d->processed += len;
  if (d->onProgress && d->total > 0) {
    const int pct = static_cast<int>(static_cast<uint64_t>(*d->processed) * 100 / d->total);
    if (pct != d->lastPct) {
      d->lastPct = pct;
      d->onProgress(d->userCtx);
    }
  }
  return true;
}
}  // namespace

OtaUpdater::OtaUpdaterError OtaUpdater::checkCustomChannel() {
  updateAvailable = false;
  channelDowngrade = false;
  checkedChannel = ota_channel::normalizeSelection(SETTINGS.otaChannel);
  if (!ota_channel::CONFIGURED) {
    LOG_INF("OTA", "No custom update channel configured for this build");
    return NO_UPDATE;
  }
  LOG_INF("OTA", "Checking %s (running build %u)", ota_channel::IDENTITY, ota_channel::BUILD);

  auto body = makeUniqueNoThrow<ManifestBuffer>();
  auto manifest = makeUniqueNoThrow<ota_manifest::Manifest>();
  if (!body || !manifest) {
    LOG_ERR("OTA", "OOM: manifest buffers");
    return OOM_ERROR;
  }
  int status = 0;
  const auto fetched = ota_fetch::get(ota_channel::manifestUrl(checkedChannel), ota_channel::IDENTITY,
                                      sizeof(body->data), 0, &appendManifest, body.get(), &status);
  if (fetched != ota_fetch::Result::OK) {
    LOG_ERR("OTA", "Manifest fetch failed: %s (HTTP %d)", ota_fetch::resultName(fetched), status);
    // A channel with no published release yet answers 404: that is "no update", not a failure.
    return (fetched == ota_fetch::Result::HTTP_STATUS && status == 404) ? NO_UPDATE : HTTP_ERROR;
  }

  auto result = ota_manifest::parse(body->data, body->len, *manifest);
  if (result == ota_manifest::Result::OK) {
    result = ota_signature::verifyManifest(*manifest, ota_public_key::OTA_PUBLIC_KEY);
  }
  if (result != ota_manifest::Result::OK) {
    LOG_ERR("OTA", "Manifest rejected: %s", ota_manifest::resultName(result));
    return result == ota_manifest::Result::BAD_SIGNATURE ? VERIFY_ERROR : JSON_PARSE_ERROR;
  }
  const ota_manifest::Policy policy{ota_channel::selectedChannel(checkedChannel),
                                    ota_channel::TARGET,
                                    ota_channel::BUILD,
                                    esp_image::StreamingValidator::MIN_IMAGE_SIZE,
                                    firmware_staging::partitionLimit(),
                                    ota_channel::RELEASE_PREFIX,
                                    ota_channel::CHANNEL};
  result = ota_manifest::checkPolicy(*manifest, policy);
  if (result == ota_manifest::Result::NOT_NEWER) {
    LOG_INF("OTA", "Signed manifest build %u is not newer than %u", static_cast<unsigned>(manifest->build),
            ota_channel::BUILD);
    return OK;
  }
  if (result != ota_manifest::Result::OK) {
    LOG_ERR("OTA", "Manifest policy: %s", ota_manifest::resultName(result));
    return VERIFY_ERROR;
  }

  if (ota_channel::isNightly(ota_channel::selectedChannel(checkedChannel))) {
    latestVersion = ota_channel::NIGHTLY_LABEL;
  } else {
    char label[ota_manifest::MAX_FIELD_LEN + 40];
    snprintf(label, sizeof(label), "stable %s (build %u)", manifest->version, static_cast<unsigned>(manifest->build));
    latestVersion = label;
  }
  otaUrl = manifest->url;
  otaSize = manifest->size;
  totalSize = otaSize;
  customBuild = manifest->build;
  channelDowngrade = customBuild <= ota_channel::BUILD;
  memcpy(customSha, manifest->sha256, sizeof(customSha));
  updateAvailable = true;
  LOG_INF("OTA", "Verified update: build %u, %u bytes", static_cast<unsigned>(customBuild),
          static_cast<unsigned>(otaSize));
  return OK;
}

OtaUpdater::OtaUpdaterError OtaUpdater::installCustom(ProgressCallback onProgress, void* ctx) {
  firmware_staging::HalStorageSink sink;
  const size_t limit = firmware_staging::partitionLimit();
  firmware_upload::Session session(sink, limit, firmware_staging::deviceChipId());
  if (limit == 0 || !session.begin(otaSize, customSha, otaSize)) {
    LOG_ERR("OTA", "Cannot stage download: %s", firmware_upload::errorName(session.lastError()));
    return session.lastError() == firmware_upload::Error::OOM ? OOM_ERROR : INTERNAL_UPDATE_ERROR;
  }

  esp_wifi_set_ps(WIFI_PS_NONE);
  processedSize = 0;
  totalSize = otaSize;
  DownloadCtx dl{&session, &processedSize, otaSize, onProgress, ctx, -1};
  int status = 0;
  const auto fetched =
      ota_fetch::get(otaUrl.c_str(), ota_channel::IDENTITY, otaSize, otaSize, &writeFirmware, &dl, &status);
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  if (fetched != ota_fetch::Result::OK) {
    LOG_ERR("OTA", "Firmware download failed: %s (HTTP %d, session %s)", ota_fetch::resultName(fetched), status,
            firmware_upload::errorName(session.lastError()));
    const bool rejected = session.state() == firmware_upload::State::IDLE &&
                          session.lastError() != firmware_upload::Error::NONE &&
                          session.lastError() != firmware_upload::Error::IO;
    session.abort();
    return rejected ? VERIFY_ERROR : HTTP_ERROR;
  }
  if (!session.end()) {
    LOG_ERR("OTA", "Downloaded firmware rejected: %s (%s)", firmware_upload::errorName(session.lastError()),
            esp_image::statusName(session.imageStatus()));
    return session.lastError() == firmware_upload::Error::IO ? INTERNAL_UPDATE_ERROR : VERIFY_ERROR;
  }

  // Re-read the removable card against the signed identity immediately before flashing.
  if (firmware_staging::recheckStaged(otaSize, customSha) != firmware_staging::Recheck::OK) {
    session.discard();
    return VERIFY_ERROR;
  }
  // FirmwareFlasher additionally re-validates the ESP image before writing the inactive slot.
  ProgressBridge bridge{this, onProgress, ctx, -1};
  const firmware_flash::Result flashResult = firmware_flash::flashFromSdPath(
      firmware_staging::STAGED_PATH, &OtaUpdater::onFlashProgress, &bridge, /*alreadyValidated=*/false);
  session.commitApplied();
  if (flashResult != firmware_flash::Result::OK) {
    LOG_ERR("OTA", "Install failed: %s", firmware_flash::resultName(flashResult));
    return INTERNAL_UPDATE_ERROR;
  }
  LOG_INF("OTA", "Custom update to build %u completed", static_cast<unsigned>(customBuild));
  return OK;
}
