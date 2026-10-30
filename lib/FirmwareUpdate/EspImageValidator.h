#pragma once

#include <mbedtls/sha256.h>

#include <cstddef>
#include <cstdint>

// Streaming ESP32 app-image integrity check, shared by the SD/OTA flasher
// (src/network/FirmwareFlasher.cpp validateImageFile) and the web upload path, so there is one
// implementation of the bootloader-equivalent verification: header magic, chip_id, segment table
// walk, XOR checksum, total size and the SHA256 trailer (hash_appended images).
//
// Fed incrementally so an upload can be checked while it arrives (bad magic / wrong chip / oversize
// are reported from the first bytes, before anything reaches the SD card) and a file can be checked
// in fixed CHUNK reads without holding the image in RAM. Holds no heap: one SHA256 context plus a
// few counters (~0.2 KB), so it may live inside a longer-lived owner.
namespace esp_image {

enum class Status : uint8_t {
  OK,
  TOO_SMALL,
  TOO_LARGE,
  BAD_MAGIC,
  BAD_CHIP,
  BAD_SEGMENTS,  // segment header/data runs past EOF
  BAD_SIZE,      // body+pad+sha length doesn't match file size
  BAD_CHECKSUM,
  BAD_SHA,
};

const char* statusName(Status s);

class StreamingValidator {
 public:
  static constexpr size_t MIN_IMAGE_SIZE = 64 * 1024;
  static constexpr uint16_t ANY_CHIP = 0xFFFF;

  // `maxSize`: destination partition size (0 = unbounded). `expectedChip`: the running image's
  // chip_id, or ANY_CHIP when it cannot be read (same skip semantics FirmwareFlasher always had).
  StreamingValidator(size_t maxSize, uint16_t expectedChip);
  ~StreamingValidator();
  StreamingValidator(const StreamingValidator&) = delete;
  StreamingValidator& operator=(const StreamingValidator&) = delete;

  // Consume the next bytes of the image. Returns false once the image is known to be invalid
  // (sticky; see failure()). Only failures decidable from a prefix are reported here: oversize,
  // magic and chip. Everything else is decided by finish().
  bool feed(const uint8_t* data, size_t len);

  // End of input. Returns the verdict for the whole image, matching validateImageFile()'s historic
  // precedence: size bounds, magic, chip, segments, size layout, checksum, SHA trailer.
  Status finish();

  Status failure() const { return failed; }
  size_t bytesSeen() const { return total; }

 private:
  enum class Phase : uint8_t { HEADER, SEG_HEADER, SEG_DATA, PAD, SHA, TRAILING };

  void startNextSegmentOrPad();
  void consume(const uint8_t* data, size_t len);

  const size_t maxSize;
  const uint16_t expectedChip;
  mbedtls_sha256_context sha;
  Status failed = Status::OK;
  bool finished = false;
  Phase phase = Phase::HEADER;
  size_t total = 0;      // bytes fed so far
  size_t pos = 0;        // bytes consumed by the structural parser
  size_t need = 0;       // bytes still wanted in the current phase
  uint8_t buf[24] = {};  // header (24), segment header (8), pad (<=16)
  size_t bufLen = 0;
  uint8_t segCount = 0;
  uint8_t segIndex = 0;
  bool hashAppended = false;
  uint8_t xorAccum = 0;
  uint8_t shaTrailer[32] = {};
  size_t shaLen = 0;
  uint8_t storedChecksum = 0;
};

}  // namespace esp_image
