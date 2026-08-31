#pragma once
#if !defined(RSCARDPUTER)
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
namespace handheld {
inline std::atomic<TaskHandle_t> deviceOwner{nullptr};
inline void bindDeviceOwner() { deviceOwner.store(xTaskGetCurrentTaskHandle(), std::memory_order_release); }
inline void assertDeviceOwner() {
    const auto owner = deviceOwner.load(std::memory_order_acquire);
    configASSERT(!owner || owner == xTaskGetCurrentTaskHandle());
}
}
#else
namespace handheld { inline void assertDeviceOwner() {} }
#endif
