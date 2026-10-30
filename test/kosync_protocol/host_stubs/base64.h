#pragma once
// Host-test stand-in for the Arduino core's base64.h + the global Arduino String type it
// returns. KOReaderSyncClient.cpp only ever calls base64::encode(const char*) to build a Basic
// auth header value; the actual encoding correctness doesn't matter for protocol-logic tests
// (the auth header's exact bytes are never asserted on), so this is a real, simple encoder
// rather than a fake return value, to keep behavior honest without needing it to be exact.
#include <cstdint>
#include <string>

class String {
 public:
  String() = default;
  String(const char* s) : s_(s ? s : "") {}
  String(const std::string& s) : s_(s) {}
  const char* c_str() const { return s_.c_str(); }
  size_t length() const { return s_.size(); }

 private:
  std::string s_;
};

namespace base64 {
inline String encode(const char* input) {
  static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const std::string in(input ? input : "");
  std::string out;
  size_t i = 0;
  const size_t len = in.size();
  while (i + 2 < len) {
    const uint32_t n = (static_cast<uint8_t>(in[i]) << 16) | (static_cast<uint8_t>(in[i + 1]) << 8) |
                       static_cast<uint8_t>(in[i + 2]);
    out += table[(n >> 18) & 0x3F];
    out += table[(n >> 12) & 0x3F];
    out += table[(n >> 6) & 0x3F];
    out += table[n & 0x3F];
    i += 3;
  }
  const size_t remaining = len - i;
  if (remaining == 1) {
    const uint32_t n = static_cast<uint8_t>(in[i]) << 16;
    out += table[(n >> 18) & 0x3F];
    out += table[(n >> 12) & 0x3F];
    out += "==";
  } else if (remaining == 2) {
    const uint32_t n = (static_cast<uint8_t>(in[i]) << 16) | (static_cast<uint8_t>(in[i + 1]) << 8);
    out += table[(n >> 18) & 0x3F];
    out += table[(n >> 12) & 0x3F];
    out += table[(n >> 6) & 0x3F];
    out += "=";
  }
  return String(out);
}
}  // namespace base64
