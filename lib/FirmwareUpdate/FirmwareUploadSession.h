#pragma once

#include <mbedtls/sha256.h>

#include <cstddef>
#include <cstdint>
#include <memory>

#include "EspImageValidator.h"

// Lifecycle of one staged firmware image (web upload or OTA download) on the SD card:
//
//   IDLE --begin--> RECEIVING --end(valid)--> STAGED --discard/commitApplied--> IDLE
//                       |  \--end(invalid)/abort--> IDLE (partial removed)
//
// Bytes stream into a ".part" file; only a complete image that passed the shared ESP image check
// (and, for OTA, the manifest's exact size + SHA256) is renamed to the staged path. Nothing is
// written before the first chunk has passed the magic/chip check, so a wrong file never touches
// the SD card. Exactly one image exists at a time: begin() removes any earlier staged image.
//
// Storage goes through Sink so host tests can observe every file operation; the firmware binds it
// to HalStorage (src/network/FirmwareStaging.cpp). No other path on the card is ever touched.
namespace firmware_upload {

class Sink {
 public:
  virtual ~Sink() = default;
  virtual bool openPart() = 0;  // create/truncate the .part file
  virtual bool write(const uint8_t* data, size_t len) = 0;
  virtual bool closePart() = 0;  // flush + close; false if the final flush failed
  virtual void removePart() = 0;
  virtual bool promote() = 0;  // rename .part -> staged (replacing nothing; staged removed first)
  virtual void removeStaged() = 0;
};

enum class State : uint8_t { IDLE, RECEIVING, STAGED };

enum class Error : uint8_t {
  NONE,
  BUSY,           // begin() while another transfer is RECEIVING
  TOO_LARGE,      // declared or streamed size over the limit
  INVALID,        // ESP image check failed (see imageStatus())
  SIZE_MISMATCH,  // OTA: streamed size != expected size
  HASH_MISMATCH,  // OTA: SHA256 != expected
  OOM,
  IO,       // SD open/write/close/rename failed
  ABORTED,  // client disconnected / cancelled
};

const char* errorName(Error e);

class Session {
 public:
  static constexpr size_t BUFFER_SIZE = 4096;

  Session(Sink& sink, size_t maxSize, uint16_t expectedChip)
      : sink(sink), maxSize(maxSize), expectedChip(expectedChip) {}
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  // Start a transfer. `declaredSize` (0 = unknown) lets an oversize request be refused before any
  // byte is read. `expectedSha` (nullptr = none) and `expectedSize` (0 = none) are enforced by end()
  // -- the OTA path passes the signed manifest's values. Returns false (and sets lastError) when
  // refused; a refused begin() never modifies the staged image.
  bool begin(size_t declaredSize, const uint8_t* expectedSha = nullptr, size_t expectedSize = 0);
  bool write(const uint8_t* data, size_t len);
  // Finish the transfer. On success the image is STAGED with its size and SHA256 recorded.
  bool end();
  // Client went away / caller cancelled. Removes the partial file; the session returns to IDLE
  // (any earlier staged image was already superseded by begin()).
  void abort();
  // Drop the staged image (cancel from the web page / after a failed or declined apply).
  void discard();
  // The staged image was flashed; forget it and remove the file.
  void commitApplied() { discard(); }

  State state() const { return st; }
  Error lastError() const { return err; }
  esp_image::Status imageStatus() const { return imgStatus; }
  size_t received() const { return receivedBytes; }
  size_t stagedSize() const { return st == State::STAGED ? receivedBytes : 0; }
  const uint8_t* stagedSha() const { return st == State::STAGED ? sha : nullptr; }
  // True when `hex` (64 lowercase/uppercase hex chars) names the currently staged image.
  bool stagedShaMatches(const char* hex) const;

  struct StagedInfo {
    size_t size = 0;
    uint8_t sha[32] = {};
  };
  // Copies the staged image's size and SHA256; false unless STAGED.
  bool stagedInfo(StagedInfo& out) const;

 private:
  bool fail(Error e);
  bool flushBuffer();
  void resetTransfer();

  Sink& sink;
  const size_t maxSize;
  const uint16_t expectedChip;

  State st = State::IDLE;
  Error err = Error::NONE;
  esp_image::Status imgStatus = esp_image::Status::OK;
  std::unique_ptr<esp_image::StreamingValidator> validator;
  std::unique_ptr<uint8_t[]> buffer;
  size_t bufferPos = 0;
  bool partOpen = false;
  mbedtls_sha256_context shaCtx{};
  bool shaActive = false;
  size_t receivedBytes = 0;
  uint8_t sha[32] = {};
  bool haveExpectedSha = false;
  uint8_t expectedShaBytes[32] = {};
  size_t expectedSizeBytes = 0;
};

// Lowercase hex encode (65-byte out incl. NUL) / strict decode of a SHA256 digest.
void shaToHex(const uint8_t sha[32], char out[65]);
bool hexToSha(const char* hex, uint8_t out[32]);

}  // namespace firmware_upload
