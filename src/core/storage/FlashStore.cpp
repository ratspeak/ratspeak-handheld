#include "FlashStore.h"
#include "runtime/TaskOwner.h"
#include "util/PerfTrace.h"
#include <esp_partition.h>
#include <algorithm>

// Global LittleFS mutex — prevents cross-task corruption (WriteQueue task)
SemaphoreHandle_t FlashStore::_mutex = nullptr;

SemaphoreHandle_t FlashStore::mutex() {
    handheld::assertDeviceOwner();
    return _mutex;
}

namespace {
// Legacy standalone builds label this partition "littlefs"; bmorcelli/Launcher
// labels the equivalent partition "spiffs". Try ours first, fall back.
const char* FLASH_PARTITION_LABELS[] = {"littlefs", "spiffs"};
constexpr const char* FLASH_BASE_PATH = "/littlefs";

const esp_partition_t* dataPartition() {
    for (const char* label : FLASH_PARTITION_LABELS) {
        const auto* partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                        ESP_PARTITION_SUBTYPE_ANY, label);
        if (partition) return partition;
    }
    return nullptr;
}

bool isBlankPartition(const esp_partition_t* partition) {
    uint8_t bytes[512];
    for (size_t offset = 0; offset < partition->size; offset += sizeof(bytes)) {
        const size_t size = std::min(sizeof(bytes), size_t(partition->size - offset));
        if (esp_partition_read(partition, offset, bytes, size) != ESP_OK) return false;
        for (size_t i = 0; i < size; ++i) if (bytes[i] != 0xff) return false;
        yield();
    }
    return true;
}

bool ensureDirLocked(const char* path) {
    if (!path || path[0] == '\0' || strcmp(path, "/") == 0) return true;
    if (LittleFS.exists(path)) return true;

    String pathStr(path);
    int lastSlash = pathStr.lastIndexOf('/');
    if (lastSlash > 0) {
        String parent = pathStr.substring(0, lastSlash);
        if (!ensureDirLocked(parent.c_str())) return false;
    }
    return LittleFS.mkdir(path) || LittleFS.exists(path);
}

String fullPathForEntry(const char* dirPath, const char* entryName) {
    if (!entryName || entryName[0] == '\0') return "";
    String name(entryName);
    if (name.startsWith("/")) return name;
    String dir(dirPath);
    if (dir.endsWith("/")) return dir + name;
    return dir + "/" + name;
}

void recoverAtomicArtifact(const String& artifactPath,
                           int& recovered,
                           int& removedTmp,
                           int& removedBak) {
    if (artifactPath.endsWith(".tmp")) {
        if (LittleFS.remove(artifactPath.c_str())) removedTmp++;
        return;
    }

    if (!artifactPath.endsWith(".bak")) return;

    String primaryPath = artifactPath.substring(0, artifactPath.length() - 4);
    if (LittleFS.exists(primaryPath.c_str())) {
        // The storage layer cannot validate config/key payloads. Retain this
        // rollback copy until a verified replacement write supersedes it.
        (void)removedBak;
        return;
    }

    if (LittleFS.rename(artifactPath.c_str(), primaryPath.c_str())) {
        recovered++;
        Serial.printf("[FLASH] Recovered backup: %s\n", primaryPath.c_str());
    } else {
        Serial.printf("[FLASH] Failed to recover backup: %s\n", artifactPath.c_str());
    }
}

void recoverArtifactsInDir(const char* dirPath,
                           int& recovered,
                           int& removedTmp,
                           int& removedBak) {
    File dir = LittleFS.open(dirPath);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return;
    }

    File entry = dir.openNextFile();
    while (entry) {
        String fullPath = fullPathForEntry(dirPath, entry.name());
        bool isArtifact = fullPath.endsWith(".tmp") || fullPath.endsWith(".bak");
        entry.close();
        if (isArtifact) {
            recoverAtomicArtifact(fullPath, recovered, removedTmp, removedBak);
        }
        entry = dir.openNextFile();
    }
    dir.close();
}
}  // namespace

bool FlashStore::begin() {
    handheld::assertDeviceOwner();
    // Create mutex before any LittleFS access
    if (!_mutex) {
        _mutex = xSemaphoreCreateMutex();
    }
    if (!_mutex) return false;

    FSLock lock;
    _ready = false;

    const auto* partition = dataPartition();
    if (!partition) return false;
    if (!LittleFS.begin(false, FLASH_BASE_PATH, 10, partition->label)) {
        // Only positively identified, completely erased factory media may be
        // initialized automatically. Existing or unreadable data stays intact.
        if (!isBlankPartition(partition)) {
            Serial.println("[FLASH] Mount failed; existing data preserved for recovery");
            return false;
        }
        if (!LittleFS.begin(true, FLASH_BASE_PATH, 10, partition->label)) return false;
    }
    _ready = true;

    ensureDirLocked("/identity");
    ensureDirLocked("/transport");
    ensureDirLocked("/config");
    ensureDirLocked("/contacts");
    ensureDirLocked("/messages");

    Serial.printf("[FLASH] LittleFS ready, total=%lu, used=%lu\n",
                  (unsigned long)LittleFS.totalBytes(),
                  (unsigned long)LittleFS.usedBytes());

    recoverAtomicArtifacts();

    return true;
}

