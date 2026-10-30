#pragma once

// Harness-local Arduino.h stub. Unlike test/epub_ports/host_stubs/Arduino.h (a fixed,
// test-settable freeHeap field), this one reports the *live* allocation-budget counter
// maintained by the operator-new override in main.cpp, so ESP.getFreeHeap() always reflects
// exactly how much of the injected fault budget remains -- the same number that will make the
// next oversized allocation fail. This is what ties "the guard reads low heap" and "the next
// allocation would actually fail" to the same source of truth, per advisor's design.

#include <cstddef>
#include <cstdint>

extern std::size_t g_allocBudgetBytes;

inline unsigned long millis() { return 0; }
inline void yield() {}

struct HostEspStub {
  uint32_t getFreeHeap() const { return static_cast<uint32_t>(g_allocBudgetBytes); }
  uint32_t getMaxAllocHeap() const { return static_cast<uint32_t>(g_allocBudgetBytes); }
};

inline HostEspStub ESP;

inline void delay(unsigned long) {}
