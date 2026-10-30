#pragma once
#include <mutex>
using SemaphoreHandle_t = std::mutex*;
using TaskHandle_t = void*;
inline TaskHandle_t testTask = reinterpret_cast<void*>(1);
inline TaskHandle_t xTaskGetCurrentTaskHandle() { return testTask; }
inline SemaphoreHandle_t xSemaphoreCreateMutex() { static std::mutex mutex; return &mutex; }
inline void xSemaphoreTake(SemaphoreHandle_t mutex, int) { mutex->lock(); }
inline void xSemaphoreGive(SemaphoreHandle_t mutex) { mutex->unlock(); }
constexpr int portMAX_DELAY = 0;
