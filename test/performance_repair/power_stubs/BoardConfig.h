#pragma once
namespace BoardConfig {
struct Config {
  int batteryAdc = -1;
  struct { int gaugeAddr = 0; } batteryGauge;
};
inline Config ACTIVE;
}
