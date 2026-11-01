// ProgressSyncNetwork.cpp reports WiFi results to the sync receipt; this suite has no SD card, so
// the receipt is a no-op here. The real formatter is covered by test/progress_sync_retry.
#include "ProgressSyncReceipt.h"

namespace ProgressSyncReceipt {
void noteWifi(bool, uint32_t) {}
}  // namespace ProgressSyncReceipt
