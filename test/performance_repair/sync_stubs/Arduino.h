#pragma once
#include <cstdint>
inline unsigned long testSyncMillis = 0;
inline unsigned long millis() { return testSyncMillis; }
inline void delay(unsigned long ms) { testSyncMillis += ms; }
