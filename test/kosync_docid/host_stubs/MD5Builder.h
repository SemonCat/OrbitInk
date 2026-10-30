#pragma once
// Host stand-in for Arduino's MD5Builder. It does not compute MD5 -- these tests are about the
// READ LOOP in KOReaderDocumentId::calculate, not the digest -- but it records every length it
// is handed, which is exactly what proves the out-of-bounds bug is gone.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

class String {
 public:
  String() = default;
  String(const char* s) : value_(s ? s : "") {}
  const char* c_str() const { return value_.c_str(); }

 private:
  std::string value_;
};

struct Md5Calls {
  static std::vector<size_t>& lengths() {
    static std::vector<size_t> v;
    return v;
  }
  static void clear() { lengths().clear(); }
};

class MD5Builder {
 public:
  void begin() { digest_ = 2166136261u; }
  void add(const uint8_t* data, size_t len) {
    Md5Calls::lengths().push_back(len);
    // Content-sensitive, so hashing a filename and hashing file bytes produce DIFFERENT values.
    // That difference is the whole point of the identity tests: a stub returning a constant
    // would make "stats always use the binary hash" trivially true and prove nothing.
    for (size_t i = 0; i < len; i++) {
      digest_ ^= data ? data[i] : 0;
      digest_ *= 16777619u;
    }
    digest_ ^= static_cast<uint32_t>(len);
    digest_ *= 16777619u;
  }
  void add(const char* s) { add(reinterpret_cast<const uint8_t*>(s), s ? strlen(s) : 0); }
  void calculate() {}
  String toString() const {
    static const char* HEX = "0123456789abcdef";
    std::string out;
    uint32_t h = digest_;
    for (int i = 0; i < 32; i++) {
      out.push_back(HEX[h & 0x0F]);
      h = (h >> 4) | (h << 28);
      h ^= static_cast<uint32_t>(i) * 2654435761u;
    }
    return String(out.c_str());
  }

 private:
  uint32_t digest_ = 2166136261u;
};
