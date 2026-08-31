#include "SDStore.h"
#include "runtime/TaskOwner.h"
#include "config/Config.h"
#include "hal/SharedSPIBus.h"
#include "util/PerfTrace.h"
#include "SPIFile.h"

bool SDStore::begin(SPIClass* spi, int csPin) {
    handheld::assertDeviceOwner();
    if (!spi) return false;
    SharedSPILock lock;
    if (!lock.locked()) return false;

    // Deassert CS, then try mounting at conservative 4MHz first
    pinMode(csPin, OUTPUT);
    digitalWrite(csPin, HIGH);
    delay(10);

    if (!SD.begin(csPin, *spi, 4000000)) {
        Serial.printf("[SD] Mount failed (CS=%d), retrying...\n", csPin);
        delay(100);
        // Second attempt
        if (!SD.begin(csPin, *spi, 4000000)) {
            Serial.println("[SD] Card not detected or mount failed");
            _ready = false;
            return false;
        }
    }

    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
        Serial.println("[SD] No card inserted");
        _ready = false;
        return false;
    }

    const char* typeStr = "UNKNOWN";
    if (cardType == CARD_MMC)  typeStr = "MMC";
    if (cardType == CARD_SD)   typeStr = "SD";
    if (cardType == CARD_SDHC) typeStr = "SDHC";

    _ready = true;

    // Increase SPI speed for faster I/O after stable mount at 4MHz
    SD.end();
    if (!SD.begin(csPin, *spi, 16000000)) {
        // 16MHz failed, fall back to 4MHz
        if (!SD.begin(csPin, *spi, 4000000)) {
            Serial.println("[SD] 16MHz failed and 4MHz fallback failed");
            _ready = false;
            return false;
        }
        _ready = true;
        Serial.println("[SD] 16MHz failed, using 4MHz");
    } else {
        _ready = true;
        Serial.println("[SD] SPI speed: 16MHz");
    }

    Serial.printf("[SD] %s card ready, total=%llu MB, used=%llu MB\n",
                  typeStr, totalBytes() / (1024 * 1024), usedBytes() / (1024 * 1024));
    return true;
}

void SDStore::end() {
    handheld::assertDeviceOwner();
    SharedSPILock lock;
    if (!lock.locked()) return;
    SD.end();
    _ready = false;
}

uint64_t SDStore::totalBytes() const {
    handheld::assertDeviceOwner();
    if (!_ready) return 0;
    SharedSPILock lock;
    if (!lock.locked()) return 0;
    return SD.totalBytes();
}

uint64_t SDStore::usedBytes() const {
    handheld::assertDeviceOwner();
    if (!_ready) return 0;
    SharedSPILock lock;
    if (!lock.locked()) return 0;
    return SD.usedBytes();
}

bool SDStore::ensureDir(const char* path) {
    handheld::assertDeviceOwner();
    if (!_ready) return false;
    SharedSPILock lock;
    if (!lock.locked()) return false;
    if (SD.exists(path)) return true;
    // Create parent directories recursively
    String pathStr = String(path);
    int lastSlash = pathStr.lastIndexOf('/');
    if (lastSlash > 0) {
        String parent = pathStr.substring(0, lastSlash);
        if (!SD.exists(parent.c_str())) {
            ensureDir(parent.c_str());
        }
    }
    return SD.mkdir(path);
}

bool SDStore::exists(const char* path) {
    handheld::assertDeviceOwner();
    if (!_ready) return false;
    SharedSPILock lock;
    if (!lock.locked()) return false;
    return SD.exists(path);
}

bool SDStore::remove(const char* path) {
    handheld::assertDeviceOwner();
    if (!_ready) return false;
    SharedSPILock lock;
    if (!lock.locked()) return false;
    return SD.remove(path);
}

bool SDStore::rename(const char* from, const char* to) {
    handheld::assertDeviceOwner();
    if (!_ready) return false;
    SharedSPILock lock;
    if (!lock.locked()) return false;
    return SD.rename(from, to);
}

File SDStore::openDir(const char* path) {
    handheld::assertDeviceOwner();
    return openFile(path, FILE_READ);
}

File SDStore::openFile(const char* path, const char* mode) {
    handheld::assertDeviceOwner();
    if (!_ready) return File();
    SharedSPILock lock;
    if (!lock.locked()) return File();
    return sharedSPIFile(SD.open(path, mode));
}

bool SDStore::removeDir(const char* path) {
    handheld::assertDeviceOwner();
    if (!_ready) return false;
    SharedSPILock lock;
    if (!lock.locked()) return false;
    return SD.rmdir(path);
}

bool SDStore::readFile(const char* path, uint8_t* buffer, size_t maxLen, size_t& bytesRead) {
    handheld::assertDeviceOwner();
    bytesRead = 0;
    File f = openFile(path, FILE_READ);
    if (!f) return false;
    const size_t size = f.size();
    if (size > maxLen) return false;
    while (bytesRead < size) {
        const size_t count = std::min(size_t(1024), size - bytesRead);
        if (f.read(buffer + bytesRead, count) != count) return false;
        bytesRead += count;
        yield();
    }
    return true;
}

