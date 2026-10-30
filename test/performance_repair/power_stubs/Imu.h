#pragma once
struct Imu {
  struct Sample { float gx = 0, gy = 0, gz = 0; };
  inline static int wakes = 0, sleeps = 0, reads = 0;
  inline static bool sleepWorks = true;
  bool begin() { return true; }
  bool sleep() { ++sleeps; return sleepWorks; }
  bool wake() { ++wakes; return true; }
  bool read(Sample&) { ++reads; return true; }
};
