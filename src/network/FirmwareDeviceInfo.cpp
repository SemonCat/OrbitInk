// Device facts for firmware staging. Kept apart from FirmwareStaging.cpp so the simulator, whose
// esp_ota stubs report no partition at all, can substitute its own definitions
// (sim/CrossFadeSimHal/FirmwareDeviceInfoCompat.cpp) without touching the staging logic.
#include <esp_ota_ops.h>

#include "FirmwareFlasher.h"
#include "FirmwareStaging.h"

namespace firmware_staging {

size_t partitionLimit() {
  const esp_partition_t* dest = esp_ota_get_next_update_partition(nullptr);
  return dest ? dest->size : 0;
}

uint16_t deviceChipId() { return firmware_flash::runningPartitionChipId(); }

}  // namespace firmware_staging