// Interrupted-writeAtomic cleanup for the small config-class dirs: drop stale
// .tmp, restore orphaned .bak (primary lost mid-rename). Deliberately excludes
// /messages to avoid an extra full history walk at boot. MessageStore discovers
// backup-only files and validates both media while reading history.
void FlashStore::recoverAtomicArtifacts() {
    handheld::assertDeviceOwner();
    static const char* dirs[] = {"/config", "/contacts", "/identity", "/transport"};
    int recovered = 0;
    int removedTmp = 0;
    int removedBak = 0;

    for (const char* dirPath : dirs) {
        recoverArtifactsInDir(dirPath, recovered, removedTmp, removedBak);
    }

    if (recovered || removedTmp || removedBak) {
        Serial.printf("[FLASH] Atomic recovery: restored=%d removed_tmp=%d removed_bak=%d\n",
                      recovered, removedTmp, removedBak);
    }
}

void FlashStore::end() {
    handheld::assertDeviceOwner();
    FSLock lock;
    LittleFS.end();
    _ready = false;
}

bool FlashStore::ensureDir(const char* path) {
    handheld::assertDeviceOwner();
    if (!_ready) return false;
    FSLock lock;
    return ensureDirLocked(path);
}

bool FlashStore::exists(const char* path) {
    handheld::assertDeviceOwner();
    if (!_ready) return false;
    FSLock lock;
    return LittleFS.exists(path);
}

bool FlashStore::remove(const char* path) {
    handheld::assertDeviceOwner();
    if (!_ready) return false;
    FSLock lock;
    return LittleFS.remove(path);
}

bool FlashStore::removeDir(const char* path) {
    handheld::assertDeviceOwner();
    if (!_ready) return false;
    FSLock lock;
    return LittleFS.rmdir(path);
}

bool FlashStore::rename(const char* from, const char* to) {
    handheld::assertDeviceOwner();
    if (!_ready) return false;
    FSLock lock;
    return LittleFS.rename(from, to);
}

File FlashStore::openDir(const char* path) {
    handheld::assertDeviceOwner();
    if (!_ready) return File();
    FSLock lock;
    return LittleFS.open(path);
}

File FlashStore::openFile(const char* path, const char* mode) {
    handheld::assertDeviceOwner();
    if (!_ready) return File();
    FSLock lock;
    return LittleFS.open(path, mode);
}

bool FlashStore::writeAtomic(const char* path, const uint8_t* data, size_t len) {
    handheld::assertDeviceOwner();
    unsigned long startMs = PerfTrace::nowMs();
    if (!_ready) {
        PerfTrace::write("flash", "atomic", path, len, startMs, false);
        return false;
    }
    // Refuse writes when heap is critically low — prevents OOM-induced LittleFS unmount
    if (ESP.getFreeHeap() < 4096) {
        Serial.printf("[FLASH] Write refused (heap=%lu) — OOM protection: %s\n",
                      (unsigned long)ESP.getFreeHeap(), path);
        PerfTrace::write("flash", "atomic", path, len, startMs, false);
        return false;
    }
    FSLock lock;

    String tmpPath = String(path) + ".tmp";
    String bakPath = String(path) + ".bak";

    File f = LittleFS.open(tmpPath.c_str(), "w");
    if (!f) {
        PerfTrace::write("flash", "atomic", path, len, startMs, false);
        return false;
    }
    size_t written = f.write(data, len);
    f.close();
    if (written != len) {
        LittleFS.remove(tmpPath.c_str());
        PerfTrace::write("flash", "atomic", path, len, startMs, false);
        return false;
    }

    File verify = LittleFS.open(tmpPath.c_str(), "r");
    if (!verify || verify.size() != len) {
        if (verify) verify.close();
        LittleFS.remove(tmpPath.c_str());
        PerfTrace::write("flash", "atomic", path, len, startMs, false);
        return false;
    }
    uint8_t check[512];
    for (size_t offset = 0; offset < len; offset += sizeof(check)) {
        const size_t count = std::min(sizeof(check), len - offset);
        if (verify.readBytes(reinterpret_cast<char*>(check), count) != count ||
            memcmp(check, data + offset, count) != 0) {
            verify.close(); LittleFS.remove(tmpPath.c_str());
            return false;
        }
    }
    verify.close();

    if (LittleFS.exists(path)) {
        if (LittleFS.exists(bakPath.c_str()) && !LittleFS.remove(bakPath.c_str())) return false;
        if (!LittleFS.rename(path, bakPath.c_str())) {
            LittleFS.remove(tmpPath.c_str());
            Serial.printf("[FLASH] writeAtomic: backup rename failed for %s\n", path);
            PerfTrace::write("flash", "atomic", path, len, startMs, false);
            return false;
        }
    }

    if (!LittleFS.rename(tmpPath.c_str(), path)) {
        if (LittleFS.exists(bakPath.c_str())) {
            LittleFS.rename(bakPath.c_str(), path);
        }
        LittleFS.remove(tmpPath.c_str());
        PerfTrace::write("flash", "atomic", path, len, startMs, false);
        return false;
    }

    // Clean up backup file after successful write
    LittleFS.remove(bakPath.c_str());

    PerfTrace::write("flash", "atomic", path, len, startMs, true);
    return true;
}

