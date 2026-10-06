#include "SDStore.h"
#include "storage/StorageLease.h"
#include "storage/AtomicStream.h"
#include "storage/TreeWipe.h"
#include "config/Config.h"
#include "hal/SharedSPIBus.h"
#include "util/PerfTrace.h"
#include "SPIFile.h"
#if SD_USE_MMC
#include <SD_MMC.h>
#define SD_CARD SD_MMC
#else
#define SD_CARD SD
#endif

bool SDStore::begin(SPIClass* spi, int csPin) {
    handheld::storage::assertOwner();
    if (!handheld::storage::StorageLease::initialize()) return false;
    handheld::storage::StorageLease lease;
    if (!lease.held()) return false;
    if (!spi) return false;
    SharedSPILock lock;
    if (!lock.locked()) return false;

#if SD_USE_MMC
    // One-bit SDMMC on dedicated pins; the SPI bus and CS are unused.
    (void)csPin;
    SD_CARD.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_D0);
    if (!SD_CARD.begin("/sdcard", true)) {
        Serial.println("[SD] SDMMC mount failed, retrying...");
        delay(100);
        if (!SD_CARD.begin("/sdcard", true)) {
            Serial.println("[SD] Card not detected or mount failed");
            _ready = false;
            return false;
        }
    }
    if (SD_CARD.cardType() == CARD_NONE) {
        Serial.println("[SD] No card inserted");
        SD_CARD.end();
        _ready = false;
        return false;
    }
    _ready = true;
    Serial.printf("[SD] SDMMC card ready, total=%llu MB, used=%llu MB\n",
                  totalBytes() / (1024 * 1024), usedBytes() / (1024 * 1024));
    return true;
