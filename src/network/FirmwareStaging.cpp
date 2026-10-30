#include "FirmwareStaging.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <mbedtls/sha256.h>

#include <cstring>

#include "util/TaskWatchdog.h"

namespace firmware_staging {

namespace {
constexpr char STAGING_DIR[] = "/.crosspoint";
constexpr size_t READ_CHUNK = 4096;
}  // namespace

bool HalStorageSink::openPart() {
  Storage.mkdir(STAGING_DIR, /*pFlag=*/true);
  if (!Storage.openFileForWrite("FWSTAGE", PART_PATH, file) || !file) {
    LOG_ERR("FWSTAGE", "open %s failed", PART_PATH);
    return false;
  }
  return true;
}

bool HalStorageSink::write(const uint8_t* data, const size_t len) { return file && file.write(data, len) == len; }

bool HalStorageSink::closePart() {
  if (!file) return true;
  file.flush();
  return file.close();
}

void HalStorageSink::removePart() {
  if (file) file.close();
  if (Storage.exists(PART_PATH)) Storage.remove(PART_PATH);
}

bool HalStorageSink::promote() {
  if (Storage.exists(STAGED_PATH)) Storage.remove(STAGED_PATH);
  if (!Storage.rename(PART_PATH, STAGED_PATH)) {
    LOG_ERR("FWSTAGE", "rename to %s failed", STAGED_PATH);
    return false;
  }
  return true;
}

void HalStorageSink::removeStaged() {
  if (Storage.exists(STAGED_PATH)) Storage.remove(STAGED_PATH);
}

Recheck recheckStaged(const size_t size, const uint8_t sha[32]) {
  HalFile file;
  if (!Storage.openFileForRead("FWSTAGE", STAGED_PATH, file) || !file) return Recheck::OPEN_FAIL;
  if (file.fileSize() != size) {
    LOG_ERR("FWSTAGE", "staged size changed: %u != %u", static_cast<unsigned>(file.fileSize()),
            static_cast<unsigned>(size));
    return Recheck::CHANGED;
  }
  auto buf = makeUniqueNoThrow<uint8_t[]>(READ_CHUNK);
  auto validator = makeUniqueNoThrow<esp_image::StreamingValidator>(partitionLimit(), deviceChipId());
  if (!buf || !validator) {
    LOG_ERR("FWSTAGE", "OOM: recheck buffers");
    return Recheck::OOM;
  }
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, /*is224=*/0);
  size_t remaining = size;
  bool readOk = true;
  while (remaining > 0) {
    resetTaskWatchdogIfSubscribed();
    const size_t want = remaining < READ_CHUNK ? remaining : READ_CHUNK;
    const int got = file.read(buf.get(), want);
    if (got <= 0 || static_cast<size_t>(got) != want) {
      readOk = false;
      break;
    }
    mbedtls_sha256_update(&ctx, buf.get(), want);
    validator->feed(buf.get(), want);
    remaining -= want;
  }
  uint8_t digest[32];
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  if (!readOk || std::memcmp(digest, sha, sizeof(digest)) != 0) {
    LOG_ERR("FWSTAGE", "staged image no longer matches the uploaded SHA256");
    return Recheck::CHANGED;
  }
  const esp_image::Status status = validator->finish();
  if (status != esp_image::Status::OK) {
    LOG_ERR("FWSTAGE", "staged image invalid: %s", esp_image::statusName(status));
    return Recheck::INVALID;
  }
  return Recheck::OK;
}

}  // namespace firmware_staging
