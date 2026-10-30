#pragma once
#include <cmath>
#include <cstdint>
inline unsigned long testMillis = 1000;
inline int testCpuMHz = 160;
inline unsigned long millis() { return testMillis; }
inline unsigned long micros() { return testMillis * 1000; }
inline int getCpuFrequencyMhz() { return testCpuMHz; }
inline bool setCpuFrequencyMhz(int frequency) { testCpuMHz = frequency; return true; }
inline void pinMode(int, int) {}
constexpr int INPUT = 0;