bool SDStore::writeAtomic(const char* path, const uint8_t* data, size_t len) {
    handheld::assertDeviceOwner();
    const unsigned long startMs = PerfTrace::nowMs();
    auto finish = [&](bool ok) { PerfTrace::write("sd", "atomic", path, len, startMs, ok); return ok; };
    if (!_ready) return finish(false);
#if defined(RSCARDPUTER)
    // The experimental frontend still has an async writer. Keep its transaction
    // serialization; Deck/Pager have one filesystem owner and release the bus
    // between actual file operations.
    SharedSPILock transaction;
    if (!transaction.locked()) return finish(false);
#endif
    const String tmpPath = String(path) + ".tmp", bakPath = String(path) + ".bak";
    File f = openFile(tmpPath.c_str(), FILE_WRITE);
    if (!f) return finish(false);
    size_t written = 0;
    while (written < len) {
        const size_t count = std::min(size_t(1024), len - written);
        if (f.write(data + written, count) != count) break;
        written += count;
        yield();
    }
    f.close();
    if (written != len) { remove(tmpPath.c_str()); return finish(false); }
    File verify = openFile(tmpPath.c_str(), FILE_READ);
    if (!verify || verify.size() != len) {
        verify.close(); remove(tmpPath.c_str()); return finish(false);
    }
    uint8_t check[512];
    for (size_t offset = 0; offset < len; offset += sizeof(check)) {
        const size_t count = std::min(sizeof(check), len - offset);
        if (verify.read(check, count) != count || memcmp(check, data + offset, count) != 0) {
            verify.close(); remove(tmpPath.c_str()); return finish(false);
        }
        yield();
    }
    verify.close();
    if (exists(path)) {
        if (exists(bakPath.c_str()) && !remove(bakPath.c_str())) return finish(false);
        // Never remove the current value if preparing the rollback copy failed.
        if (!rename(path, bakPath.c_str())) return finish(false);
    }
    if (!rename(tmpPath.c_str(), path)) {
        if (exists(bakPath.c_str())) rename(bakPath.c_str(), path);
        return finish(false);
    }
    if (exists(bakPath.c_str())) remove(bakPath.c_str());
    return finish(true);
}

bool SDStore::writeSimple(const char* path, const uint8_t* data, size_t len) {
    handheld::assertDeviceOwner();
    const unsigned long startMs = PerfTrace::nowMs();
    File f = openFile(path, FILE_WRITE);
    size_t written = 0;
    while (f && written < len) {
        const size_t count = std::min(size_t(1024), len - written);
        if (f.write(data + written, count) != count) break;
        written += count;
        yield();
    }
    const bool ok = f && written == len;
    f.close();
    PerfTrace::write("sd", "simple", path, len, startMs, ok);
    return ok;
}

bool SDStore::writeString(const char* path, const String& data) {
    handheld::assertDeviceOwner();
    const bool ok = writeAtomic(path, reinterpret_cast<const uint8_t*>(data.c_str()), data.length());
    return ok;
}

String SDStore::readString(const char* path) {
    handheld::assertDeviceOwner();
    File f = openFile(path, FILE_READ);
    if (!f) f = openFile((String(path) + ".bak").c_str(), FILE_READ);
    if (!f || f.size() > 32768) return "";
    const size_t size = f.size();
    String result;
    if (!result.reserve(size)) return "";
    char chunk[1024];
    size_t read = 0;
    while (read < size) {
        const size_t count = std::min(sizeof(chunk), size - read);
        if (f.read(reinterpret_cast<uint8_t*>(chunk), count) != count ||
            !result.concat(chunk, count)) return "";
        read += count;
        yield();
    }
    return result;
}

bool SDStore::wipeRsDeck() {
    handheld::assertDeviceOwner();
    if (!_ready) return false;
    bool ok = true;
    ok &= wipeDir(SD_PATH_MESSAGES);
    ok &= wipeDir(SD_PATH_CONTACTS);
    ok &= wipeDir(SD_PATH_IDENTITY_DIR);
    ok &= wipeDir(SD_PATH_CONFIG_DIR);
    ok &= wipeDir(SD_PATH_TRANSPORT);
    return ok && formatForRsDeck();
}

bool SDStore::wipeDir(const char* path) {
    handheld::assertDeviceOwner();
    if (!exists(path)) return true;
    File dir = openDir(path);
    if (!dir || !dir.isDirectory()) return false;
    bool ok = true;
    for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
        const String child = String(path) + "/" + entry.name();
        const bool directory = entry.isDirectory();
        entry.close();
        if (directory) ok &= wipeDir(child.c_str()) && removeDir(child.c_str());
        else ok &= remove(child.c_str());
        yield();
    }
    return ok;
}

bool SDStore::hasExistingData() {
    handheld::assertDeviceOwner();
    if (!_ready) return false;
    SharedSPILock lock;
    if (!lock.locked()) return false;
    if (SD.exists(SD_PATH_USER_CONFIG)) return true;
    if (SD.exists(SD_PATH_IDENTITY)) return true;
    File dir = SD.open(SD_PATH_MESSAGES);
    if (dir && dir.isDirectory()) {
        File entry = dir.openNextFile();
        bool found = (bool)entry;
        if (entry) entry.close();
        dir.close();
        if (found) return true;
    }
    return false;
}

bool SDStore::formatForRsDeck() {
    handheld::assertDeviceOwner();
    if (!_ready) return false;
    Serial.printf("[SD] Creating legacy %s/ directory structure...\n", SD_PATH_ROOT);
    bool ok = true;
    ok &= ensureDir(SD_PATH_ROOT);
    ok &= ensureDir(SD_PATH_CONFIG_DIR);
    ok &= ensureDir(SD_PATH_MESSAGES);
    ok &= ensureDir(SD_PATH_CONTACTS);
    ok &= ensureDir(SD_PATH_IDENTITY_DIR);
    ok &= ensureDir(SD_PATH_TRANSPORT);
    if (ok) Serial.println("[SD] Directory structure ready");
    return ok;
}
