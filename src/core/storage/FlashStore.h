#pragma once

#include <Arduino.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class FlashStore {
public:
    bool begin();
    void end();

    // Atomic write: .tmp → validate → rename .bak → rename .tmp → primary
    bool writeAtomic(const char* path, const uint8_t* data, size_t len);
    // Simple direct write (no rename dance — WriteQueue / small config files)
    bool writeDirect(const char* path, const uint8_t* data, size_t len);
    bool readFile(const char* path, uint8_t* buffer, size_t maxLen, size_t& bytesRead);
    // Read exactly one complete file into a bounded caller buffer. Unlike readFile(), this
    // rejects oversized files instead of returning a valid-looking prefix.
    bool readFileFully(const char* path, uint8_t* buffer, size_t maxLen, size_t& bytesRead);

    bool writeString(const char* path, const String& data);
    String readString(const char* path);

    bool ensureDir(const char* path);
    bool exists(const char* path);
    bool remove(const char* path);
    bool removeDir(const char* path);
    bool rename(const char* from, const char* to);
    File openDir(const char* path);
    File openFile(const char* path, const char* mode = "r");

    // String overloads (avoid .c_str() at every call site)
    bool remove(const String& p) { return remove(p.c_str()); }
    bool removeDir(const String& p) { return removeDir(p.c_str()); }
    bool rename(const String& f, const String& t) { return rename(f.c_str(), t.c_str()); }
    File openDir(const String& p) { return openDir(p.c_str()); }

    bool format();

    bool isReady() const { return _ready; }
    size_t totalBytes() const { return _ready ? LittleFS.totalBytes() : 0; }
    size_t usedBytes() const { return _ready ? LittleFS.usedBytes() : 0; }

    // Global LittleFS mutex — serializes WriteQueue and direct callers
    // and the WriteQueue task. Created on first begin().
    static SemaphoreHandle_t mutex();

private:
    void recoverAtomicArtifacts();
    bool _ready = false;
    static SemaphoreHandle_t _mutex;
};

// RAII guard for the global LittleFS mutex (main loop vs WriteQueue task).
struct FSLock {
    FSLock() : _m(FlashStore::mutex()) {
        if (_m) xSemaphoreTake(_m, portMAX_DELAY);
    }
    ~FSLock() {
        if (_m) xSemaphoreGive(_m);
    }
    SemaphoreHandle_t _m;
};
