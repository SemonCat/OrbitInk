// Simulator stand-in for src/network/FirmwareDeviceInfo.cpp (excluded from [env:simulator_x3]).
// The pinned simulator's esp_ota_get_next_update_partition() returns nullptr, which would make the
// web firmware upload refuse every image. Report what a real X3 reports instead -- the app slot
// size from partitions.csv (app0/app1 = 0x640000) and the ESP32-C3 chip_id -- so the simulator
// exercises the same size/chip checks. Nothing here can flash: flashFromSdPath() is the
// simulator's non-destructive stub.
#include "network/FirmwareStaging.h"

namespace firmware_staging {

size_t partitionLimit() { return 0x640000; }

uint16_t deviceChipId() { return 0x0005; }

}  // namespace firmware_staging
