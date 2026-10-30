#pragma once

#include <cstdint>

inline unsigned long millis() { return 0; }
inline void yield() {}

struct HostEspStub {
  uint32_t freeHeap = 0xFFFFFFFFu;
  uint32_t maxAllocHeap = 0xFFFFFFFFu;

  uint32_t getFreeHeap() const { return freeHeap; }
  uint32_t getMaxAllocHeap() const { return maxAllocHeap; }
};

inline HostEspStub ESP;
