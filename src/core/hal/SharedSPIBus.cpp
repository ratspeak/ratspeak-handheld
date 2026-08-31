#include "SharedSPIBus.h"
#include <atomic>

static std::atomic<uint32_t> maxWaitUs{0}, maxHoldUs{0};
static void recordMax(std::atomic<uint32_t>& metric, uint32_t value) {
    auto old = metric.load(std::memory_order_relaxed);
    while (value > old && !metric.compare_exchange_weak(old, value, std::memory_order_relaxed)) {}
}

static SemaphoreHandle_t sharedSPIMutexHandle() {
    static SemaphoreHandle_t sharedSPIMutex = xSemaphoreCreateRecursiveMutex();
    return sharedSPIMutex;
}

bool initializeSharedSPIBus() { return sharedSPIMutexHandle() != nullptr; }
SharedSPIStats sharedSPIStats() { return {maxWaitUs.load(), maxHoldUs.load()}; }

SharedSPILock::SharedSPILock(TickType_t timeout) {
    SemaphoreHandle_t mutex = sharedSPIMutexHandle();
    const auto started = micros();
    _locked = mutex && (xSemaphoreTakeRecursive(mutex, timeout) == pdTRUE);
    _startUs = micros();
    recordMax(maxWaitUs, _startUs - started);
}

SharedSPILock::~SharedSPILock() {
    if (_locked) {
        recordMax(maxHoldUs, micros() - _startUs);
        xSemaphoreGiveRecursive(sharedSPIMutexHandle());
    }
}
