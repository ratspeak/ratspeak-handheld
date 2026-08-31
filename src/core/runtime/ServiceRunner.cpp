#include "ServiceRunner.h"
#if !defined(RSCARDPUTER)
namespace handheld {
bool ServiceRunner::start() {
    _started = true;
#if DEVICE_SERVICE_TASK
    _threaded = xTaskCreatePinnedToCore(task, "device-service", 24 * 1024,
                                      this, 1, nullptr, 0) == pdPASS;
    if (_threaded) {
        Serial.println("[SERVICE] Dedicated network/storage task started");
        return true;
    }
    Serial.println("[SERVICE] Task allocation failed; cooperative recovery mode");
    _service.notice("Service task unavailable; cooperative recovery mode");
#endif
    _service.bindOwner();
    return true;
}
void ServiceRunner::task(void* context) {
    auto& self = *static_cast<ServiceRunner*>(context);
    self._service.bindOwner();
    for (;;) {
        self._service.tick();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
} // namespace handheld
#endif
