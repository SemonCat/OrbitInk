#pragma once
#include <string>
class Xtc {
 public:
  Xtc(const std::string&, const std::string&) {}
  std::string getThumbBmpPath(int, int) const { return "/xtc-thumb.bmp"; }
};
