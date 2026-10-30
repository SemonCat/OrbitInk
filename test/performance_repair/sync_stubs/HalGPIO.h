#pragma once
#include <Arduino.h>
struct HostGpio {
  bool pressed = false;
  void update() {}
  bool wasAnyPressed() { return pressed; }
};
inline HostGpio gpio;