bool FlashStore::writeDirect(const char* path, const uint8_t* data, size_t len) {
    handheld::assertDeviceOwner();
    unsigned long startMs = PerfTrace::nowMs();
    if (!_ready) {
        PerfTrace::write("flash", "direct", path, len, startMs, false);
        return false;
    }
    if (ESP.getFreeHeap() < 4096) {
        Serial.printf("[FLASH] Write refused (heap=%lu) — OOM protection: %s\n",
                      (unsigned long)ESP.getFreeHeap(), path);
        PerfTrace::write("flash", "direct", path, len, startMs, false);
        return false;
    }
    FSLock lock;
    File f = LittleFS.open(path, "w");
    if (!f) {
        PerfTrace::write("flash", "direct", path, len, startMs, false);
        return false;
    }
    size_t written = f.write(data, len);
    f.flush();
    f.close();
    PerfTrace::write("flash", "direct", path, len, startMs, written == len);
    return written == len;
}

bool FlashStore::readFile(const char* path, uint8_t* buffer, size_t maxLen, size_t& bytesRead) {
    return readFileFully(path, buffer, maxLen, bytesRead);
}

bool FlashStore::readFileFully(const char* path, uint8_t* buffer, size_t maxLen,
                               size_t& bytesRead) {
    handheld::assertDeviceOwner();
    bytesRead = 0;
    if (!_ready || !path || !buffer) return false;
    FSLock lock;

    File f = LittleFS.open(path, "r");
    if (!f) {
        String bakPath = String(path) + ".bak";
        f = LittleFS.open(bakPath.c_str(), "r");
        if (!f) return false;
        Serial.printf("[FLASH] Read complete backup: %s\n", path);
    }

    const size_t fileLen = f.size();
    if (fileLen == 0 || fileLen > maxLen) {
        f.close();
        return false;
    }
    bytesRead = f.readBytes((char*)buffer, fileLen);
    f.close();
    if (bytesRead != fileLen) {
        bytesRead = 0;
        return false;
    }
    return true;
}

bool FlashStore::writeString(const char* path, const String& data) {
    handheld::assertDeviceOwner();
    return writeAtomic(path, (const uint8_t*)data.c_str(), data.length());
}

String FlashStore::readString(const char* path) {
    handheld::assertDeviceOwner();
    if (!_ready) return "";
    FSLock lock;
    File f = LittleFS.open(path, "r");
    if (!f) {
        String bakPath = String(path) + ".bak";
        f = LittleFS.open(bakPath.c_str(), "r");
        if (!f) return "";
    }
    // Guard against corrupted/huge files exhausting heap.
    // 32KB clears the worst legit case (names.json at 300 cached entries).
    if (f.size() > 32768) {
        Serial.printf("[FLASH] readString: file too large (%d bytes): %s\n", (int)f.size(), path);
        f.close();
        return "";
    }
    const size_t size = f.size();
    String result;
    if (!result.reserve(size)) return "";
    char bytes[512];
    for (size_t offset = 0; offset < size; offset += sizeof(bytes)) {
        const size_t count = std::min(sizeof(bytes), size - offset);
        if (f.readBytes(bytes, count) != count || !result.concat(bytes, count)) return "";
    }
    f.close();
    return result;
}

bool FlashStore::format() {
    handheld::assertDeviceOwner();
    Serial.println("[FLASH] Formatting LittleFS...");
    bool ok;
    {
        FSLock lock;
        LittleFS.end();
        _ready = false;
        ok = LittleFS.format();
    }
    // Re-mount with fresh filesystem (begin() takes its own lock)
    if (ok) ok = begin();
    return ok;
}
