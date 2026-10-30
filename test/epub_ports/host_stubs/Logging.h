#pragma once

template <typename... Args>
inline void hostDictionaryLog(Args&&...) {}

#define LOG_ERR(...) hostDictionaryLog(__VA_ARGS__)
#define LOG_INF(...) hostDictionaryLog(__VA_ARGS__)
#define LOG_DBG(...) hostDictionaryLog(__VA_ARGS__)
