#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

class SDStore;
class FlashStore;

enum class WriteBackend : uint8_t { SD_ONLY, FLASH_ONLY, BOTH };

struct WriteJob {
    char sdPath[128];
    char flashPath[128];
    String data;
    WriteBackend backend;
};

class WriteQueue {
public:
    bool begin(SDStore* sd, FlashStore* flash);

    // Enqueue a write job. For BOTH, provide sdPath and flashPath.
    // For SD_ONLY or FLASH_ONLY, the relevant path field is used.
    bool enqueue(const char* sdPath, const char* flashPath, const String& data, WriteBackend backend);

    // Convenience: single-path enqueue
    bool enqueue(const char* path, const String& data, WriteBackend backend = WriteBackend::SD_ONLY);

    int drainCount() const { return _pending; }
    bool isFull() const;
    void waitForFlush(unsigned long timeoutMs = 200);  // Block until queue is empty or timeout

private:
    static void taskFunc(void* param);
    void processJob(const WriteJob& job);

    QueueHandle_t _queue = nullptr;
    TaskHandle_t _task = nullptr;
    SDStore* _sd = nullptr;
    FlashStore* _flash = nullptr;
    volatile int _pending = 0;

    static constexpr int QUEUE_DEPTH = 32;
    static constexpr int TASK_STACK = 8192;
    static constexpr int TASK_PRIORITY = 1;
};
