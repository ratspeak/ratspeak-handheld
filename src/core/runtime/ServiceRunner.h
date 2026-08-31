#pragma once

#if !defined(RSCARDPUTER)
#include "DeviceService.h"

#ifndef DEVICE_SERVICE_TASK
#define DEVICE_SERVICE_TASK 0
#endif

namespace handheld {
class ServiceRunner {
public:
    explicit ServiceRunner(DeviceService& service) : _service(service) {}
    bool start();
    void cooperativeTick() { if (_started && !_threaded) _service.tick(); }
    bool threaded() const { return _threaded; }
private:
    static void task(void* context);
    DeviceService& _service;
    bool _threaded = false;
    bool _started = false;
};
} // namespace handheld
#endif
