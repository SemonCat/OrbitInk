#pragma once
// Force-included (via CMake -include) into KOReaderSyncClient.cpp for the host build, standing
// in for the ESP/millis/yield globals PlatformIO's Arduino framework normally injects. Only
// ESP.getFreeHeap()/getMaxAllocHeap() are actually used by KOReaderSyncClient.cpp; both default
// generously so the TLS heap gate never fires unless a test deliberately lowers them.
#include <cstdint>

struct HostEspStub {
  uint32_t freeHeap = 0xFFFFFFFFu;
  uint32_t maxAllocHeap = 0xFFFFFFFFu;
  uint32_t getFreeHeap() const { return freeHeap; }
  uint32_t getMaxAllocHeap() const { return maxAllocHeap; }
};

inline HostEspStub ESP;
