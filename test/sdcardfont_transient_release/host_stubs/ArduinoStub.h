#pragma once
// Force-included (via CMake's -include flag) into SdCardFont.cpp for the host
// build, standing in for the free functions/globals (millis(), yield(), the
// `ESP` object) that PlatformIO's Arduino framework normally injects into
// every translation unit. SdCardFont.cpp calls these directly with no
// explicit #include, so on host they must be visible some other way.
//
// `ESP.freeHeap`/`ESP.maxAllocHeap` are plain mutable fields (not a faithful
// ESPClass) so a test can directly set the simulated heap state before
// exercising SdCardFont's heap-floor-dependent retention logic
// (resetStyleMiniData()'s MINI_RETAIN_MIN_FREE_HEAP check) -- this is what
// lets the test prove releaseTransientRenderCache() frees mini data
// UNCONDITIONALLY even when the stubbed free heap is high enough that
// clearCache() would retain it.

#include <cstdint>

inline unsigned long millis() { return 0; }
inline void yield() {}

struct HostEspStub {
  uint32_t freeHeap = 0xFFFFFFFFu;      // default: effectively unlimited, so clearCache() retains
  uint32_t maxAllocHeap = 0xFFFFFFFFu;  // by default unless a test lowers it
  uint32_t getFreeHeap() const { return freeHeap; }
  uint32_t getMaxAllocHeap() const { return maxAllocHeap; }
};

inline HostEspStub ESP;
