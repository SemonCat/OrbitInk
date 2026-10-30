#pragma once
#include <cstddef>
#include <cstdint>
class Print {
 public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t) = 0;
  virtual size_t write(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i)
      if (!write(p[i])) return i;
    return n;
  }
};
