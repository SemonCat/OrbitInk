#pragma once
#include <Arduino.h>
#include <string>
namespace base64 {
// Basic auth is not exercised; native synthetic BookOrbit headers are used.
inline std::string encode(const char*) { return "unused"; }
}
