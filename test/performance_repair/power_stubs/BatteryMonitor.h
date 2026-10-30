#pragma once
class BatteryMonitor {
 public:
  bool readPercentageChecked(uint16_t& value) const { value = 75; return true; }
  int readPercentage() const { return 75; }
};