#else
    // Deassert CS, then try mounting at conservative 4MHz first
    pinMode(csPin, OUTPUT);
    digitalWrite(csPin, HIGH);
    delay(10);

    if (!SD_CARD.begin(csPin, *spi, 4000000)) {
        Serial.printf("[SD] Mount failed (CS=%d), retrying...\n", csPin);
        delay(100);
        // Second attempt
        if (!SD_CARD.begin(csPin, *spi, 4000000)) {
            Serial.println("[SD] Card not detected or mount failed");
            _ready = false;
            return false;
        }
    }

    uint8_t cardType = SD_CARD.cardType();
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
    SD_CARD.end();
    if (!SD_CARD.begin(csPin, *spi, 16000000)) {
        // 16MHz failed, fall back to 4MHz
        if (!SD_CARD.begin(csPin, *spi, 4000000)) {
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
#endif
}

void SDStore::end() {
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return ;
    SharedSPILock lock;
    if (!lock.locked()) return;
    SD_CARD.end();
    _ready = false;
}

uint64_t SDStore::totalBytes() const {
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return 0;
    if (!_ready) return 0;
    SharedSPILock lock;
    if (!lock.locked()) return 0;
    return SD_CARD.totalBytes();
}

uint64_t SDStore::usedBytes() const {
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return 0;
    if (!_ready) return 0;
    SharedSPILock lock;
    if (!lock.locked()) return 0;
    return SD_CARD.usedBytes();
}

bool SDStore::ensureDir(const char* path) {
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return false;
    if (!_ready) return false;
    SharedSPILock lock;
    if (!lock.locked()) return false;
    if (SD_CARD.exists(path)) return true;
    // Create parent directories recursively
    String pathStr = String(path);
    int lastSlash = pathStr.lastIndexOf('/');
    if (lastSlash > 0) {
        String parent = pathStr.substring(0, lastSlash);
        if (!SD_CARD.exists(parent.c_str())) {
            ensureDir(parent.c_str());
        }
    }
    return SD_CARD.mkdir(path);
}

bool SDStore::exists(const char* path) {
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return false;
    if (!_ready) return false;
    SharedSPILock lock;
    if (!lock.locked()) return false;
    return SD_CARD.exists(path);
}

bool SDStore::remove(const char* path) {
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return false;
    if (!_ready) return false;
    SharedSPILock lock;
    if (!lock.locked()) return false;
    return SD_CARD.remove(path);
}

bool SDStore::rename(const char* from, const char* to) {
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return false;
    if (!_ready) return false;
    SharedSPILock lock;
    if (!lock.locked()) return false;
    return SD_CARD.rename(from, to);
}

File SDStore::openDir(const char* path) {
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return File();
    return openFile(path, FILE_READ);
}

File SDStore::openFile(const char* path, const char* mode) {
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return File();
    if (!_ready) return File();
    SharedSPILock lock;
    if (!lock.locked()) return File();
    return sharedSPIFile(SD_CARD.open(path, mode));
}

bool SDStore::removeDir(const char* path) {
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return false;
    if (!_ready) return false;
    SharedSPILock lock;
    if (!lock.locked()) return false;
    return SD_CARD.rmdir(path);
}

bool SDStore::readFile(const char* path, uint8_t* buffer, size_t maxLen, size_t& bytesRead) {
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return false;
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
    handheld::storage::MemorySource source(data, len);
    return writeAtomic(path, source) == handheld::storage::Error::None;
}

handheld::storage::Error SDStore::writeAtomic(const char* path, const handheld::storage::AtomicSource& source) {
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return handheld::storage::Error::Unavailable;
    const unsigned long start = PerfTrace::nowMs();
    const auto error = handheld::storage::atomicStream(*this, path, source);
    PerfTrace::write("sd", "atomic", path, source.length(), start,
                     error == handheld::storage::Error::None);
    return error;
}

bool SDStore::writeSimple(const char* path, const uint8_t* data, size_t len) {
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return false;
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
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return false;
    const bool ok = writeAtomic(path, reinterpret_cast<const uint8_t*>(data.c_str()), data.length());
    return ok;
}

String SDStore::readString(const char* path, bool (*reserve)(String&, size_t), bool* admissionRefused) {
    if (admissionRefused) *admissionRefused = false;
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return String();
    File f = openFile(path, FILE_READ);
    if (!f) f = openFile((String(path) + ".bak").c_str(), FILE_READ);
    if (!f || f.size() > 32768) return "";
    const size_t size = f.size();
    String result;
    if (!(reserve ? reserve(result, size) : result.reserve(size))) {
        if (admissionRefused && reserve) *admissionRefused = true;
        return "";
    }
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
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return false;
    if (!_ready) return false;
    // Establish known directories even after an interrupted earlier wipe.
    // This also rejects regular files occupying required directory paths.
    if (!formatForRsDeck()) return false;
    const char* paths[] = {SD_PATH_MESSAGES, SD_PATH_CONTACTS,
        SD_PATH_IDENTITY_DIR, SD_PATH_CONFIG_DIR, SD_PATH_TRANSPORT};
    for (const char* path : paths) {
        // openNextFile() can return an empty handle on allocation/read failure,
        // just as it does at EOF. A successful filesystem rmdir additionally
        // proves that no child was silently skipped by the recursive walker.
        if (!wipeDir(path) || !removeDir(path)) return false;
    }
    return formatForRsDeck();
}

bool SDStore::wipeDir(const char* path) {
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return false;
    if (!exists(path)) return true;
    return handheld::storage::wipeTreeContents(*this, path);
}

bool SDStore::hasExistingData() {
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return false;
    if (!_ready) return false;
    SharedSPILock lock;
    if (!lock.locked()) return false;
    if (SD_CARD.exists(SD_PATH_USER_CONFIG)) return true;
    if (SD_CARD.exists(SD_PATH_IDENTITY)) return true;
    File dir = SD_CARD.open(SD_PATH_MESSAGES);
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
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return false;
    if (!_ready) return false;
    Serial.printf("[SD] Creating legacy %s/ directory structure...\n", SD_PATH_ROOT);
    const char* paths[] = {SD_PATH_ROOT, SD_PATH_CONFIG_DIR, SD_PATH_MESSAGES,
        SD_PATH_CONTACTS, SD_PATH_IDENTITY_DIR, SD_PATH_TRANSPORT};
    for (const char* path : paths) {
        if (!ensureDir(path)) return false;
        File directory = openDir(path);
        if (!directory || !directory.isDirectory()) return false;
    }
    Serial.println("[SD] Directory structure ready");
    return true;
}
