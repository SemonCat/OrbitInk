#pragma once
// No-op host stand-in for lib/Logging/Logging.h. Nothing under test here asserts on log content
// (unlike test/kosync_protocol's Logging.h stub, which captures LOG_INF for exactly that reason).
#define LOG_DBG(origin, format, ...) ((void)0)
#define LOG_ERR(origin, format, ...) ((void)0)
#define LOG_INF(origin, format, ...) ((void)0)
