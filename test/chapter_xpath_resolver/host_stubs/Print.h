#pragma once
// Host-test stand-in for the Arduino core's Print.h (not vendored in this repo; normally
// supplied by the Arduino-ESP32 framework at firmware build time). Only the members
// BoundedHtmlScanner (koxpath_detail, ChapterXPathResolverInternal.h) actually overrides/calls
// are declared.
#include <cstddef>
#include <cstdint>

class Print {
 public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t c) = 0;
  virtual size_t write(const uint8_t* buffer, size_t size) = 0;
};
