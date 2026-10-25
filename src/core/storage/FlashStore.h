#pragma once

#include <Arduino.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
namespace handheld::storage { enum class Error : uint8_t; class AtomicSource; }

class FlashStore {
public:
    bool begin();
    void end();

    // Atomic write: .tmp → validate → rename .bak → rename .tmp → primary
    bool writeAtomic(const char* path, const uint8_t* data, size_t len);
    handheld::storage::Error writeAtomic(const char* path, const handheld::storage::AtomicSource& source);
    // Simple direct write (no rename dance — WriteQueue / small config files)
    bool writeDirect(const char* path, const uint8_t* data, size_t len);
    bool readFile(const char* path, uint8_t* buffer, size_t maxLen, size_t& bytesRead);
    // Read exactly one complete file into a bounded caller buffer. Unlike readFile(), this
    // rejects oversized files instead of returning a valid-looking prefix.
    bool readFileFully(const char* path, uint8_t* buffer, size_t maxLen, size_t& bytesRead);

    bool writeString(const char* path, const String& data);
    String readString(const char* path);
    // Recovery-sensitive records never hide an existing primary open failure
    // behind a backup. Backup is considered only when primary is absent.
    enum class RecordSource : uint8_t { Absent, Primary, Backup, Unavailable, Invalid };
    RecordSource readRecord(const char* path, String& out, size_t maxBytes = 32768,
                            bool (*reserve)(String&, size_t) = nullptr);

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

    // A mounted reset intent blocks normal begin() before config/identity
    // recovery. Only an explicitly confirmed, quiescent reset may use these.
    enum class ResetState : uint8_t { Clear, Pending, Unavailable };
    ResetState resetState() const { return _resetState; }
    bool resetScope(bool& includesSD);
    bool prepareReset(bool includesSD, bool replaceInvalidScope = false);
    bool wipeForReset();
    bool completeReset();

    // Why begin() did not mount. Unprepared: blank, or leftovers without a
    // LittleFS on our own partition, so nothing of anyone's is at stake and
    // prepare() runs unasked. Damaged/Foreign may hold someone's data: erase
    // them only on the user's explicit choice.
    enum class Startup : uint8_t { Ready, Missing, Unprepared, Damaged, Foreign, Failed };
    Startup startup() const { return _startup; }
    bool prepare();

    bool isReady() const { return _ready; }
    size_t totalBytes() const;
    size_t usedBytes() const;

    // Global LittleFS mutex — serializes WriteQueue and direct callers
    // and the WriteQueue task. Created on first begin().
    static SemaphoreHandle_t mutex();

private:
    void recoverAtomicArtifacts();
    bool _ready = false;
    ResetState _resetState = ResetState::Unavailable;
    Startup _startup = Startup::Failed;
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
