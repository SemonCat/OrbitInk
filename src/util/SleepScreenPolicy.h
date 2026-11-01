#pragma once

#include <cstdint>

// Resolved once while the outgoing activity still identifies the sleep context.
// Preserving a Home overlay needs rollback storage, but never a reader fast boot.
struct SleepScreenPolicy {
  uint8_t screen;
  bool fromReader;
  bool preserveView;
  bool resumeReader;
};
