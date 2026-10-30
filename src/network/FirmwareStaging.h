#pragma once

#include <FirmwareUploadSession.h>
#include <HalStorage.h>

#include <cstddef>
#include <cstdint>

// SD-card binding of firmware_upload::Sink, shared by the web upload (CrossPointWebServer) and the
// custom OTA download (OtaUpdater). Only these two paths are ever written, both under the hidden
// cache directory every book cache already uses -- books, fonts and settings are never touched.
namespace firmware_staging {

constexpr char PART_PATH[] = "/.crosspoint/fw_upload.part";
constexpr char STAGED_PATH[] = "/.crosspoint/fw_staged.bin";

class HalStorageSink final : public firmware_upload::Sink {
 public:
  bool openPart() override;
  bool write(const uint8_t* data, size_t len) override;
  bool closePart() override;
  void removePart() override;
  bool promote() override;
  void removeStaged() override;

 private:
  HalFile file;
};

// Size of the next OTA app partition (the upload size limit), or 0 if none.
size_t partitionLimit();

// chip_id of the running image (esp_image::StreamingValidator::ANY_CHIP if unreadable).
uint16_t deviceChipId();

// Re-reads STAGED_PATH once: OK only if it still has exactly `size` bytes with SHA256 `sha` AND
// passes the shared ESP image check (esp_image::StreamingValidator) against this device. The SD card
// is removable and the hidden directory is reachable over WebDAV, so the bytes confirmed in the
// browser are re-checked immediately before the on-device confirmation. Uses the shared validator,
// not firmware_flash::validateImageFile, so the simulator runs the same check.
enum class Recheck : uint8_t { OK, OPEN_FAIL, CHANGED, INVALID, OOM };
Recheck recheckStaged(size_t size, const uint8_t sha[32]);

}  // namespace firmware_staging
