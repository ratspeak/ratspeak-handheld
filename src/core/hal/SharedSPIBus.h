#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

bool initializeSharedSPIBus();
struct SharedSPIStats { uint32_t maxWaitUs; uint32_t maxHoldUs; };
SharedSPIStats sharedSPIStats();

class SharedSPILock {
public:
    explicit SharedSPILock(TickType_t timeout = portMAX_DELAY);
    ~SharedSPILock();

    SharedSPILock(const SharedSPILock&) = delete;
    SharedSPILock& operator=(const SharedSPILock&) = delete;

    bool locked() const { return _locked; }

private:
    bool _locked = false;
    uint32_t _startUs = 0;
};
