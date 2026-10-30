#pragma once
// Host-test stand-in for lib/Logging/Logging.h. SdCardFont.cpp only needs the
// LOG_DBG/LOG_ERR macros to exist and accept printf-style varargs; the actual
// device implementation (Serial output, log levels) is irrelevant to this
// test and dragging it in would pull in the ESP32 Arduino framework.
#define LOG_DBG(origin, format, ...) ((void)0)
#define LOG_ERR(origin, format, ...) ((void)0)
#define LOG_INF(origin, format, ...) ((void)0)
