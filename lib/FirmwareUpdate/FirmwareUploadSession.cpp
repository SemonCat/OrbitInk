#include "FirmwareUploadSession.h"

#include <Memory.h>

#include <cstring>

namespace firmware_upload {

const char* errorName(const Error e) {
  switch (e) {
    case Error::NONE:
      return "NONE";
    case Error::BUSY:
      return "BUSY";
    case Error::TOO_LARGE:
      return "TOO_LARGE";
    case Error::INVALID:
      return "INVALID";
    case Error::SIZE_MISMATCH:
      return "SIZE_MISMATCH";
    case Error::HASH_MISMATCH:
      return "HASH_MISMATCH";
    case Error::OOM:
      return "OOM";
    case Error::IO:
      return "IO";
    case Error::ABORTED:
      return "ABORTED";
  }
  return "?";
}

void shaToHex(const uint8_t sha[32], char out[65]) {
  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  for (size_t i = 0; i < 32; i++) {
    out[i * 2] = HEX_DIGITS[sha[i] >> 4];
    out[i * 2 + 1] = HEX_DIGITS[sha[i] & 0x0F];
  }
  out[64] = '\0';
}

namespace {
int hexNibble(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
}  // namespace

bool hexToSha(const char* hex, uint8_t out[32]) {
  if (!hex) return false;
  for (size_t i = 0; i < 32; i++) {
    const int hi = hexNibble(hex[i * 2]);
    if (hi < 0) return false;
    const int lo = hexNibble(hex[i * 2 + 1]);
    if (lo < 0) return false;
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return hex[64] == '\0';
}

Session::~Session() {
  if (st == State::RECEIVING) abort();
  if (shaActive) mbedtls_sha256_free(&shaCtx);
}

void Session::resetTransfer() {
  validator.reset();
  buffer.reset();
  bufferPos = 0;
  if (shaActive) {
    mbedtls_sha256_free(&shaCtx);
    shaActive = false;
  }
}

bool Session::fail(const Error e) {
  if (partOpen) {
    sink.closePart();
    partOpen = false;
  }
  sink.removePart();
  resetTransfer();
  receivedBytes = 0;
  st = State::IDLE;
  err = e;
  return false;
}

bool Session::begin(const size_t declaredSize, const uint8_t* expectedSha, const size_t expectedSize) {
  if (st == State::RECEIVING) {
    err = Error::BUSY;
    return false;
  }
  if (maxSize > 0 && (declaredSize > maxSize || expectedSize > maxSize)) {
    err = Error::TOO_LARGE;
    return false;
  }

  // One image at a time: a new transfer supersedes whatever was staged before it.
  if (st == State::STAGED) sink.removeStaged();
  sink.removePart();  // leftover from a power loss mid-transfer
  st = State::IDLE;
  receivedBytes = 0;
  imgStatus = esp_image::Status::OK;
  err = Error::NONE;
  std::memset(sha, 0, sizeof(sha));

  haveExpectedSha = expectedSha != nullptr;
  if (haveExpectedSha) std::memcpy(expectedShaBytes, expectedSha, sizeof(expectedShaBytes));
  expectedSizeBytes = expectedSize;

  validator = makeUniqueNoThrow<esp_image::StreamingValidator>(maxSize, expectedChip);
  buffer = makeUniqueNoThrow<uint8_t[]>(BUFFER_SIZE);
  if (!validator || !buffer) {
    resetTransfer();
    err = Error::OOM;
    return false;
  }
  mbedtls_sha256_init(&shaCtx);
  mbedtls_sha256_starts(&shaCtx, /*is224=*/0);
  shaActive = true;
  st = State::RECEIVING;
  return true;
}

bool Session::flushBuffer() {
  if (bufferPos == 0) return true;
  const bool ok = sink.write(buffer.get(), bufferPos);
  bufferPos = 0;
  return ok;
}

bool Session::write(const uint8_t* data, const size_t len) {
  if (st != State::RECEIVING) return false;
  if (len == 0) return true;
  if (expectedSizeBytes > 0 && receivedBytes + len > expectedSizeBytes) return fail(Error::SIZE_MISMATCH);

  // Validate before storing: a non-image or wrong-chip file is refused from its first bytes.
  if (!validator->feed(data, len)) {
    imgStatus = validator->failure();
    return fail(imgStatus == esp_image::Status::TOO_LARGE ? Error::TOO_LARGE : Error::INVALID);
  }
  if (!partOpen && validator->bytesSeen() >= 24) {
    if (!sink.openPart()) return fail(Error::IO);
    partOpen = true;
  }

  mbedtls_sha256_update(&shaCtx, data, len);
  receivedBytes += len;
  size_t remaining = len;
  while (remaining > 0) {
    const size_t n = std::min(BUFFER_SIZE - bufferPos, remaining);
    std::memcpy(buffer.get() + bufferPos, data, n);
    bufferPos += n;
    data += n;
    remaining -= n;
    if (bufferPos == BUFFER_SIZE && !flushBuffer()) return fail(Error::IO);
  }
  return true;
}

bool Session::end() {
  if (st != State::RECEIVING) return false;
  if (expectedSizeBytes > 0 && receivedBytes != expectedSizeBytes) return fail(Error::SIZE_MISMATCH);
  imgStatus = validator->finish();
  if (imgStatus != esp_image::Status::OK) {
    return fail(imgStatus == esp_image::Status::TOO_LARGE ? Error::TOO_LARGE : Error::INVALID);
  }
  if (!flushBuffer()) return fail(Error::IO);
  if (partOpen) {
    partOpen = false;
    if (!sink.closePart()) return fail(Error::IO);
  }

  mbedtls_sha256_finish(&shaCtx, sha);
  if (haveExpectedSha && std::memcmp(sha, expectedShaBytes, sizeof(sha)) != 0) return fail(Error::HASH_MISMATCH);

  if (!sink.promote()) return fail(Error::IO);
  resetTransfer();
  st = State::STAGED;
  err = Error::NONE;
  return true;
}

void Session::abort() {
  if (st != State::RECEIVING) return;
  fail(Error::ABORTED);
}

void Session::discard() {
  if (st == State::RECEIVING) {
    abort();
  }
  sink.removePart();
  sink.removeStaged();
  receivedBytes = 0;
  std::memset(sha, 0, sizeof(sha));
  st = State::IDLE;
}

bool Session::stagedInfo(StagedInfo& out) const {
  if (st != State::STAGED) return false;
  out.size = receivedBytes;
  std::memcpy(out.sha, sha, sizeof(out.sha));
  return true;
}

bool Session::stagedShaMatches(const char* hex) const {
  uint8_t want[32];
  if (st != State::STAGED || !hexToSha(hex, want)) return false;
  return std::memcmp(want, sha, sizeof(sha)) == 0;
}

}  // namespace firmware_upload
