#pragma once
#include <cstddef>

// Shared "last synced" status for BookOrbit progress sync (src/util/BookOrbitAutoSync.h): the
// persisted success timestamp behind the status-bar sync age, and the one-shot success notice the
// next Home/reader screen shows. The namespace keeps its historical KOSync name because the
// persisted field (CrossPointState::lastKoSyncEpochMinutes) and every caller already use it; the
// independent KOSync provider it once also served has been removed from this BookOrbit-only
// product (docs/bookorbit-library.md §6).
namespace KOReaderAutoSync {

// One-shot UI handoff consumed by the destination activity after automatic open/close sync.
bool consumeSuccessNotice();

// Compact status-bar age ("5m", "2h", "3d", then "MM/DD"). False means no valid persisted
// success timestamp or no RTC time is available.
bool formatLastSuccessRelative(char* output, size_t outputSize);

// Records a successful BookOrbit sync transaction. `queueNotice` is true for close/open (visible
// transitions), false for the silent sleep and timer paths.
void recordProviderSuccess(bool queueNotice);

}  // namespace KOReaderAutoSync
