#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
using String = std::string;
class IPAddress {};
inline unsigned long hostMillis = 0;
inline unsigned long millis() { return hostMillis; }
inline void delay(unsigned long ms) { hostMillis += ms; }
