#include "config/Config.h"

// Deferred SD/flash writes off the main loop (0.5-4s LittleFS writes on the
// no-PSRAM board). Compiled only where a BoardConfig opts in.
#if STORAGE_ASYNC_WRITES

#include "WriteQueue.h"
#include "storage/SDStore.h"
#include "storage/FlashStore.h"

bool WriteQueue::begin(SDStore* sd, FlashStore* flash) {
    _sd = sd;
    _flash = flash;

    _queue = xQueueCreate(QUEUE_DEPTH, sizeof(WriteJob*));
    if (!_queue) {
        Serial.println("[WRITEQ] Failed to create queue");
        return false;
    }

    // Pin to Core 1 (same as main loop) — LittleFS is NOT thread-safe across cores
    xTaskCreatePinnedToCore(taskFunc, "WriteQ", TASK_STACK, this, TASK_PRIORITY, &_task, 1);
    if (!_task) {
        Serial.println("[WRITEQ] Failed to create task");
        return false;
    }

    Serial.println("[WRITEQ] Async write queue started on Core 1");
    return true;
}

bool WriteQueue::enqueue(const char* sdPath, const char* flashPath, const String& data, WriteBackend backend) {
    if (!_queue) return false;

    WriteJob* job = new (std::nothrow) WriteJob();
    if (!job) return false;

    job->sdPath[0] = '\0';
    job->flashPath[0] = '\0';
    if (sdPath) {
        strncpy(job->sdPath, sdPath, sizeof(job->sdPath) - 1);
        job->sdPath[sizeof(job->sdPath) - 1] = '\0';
    }
    if (flashPath) {
        strncpy(job->flashPath, flashPath, sizeof(job->flashPath) - 1);
        job->flashPath[sizeof(job->flashPath) - 1] = '\0';
    }
    job->data = data;
    job->backend = backend;

    if (xQueueSend(_queue, &job, 0) != pdTRUE) {
        delete job;
        Serial.println("[WRITEQ] Queue full, dropping write");
        return false;
    }

    _pending++;
    return true;
}

bool WriteQueue::enqueue(const char* path, const String& data, WriteBackend backend) {
    if (backend == WriteBackend::SD_ONLY) {
        return enqueue(path, nullptr, data, backend);
    } else if (backend == WriteBackend::FLASH_ONLY) {
        return enqueue(nullptr, path, data, backend);
    }
    // BOTH requires dual-path overload
    return enqueue(path, path, data, backend);
}

void WriteQueue::waitForFlush(unsigned long timeoutMs) {
    unsigned long start = millis();
    while (_pending > 0 && (millis() - start) < timeoutMs) {
        delay(5);  // Yield to let WriteQueue task process
    }
    if (_pending > 0) {
        Serial.printf("[WRITEQ] Flush timeout (%d pending)\n", (int)_pending);
    }
}

bool WriteQueue::isFull() const {
    if (!_queue) return true;
    return uxQueueSpacesAvailable(_queue) == 0;
}

void WriteQueue::taskFunc(void* param) {
    WriteQueue* self = static_cast<WriteQueue*>(param);
    WriteJob* job = nullptr;

    for (;;) {
        if (xQueueReceive(self->_queue, &job, portMAX_DELAY) == pdTRUE) {
            self->processJob(*job);
            delete job;
            if (self->_pending > 0) self->_pending--;
        }
    }
}

void WriteQueue::processJob(const WriteJob& job) {
    // SD write
    if (job.backend == WriteBackend::SD_ONLY || job.backend == WriteBackend::BOTH) {
        bool backendOk = false;
        if (_sd && _sd->isReady() && job.sdPath[0] != '\0') {
            // Ensure parent directory exists on SD
            String sdDir = String(job.sdPath);
            int lastSlash = sdDir.lastIndexOf('/');
            if (lastSlash > 0) {
                sdDir = sdDir.substring(0, lastSlash);
                _sd->ensureDir(sdDir.c_str());
            }
            backendOk = _sd->writeDirect(job.sdPath, (const uint8_t*)job.data.c_str(), job.data.length());
        }
        if (!backendOk) {
            Serial.printf("[WRITEQ] SD write FAILED: %s\n", job.sdPath);
        }
    }

    // Flash write
    if (job.backend == WriteBackend::FLASH_ONLY || job.backend == WriteBackend::BOTH) {
        bool backendOk = false;
        if (_flash && _flash->isReady() && job.flashPath[0] != '\0') {
            // Ensure parent directory exists on flash
            String flashDir = String(job.flashPath);
            int lastSlash = flashDir.lastIndexOf('/');
            if (lastSlash > 0) {
                flashDir = flashDir.substring(0, lastSlash);
                _flash->ensureDir(flashDir.c_str());
            }
            backendOk = _flash->writeDirect(job.flashPath, (const uint8_t*)job.data.c_str(), job.data.length());
        }
        if (!backendOk) {
            Serial.printf("[WRITEQ] Flash write FAILED: %s\n", job.flashPath);
        }
    }
}

// Counter persistence moved to MessageStore::saveMessage (crash-safe reservation
// ceiling, main task): the 30s batch here left a reuse window after a crash.

#endif  // STORAGE_ASYNC_WRITES
