#include "EspImageValidator.h"

#include <algorithm>
#include <cstring>

namespace esp_image {

namespace {
constexpr uint8_t ESP_IMAGE_MAGIC = 0xE9;
constexpr uint8_t CHECKSUM_SEED = 0xEF;
constexpr size_t HEADER_SIZE = 24;
constexpr size_t SEG_HEADER_SIZE = 8;
constexpr size_t SHA_TRAILER = 32;
constexpr size_t CHIP_ID_OFFSET = 12;  // esp_image_header_t.chip_id
}  // namespace

const char* statusName(const Status s) {
  switch (s) {
    case Status::OK:
      return "OK";
    case Status::TOO_SMALL:
      return "TOO_SMALL";
    case Status::TOO_LARGE:
      return "TOO_LARGE";
    case Status::BAD_MAGIC:
      return "BAD_MAGIC";
    case Status::BAD_CHIP:
      return "BAD_CHIP";
    case Status::BAD_SEGMENTS:
      return "BAD_SEGMENTS";
    case Status::BAD_SIZE:
      return "BAD_SIZE";
    case Status::BAD_CHECKSUM:
      return "BAD_CHECKSUM";
    case Status::BAD_SHA:
      return "BAD_SHA";
  }
  return "?";
}

StreamingValidator::StreamingValidator(const size_t maxSize, const uint16_t expectedChip)
    : maxSize(maxSize), expectedChip(expectedChip), xorAccum(CHECKSUM_SEED) {
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, /*is224=*/0);
}

StreamingValidator::~StreamingValidator() { mbedtls_sha256_free(&sha); }

bool StreamingValidator::feed(const uint8_t* data, const size_t len) {
  if (failed != Status::OK || finished) return false;
  total += len;
  if (maxSize > 0 && total > maxSize) {
    failed = Status::TOO_LARGE;
    return false;
  }
  consume(data, len);
  return failed == Status::OK;
}

void StreamingValidator::startNextSegmentOrPad() {
  bufLen = 0;
  if (segIndex < segCount) {
    phase = Phase::SEG_HEADER;
    need = SEG_HEADER_SIZE;
    return;
  }
  // The checksum byte sits at the last byte of the 16-byte aligned pad (always 1..16 bytes).
  const size_t padEnd = (pos + 16) & ~static_cast<size_t>(15);
  phase = Phase::PAD;
  need = padEnd - pos;
}

void StreamingValidator::consume(const uint8_t* data, size_t len) {
  while (len > 0 && failed == Status::OK) {
    switch (phase) {
      case Phase::HEADER: {
        const size_t n = std::min(HEADER_SIZE - bufLen, len);
        std::memcpy(buf + bufLen, data, n);
        mbedtls_sha256_update(&sha, data, n);
        bufLen += n;
        data += n;
        len -= n;
        if (buf[0] != ESP_IMAGE_MAGIC) {
          failed = Status::BAD_MAGIC;
          return;
        }
        if (bufLen >= CHIP_ID_OFFSET + 2 && expectedChip != ANY_CHIP) {
          uint16_t chip;
          std::memcpy(&chip, buf + CHIP_ID_OFFSET, sizeof(chip));
          if (chip != expectedChip) {
            failed = Status::BAD_CHIP;
            return;
          }
        }
        if (bufLen == HEADER_SIZE) {
          segCount = buf[1];
          hashAppended = buf[23] != 0;
          pos = HEADER_SIZE;
          startNextSegmentOrPad();
        }
        break;
      }
      case Phase::SEG_HEADER: {
        const size_t n = std::min(need, len);
        std::memcpy(buf + bufLen, data, n);
        mbedtls_sha256_update(&sha, data, n);
        bufLen += n;
        need -= n;
        pos += n;
        data += n;
        len -= n;
        if (need == 0) {
          uint32_t dataLen;
          std::memcpy(&dataLen, buf + 4, sizeof(dataLen));
          need = dataLen;
          phase = Phase::SEG_DATA;
          if (need == 0) {
            segIndex++;
            startNextSegmentOrPad();
          }
        }
        break;
      }
      case Phase::SEG_DATA: {
        const size_t n = std::min(need, len);
        mbedtls_sha256_update(&sha, data, n);
        uint8_t acc = xorAccum;
        for (size_t i = 0; i < n; i++) acc ^= data[i];
        xorAccum = acc;
        need -= n;
        pos += n;
        data += n;
        len -= n;
        if (need == 0) {
          segIndex++;
          startNextSegmentOrPad();
        }
        break;
      }
      case Phase::PAD: {
        const size_t n = std::min(need, len);
        std::memcpy(buf + bufLen, data, n);
        mbedtls_sha256_update(&sha, data, n);
        bufLen += n;
        need -= n;
        pos += n;
        data += n;
        len -= n;
        if (need == 0) {
          storedChecksum = buf[bufLen - 1];
          if (hashAppended) {
            phase = Phase::SHA;
            need = SHA_TRAILER;
          } else {
            phase = Phase::TRAILING;
          }
        }
        break;
      }
      case Phase::SHA: {
        const size_t n = std::min(need, len);
        std::memcpy(shaTrailer + shaLen, data, n);
        shaLen += n;
        need -= n;
        pos += n;
        data += n;
        len -= n;
        if (need == 0) phase = Phase::TRAILING;
        break;
      }
      case Phase::TRAILING:
        // Bytes past the layout's end; counted in `total` only, rejected as BAD_SIZE by finish().
        return;
    }
  }
}

Status StreamingValidator::finish() {
  if (finished) return failed;
  finished = true;
  if (failed == Status::TOO_LARGE) return failed;
  if (total < MIN_IMAGE_SIZE) return failed = Status::TOO_SMALL;
  if (failed != Status::OK) return failed;

  switch (phase) {
    case Phase::HEADER:
    case Phase::SEG_HEADER:
    case Phase::SEG_DATA:
      return failed = Status::BAD_SEGMENTS;
    case Phase::PAD:
    case Phase::SHA:
      return failed = Status::BAD_SIZE;
    case Phase::TRAILING:
      if (total != pos) return failed = Status::BAD_SIZE;
      break;
  }

  if (xorAccum != storedChecksum) return failed = Status::BAD_CHECKSUM;
  if (hashAppended) {
    uint8_t computed[SHA_TRAILER];
    mbedtls_sha256_finish(&sha, computed);
    if (std::memcmp(computed, shaTrailer, SHA_TRAILER) != 0) return failed = Status::BAD_SHA;
  }
  return Status::OK;
}

}  // namespace esp_image
