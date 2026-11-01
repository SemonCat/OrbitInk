#pragma once
constexpr int ESP_SLEEP_WAKEUP_TIMER = 0;
inline void esp_sleep_disable_wakeup_source(int) {}
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
inline esp_err_t esp_sleep_enable_timer_wakeup(uint64_t) { return ESP_OK; }
