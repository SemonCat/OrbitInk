#pragma once
// LOG_INF is captured (not just no-op'd like LOG_DBG/LOG_ERR here) so tests can assert on the
// release-visible GET/PUT outcome logging in KOReaderSyncClient.cpp: that it actually fires, and
// that it never leaks credentials/auth headers/passwords/full sensitive URLs. Real firmware builds
// use lib/Logging/Logging.h (UART), which this file never touches.
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

class CapturedLogs {
 public:
  static std::vector<std::string>& infoLines() {
    static std::vector<std::string> lines;
    return lines;
  }
  static void clear() { infoLines().clear(); }
};

inline void captureLogInf(const char* origin, const char* format, ...) {
  char buf[256];
  va_list args;
  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  CapturedLogs::infoLines().push_back(std::string(origin) + ": " + buf);
}

#define LOG_DBG(origin, format, ...) ((void)0)
#define LOG_ERR(origin, format, ...) ((void)0)
#define LOG_INF(origin, format, ...) captureLogInf(origin, format, ##__VA_ARGS__)
