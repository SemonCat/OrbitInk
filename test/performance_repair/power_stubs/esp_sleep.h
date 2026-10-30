#pragma once
constexpr int ESP_SLEEP_WAKEUP_TIMER = 0;
inline void esp_sleep_disable_wakeup_source(int) {}
inline void esp_sleep_enable_timer_wakeup(uint64_t) {}
