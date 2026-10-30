#pragma once

#include <cstdint>

#include "ProgressTimestamp.h"

namespace ReadingPositionPolicy {

constexpr bool shouldStamp(bool logicalPositionChanged, bool navigationRequested, bool inFootnote) {
  return logicalPositionChanged && navigationRequested && !inFootnote;
}

// Seconds precision can legitimately give consecutive page turns the same value. Only a clock
// that is invalid or moved backwards destroys chronology; equal seconds remain a valid tie that
// the sync policy resolves deterministically from the fetched server record.
constexpr uint64_t timestampAfterChange(uint64_t previous, uint64_t current) {
  if (!ProgressTimestamp::valid(current)) return 0;
  if (ProgressTimestamp::valid(previous) && current < previous) return 0;
  return current;
}

}  // namespace ReadingPositionPolicy
