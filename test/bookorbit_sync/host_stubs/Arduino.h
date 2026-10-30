#pragma once
// Host stand-in for the Arduino globals BookOrbitStatsSync.cpp touches (the heap figures the TLS
// gate reads; installation-id entropy is supplied by a separate test binding).
#include <cstdint>

struct HostEspStub {
  uint32_t freeHeap = 0xFFFFFFFFu;
  uint32_t maxAllocHeap = 0xFFFFFFFFu;
  uint32_t getFreeHeap() const { return freeHeap; }
  uint32_t getMaxAllocHeap() const { return maxAllocHeap; }
};

extern HostEspStub ESP;

// Controllable host clock for deadline tests. Each call advances by hostMillisStep, so a test can
// model time passing inside a request without a real wait.
inline uint32_t hostMillisNow = 0;
inline uint32_t hostMillisStep = 0;
inline unsigned long millis() {
  const uint32_t now = hostMillisNow;
  hostMillisNow += hostMillisStep;
  return now;
}
