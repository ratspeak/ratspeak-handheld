#pragma once

#include "runtime/TaskOwner.h"
#include "storage/CaptureQuiet.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <atomic>
#include <utility>

namespace handheld::storage {

// Only MessageStore's storage executor registers this task. No protocol, UI or
// configuration ownership changes when the storage adapters use this assertion.
inline std::atomic<TaskHandle_t> workerTask{nullptr};
inline bool isWorker() {
    const auto worker = workerTask.load(std::memory_order_acquire);
    return worker && worker == xTaskGetCurrentTaskHandle();
}
inline void assertOwner() { if (!isWorker()) handheld::assertDeviceOwner(); }
inline bool bindWorker() {
    const auto current = xTaskGetCurrentTaskHandle();
    TaskHandle_t expected = nullptr;
    const bool bound = workerTask.compare_exchange_strong(expected, current) || expected == current;
    configASSERT(bound);
    return bound;
}
inline void clearWorker() { workerTask.store(nullptr, std::memory_order_release); }

// Filesystem transaction ordering is separate from the SPI bus. The registered
// worker may wait for an explicit owner history lease; owner calls only try and
// defer. File/SPI primitives keep their much shorter bus critical sections.
class StorageLease {
public:
    explicit StorageLease(bool acquire = true) { if (acquire) tryAcquire(); }
    ~StorageLease() { release(); }
    StorageLease(const StorageLease&) = delete;
    StorageLease& operator=(const StorageLease&) = delete;
    StorageLease(StorageLease&& other) noexcept : _held(other._held) { other._held = false; }
    StorageLease& operator=(StorageLease&& other) noexcept {
        if (this != &other) { release(); _held = other._held; other._held = false; }
        return *this;
    }
    // Initialize on the setup owner, before constructing any storage worker.
    static bool initialize() {
        handheld::assertDeviceOwner();
        if (!_mutex) _mutex = xSemaphoreCreateRecursiveMutex();
        return _mutex != nullptr;
    }
    bool tryAcquire() {
        assertOwner();
        if (_held) return true;
        const bool worker=isWorker();
        do {
            if (!_mutex || xSemaphoreTakeRecursive(_mutex, worker ? portMAX_DELAY : 0) != pdTRUE) return false;
            if (_depth || CaptureQuiet::enter()) {
                ++_depth;_held=true;return true;
            }
            xSemaphoreGiveRecursive(_mutex);
            // Accepted executor jobs retain their queue credits while capture
            // runs. Owner calls defer normally; nested in-flight work can finish.
            if (worker) vTaskDelay(1);
        } while (worker);
        return false;
    }
    bool held() const { return _held; }
    void release() {
        if (_held) {
            if (!--_depth) CaptureQuiet::leave();
            xSemaphoreGiveRecursive(_mutex); _held = false;
        }
    }
private:
    inline static SemaphoreHandle_t _mutex = nullptr;
    inline static unsigned _depth = 0; // Protected by the recursive mutex.
    bool _held = false;
};

} // namespace handheld::storage
