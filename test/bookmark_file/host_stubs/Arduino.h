#pragma once
// Host-test stand-in for Arduino.h, sized to what PersistableStore.h/.cpp and BookmarkFile.cpp
// actually need: the Arduino String class (ArduinoJson's serializeJson/deserializeJson both
// accept it directly via a Print-like write() interface) and Print itself.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

class Print {
 public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t b) = 0;
  virtual size_t write(const uint8_t* buffer, size_t size) {
    size_t n = 0;
    for (size_t i = 0; i < size; i++) {
      if (write(buffer[i]))
        n++;
      else
        break;
    }
    return n;
  }
};

// Minimal Arduino String: just enough for ArduinoJson's serializeJson()/deserializeJson() to
// treat it as a writable/readable buffer, and for PersistableStore.cpp's own isEmpty()/c_str()
// use. Backed by std::string rather than reproducing Arduino's own SSO buffer.
class String : public Print {
 public:
  String() = default;
  String(const char* s) : value_(s ? s : "") {}  // NOLINT(google-explicit-constructor)
  String(const std::string& s) : value_(s) {}    // NOLINT(google-explicit-constructor)

  size_t write(uint8_t b) override {
    value_.push_back(static_cast<char>(b));
    return 1;
  }
  size_t write(const uint8_t* buffer, size_t size) override {
    value_.append(reinterpret_cast<const char*>(buffer), size);
    return size;
  }

  const char* c_str() const { return value_.c_str(); }
  bool isEmpty() const { return value_.empty(); }
  size_t length() const { return value_.size(); }
  operator const std::string&() const { return value_; }  // NOLINT(google-explicit-constructor)

  // ArduinoJson's deserializeJson(doc, aString) treats its source via a stream-like Reader
  // concept (a single-byte read() returning -1 at end), not the write()-based Writer concept
  // used for serialization -- both are needed since PersistableStore.cpp uses String for both
  // directions.
  int read() { return readPos_ < value_.size() ? static_cast<uint8_t>(value_[readPos_++]) : -1; }

 private:
  std::string value_;
  size_t readPos_ = 0;
};
