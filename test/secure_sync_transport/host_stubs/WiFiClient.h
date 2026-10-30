#pragma once
#include <Client.h>
#include <algorithm>
#include <cstring>
#include <string>

// Finite in-memory peer, with optional open socket to exercise parser timeouts.
namespace HostPeer {
inline std::string response;
inline std::string request;
inline size_t offset = 0;
inline bool stayOpen = false;
inline bool connected = false;
inline size_t bodyOffset = 0;
inline void reset(std::string bytes, bool open = false) {
  response = std::move(bytes);
  request.clear();
  offset = 0;
  bodyOffset = response.find("\r\n\r\n");
  if (bodyOffset != std::string::npos) bodyOffset += 4;
  stayOpen = open;
  connected = false;
  hostMillis = 0;
}
}
class WiFiClient : public Client {
 public:
  int connect(IPAddress, uint16_t) override { return connect("peer", 80); }
  int connect(const char*, uint16_t) override { HostPeer::connected = true; return 1; }
  size_t write(uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t* bytes, size_t count) override {
    HostPeer::request.append(reinterpret_cast<const char*>(bytes), count);
    return count;
  }
  int available() override { return static_cast<int>(HostPeer::response.size() - HostPeer::offset); }
  int read() override {
    return available() ? static_cast<uint8_t>(HostPeer::response[HostPeer::offset++]) : -1;
  }
  int read(uint8_t* bytes, size_t count) override {
    count = std::min(count, static_cast<size_t>(available()));
    std::memcpy(bytes, HostPeer::response.data() + HostPeer::offset, count);
    HostPeer::offset += count;
    return static_cast<int>(count);
  }
  int peek() override { return available() ? static_cast<uint8_t>(HostPeer::response[HostPeer::offset]) : -1; }
  void flush() override {}
  void stop() override { HostPeer::connected = false; }
  uint8_t connected() override { return HostPeer::connected && (available() || HostPeer::stayOpen); }
  operator bool() override { return connected(); }
};
