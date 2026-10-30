#pragma once
constexpr int SNTP_SYNC_STATUS_COMPLETED = 1;
inline int hostNtpStatus = 0, hostNtpRequests = 0;
inline int sntp_get_sync_status() { return hostNtpStatus; }
inline void configTzTime(const char*, const char*, const char*) { ++hostNtpRequests; }
