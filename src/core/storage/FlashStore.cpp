#include "FlashStore.h"
#include "storage/StorageLease.h"
#include "storage/AtomicStream.h"
#include "storage/ResetDirectory.h"
#include "storage/TreeWipe.h"
#include "util/PerfTrace.h"
#include <esp_partition.h>
#include <algorithm>

// Global LittleFS mutex — prevents cross-task corruption (WriteQueue task)
SemaphoreHandle_t FlashStore::_mutex = nullptr;

SemaphoreHandle_t FlashStore::mutex() {
    handheld::storage::assertOwner();
    return _mutex;
}

namespace {
// Legacy standalone builds label this partition "littlefs"; bmorcelli/Launcher
// labels the equivalent partition "spiffs". Try ours first, fall back.
const char* FLASH_PARTITION_LABELS[] = {"littlefs", "spiffs"};
constexpr const char* FLASH_BASE_PATH = "/littlefs";
constexpr const char* RESET_SCOPE = "/.factory-reset";
constexpr const char* RESET_NAME = ".factory-reset";
constexpr const char* RESET_PATHS[] = {RESET_SCOPE, "/.factory-reset.tmp", "/.factory-reset.bak"};
constexpr uint8_t RESET_MAGIC[] = {'R', 'S', 'R', 'E', 'S', 'E', 'T', 1};

bool resetArtifact(const char* name) {
    return !strcmp(name, RESET_NAME) || !strcmp(name, ".factory-reset.tmp") ||
           !strcmp(name, ".factory-reset.bak");
}

FlashStore::ResetState resetStateLocked() {
    using namespace handheld::storage::reset;
    bool pending = false;
    for (const char* path : RESET_PATHS) {
        const auto state = probe(FLASH_BASE_PATH, path);
        if (state == Presence::Unavailable) return FlashStore::ResetState::Unavailable;
        pending |= state != Presence::Absent;
    }
    return pending ? FlashStore::ResetState::Pending : FlashStore::ResetState::Clear;
}

const esp_partition_t* dataPartition() {
    for (const char* label : FLASH_PARTITION_LABELS) {
        const auto* partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                        ESP_PARTITION_SUBTYPE_ANY, label);
        if (partition) return partition;
    }
    return nullptr;
}

enum class Contents : uint8_t { Blank, LittleFs, Other, Unreadable };

// LittleFS keeps two superblock copies, each naming itself at byte 8 of
// blocks 0 and 1. Without either, nothing in the partition was ever ours.
Contents unmountedContents(const esp_partition_t* partition) {
    uint8_t bytes[512];
    for (size_t block : {size_t(0), size_t(4096)}) {
        if (block + 16 > partition->size) break;
        if (esp_partition_read(partition, block, bytes, 16) != ESP_OK) return Contents::Unreadable;
        if (!memcmp(bytes + 8, "littlefs", 8)) return Contents::LittleFs;
    }
    for (size_t offset = 0; offset < partition->size; offset += sizeof(bytes)) {
        const size_t size = std::min(sizeof(bytes), size_t(partition->size - offset));
        if (esp_partition_read(partition, offset, bytes, size) != ESP_OK) return Contents::Unreadable;
        for (size_t i = 0; i < size; ++i) if (bytes[i] != 0xff) return Contents::Other;
        yield();
    }
    return Contents::Blank;
}

// A mounted filesystem with entries but none of ours (shared with another
// Launcher app) is never adopted silently.
bool holdsOtherFiles() {
    static const char* ours[] = {"identity", "transport", "config", "contacts", "messages",
                                 ".factory-reset", ".factory-reset.tmp", ".factory-reset.bak"};
    File root = LittleFS.open("/");
    if (!root || !root.isDirectory()) return false;
    bool any = false;
    for (File entry = root.openNextFile(); entry; entry = root.openNextFile()) {
        const char* name = entry.name(); if (name[0] == '/') ++name;
        any = true;
        for (const char* known : ours) if (!strcmp(name, known)) return false;
    }
    return any;
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
    handheld::storage::assertOwner();
    if (!handheld::storage::StorageLease::initialize()) return false;
    handheld::storage::StorageLease lease;
    if (!lease.held()) return false;
    // Create mutex before any LittleFS access
    if (!_mutex) {
        _mutex = xSemaphoreCreateMutex();
    }
    if (!_mutex) return false;

    FSLock lock;
    _ready = false;
    _resetState = ResetState::Unavailable;
    _startup = Startup::Failed;

    const auto* partition = dataPartition();
    if (!partition) { _startup = Startup::Missing; return false; }
    if (!LittleFS.begin(false, FLASH_BASE_PATH, 10, partition->label)) {
        // Only completely erased factory media initializes here. Everything
        // else is classified and stays intact until startup decides.
        switch (unmountedContents(partition)) {
        case Contents::Blank:
            if (!LittleFS.begin(true, FLASH_BASE_PATH, 10, partition->label)) return false;
            break;
        case Contents::LittleFs:
            Serial.println("[FLASH] Mount failed; existing data preserved for recovery");
            _startup = Startup::Damaged; return false;
        case Contents::Other:
            // Launcher allocates our partition over removed apps without erasing it.
            _startup = strcmp(partition->label, FLASH_PARTITION_LABELS[0]) ? Startup::Foreign : Startup::Unprepared;
            return false;
        case Contents::Unreadable:
            return false;
        }
    } else if (holdsOtherFiles()) {
        LittleFS.end();
        Serial.println("[FLASH] Storage holds another app's files; left unchanged");
        _startup = Startup::Foreign; return false;
    }
    _ready = true; _startup = Startup::Ready;

    // Never restore backups or initialize application directories while a
    // destructive operation is incomplete (including an unreadable probe).
    _resetState = resetStateLocked();
    if (_resetState != ResetState::Clear) return false;

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
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return ;
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
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return ;
    FSLock lock;
    LittleFS.end();
    _ready = false;
}

bool FlashStore::ensureDir(const char* path) {
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return false;
    if (!_ready) return false;
    FSLock lock;
    return ensureDirLocked(path);
}

bool FlashStore::exists(const char* path) {
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return false;
    if (!_ready) return false;
    FSLock lock;
    return LittleFS.exists(path);
}

bool FlashStore::remove(const char* path) {
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return false;
    if (!_ready) return false;
    FSLock lock;
    return LittleFS.remove(path);
}

bool FlashStore::removeDir(const char* path) {
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return false;
    if (!_ready) return false;
    FSLock lock;
    return LittleFS.rmdir(path);
}

bool FlashStore::rename(const char* from, const char* to) {
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return false;
    if (!_ready) return false;
    FSLock lock;
    return LittleFS.rename(from, to);
}

File FlashStore::openDir(const char* path) {
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return File();
    if (!_ready) return File();
    FSLock lock;
    return LittleFS.open(path);
}

File FlashStore::openFile(const char* path, const char* mode) {
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return File();
    if (!_ready) return File();
    FSLock lock;
    return LittleFS.open(path, mode);
}

bool FlashStore::writeAtomic(const char* path, const uint8_t* data, size_t len) {
    handheld::storage::MemorySource source(data, len);
    return writeAtomic(path, source) == handheld::storage::Error::None;
}

handheld::storage::Error FlashStore::writeAtomic(const char* path, const handheld::storage::AtomicSource& source) {
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return handheld::storage::Error::Unavailable;
    const unsigned long start = PerfTrace::nowMs();
    const auto error = handheld::storage::atomicStream(*this, path, source);
    PerfTrace::write("flash", "atomic", path, source.length(), start,
                     error == handheld::storage::Error::None);
    return error;
}

bool FlashStore::writeDirect(const char* path, const uint8_t* data, size_t len) {
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return false;
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
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return false;
    return readFileFully(path, buffer, maxLen, bytesRead);
}

bool FlashStore::readFileFully(const char* path, uint8_t* buffer, size_t maxLen,
                               size_t& bytesRead) {
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return false;
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
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return false;
    return writeAtomic(path, (const uint8_t*)data.c_str(), data.length());
}

String FlashStore::readString(const char* path) {
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return String();
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

FlashStore::RecordSource FlashStore::readRecord(const char* path, String& out, size_t maxBytes,
                                               bool (*reserve)(String&, size_t)) {
    handheld::storage::assertOwner();
    out = "";
    handheld::storage::StorageLease lease;
    if (!lease.held() || !_ready || !path) return RecordSource::Unavailable;
    FSLock lock;
    const bool primary = LittleFS.exists(path);
    const String selected = primary ? String(path) : String(path) + ".bak";
    if (!primary && !LittleFS.exists(selected.c_str())) return RecordSource::Absent;
    File file = LittleFS.open(selected.c_str(), "r");
    if (!file) return RecordSource::Unavailable;
    const size_t size = file.size();
    if (!size || size > maxBytes || file.isDirectory()) { file.close(); return RecordSource::Invalid; }
    if (!(reserve ? reserve(out, size) : out.reserve(size))) {
        file.close(); return RecordSource::Unavailable;
    }
    char bytes[512];
    for (size_t offset = 0; offset < size; offset += sizeof bytes) {
        const size_t count = std::min(sizeof bytes, size - offset);
        if (file.readBytes(bytes, count) != count || !out.concat(bytes, count)) {
            file.close(); out = ""; return RecordSource::Unavailable;
        }
    }
    file.close();
    return primary ? RecordSource::Primary : RecordSource::Backup;
}

bool FlashStore::format() {
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return false;
    Serial.println("[FLASH] Formatting LittleFS...");
    bool ok;
    {
        FSLock lock;
        // The factory-reset intent must outlive NVS erase and internal data
        // removal. A legacy/direct format must never erase that recovery gate.
        if (!_ready || resetStateLocked() != ResetState::Clear) return false;
        LittleFS.end();
        _ready = false;
        ok = LittleFS.format();
    }
    // Re-mount with fresh filesystem (begin() takes its own lock)
    if (ok) ok = begin();
    return ok;
}

bool FlashStore::prepare() {
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held() || _ready) return false;
    {
        FSLock lock;
        const auto* partition = dataPartition();
        if (!partition) return false;
        Serial.printf("[FLASH] Preparing internal storage (%s, %lu bytes)\n", partition->label,
                      (unsigned long)partition->size);
        LittleFS.end();
        // Format-on-fail covers an unmountable partition; a mountable one that
        // holds another app's files must be formatted explicitly.
        bool ok = LittleFS.begin(true, FLASH_BASE_PATH, 10, partition->label);
        if (ok && holdsOtherFiles()) ok = LittleFS.format();
        LittleFS.end();
        if (!ok) return false;
    }
    return begin();
}

bool FlashStore::resetScope(bool& includesSD) {
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held() || !_ready) return false;
    uint8_t bytes[12]; size_t length = 0;
    if (!readFileFully(RESET_SCOPE, bytes, sizeof(bytes), length) || length != sizeof(bytes) ||
        memcmp(bytes, RESET_MAGIC, sizeof(RESET_MAGIC)) || bytes[8] > 1 ||
        bytes[9] != uint8_t(~bytes[8]) || bytes[10] || bytes[11]) return false;
    includesSD = bytes[8] != 0;
    return true;
}

bool FlashStore::prepareReset(bool includesSD, bool replaceInvalidScope) {
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held() || !_ready) return false;
    {
        FSLock lock;
        using namespace handheld::storage::reset;
        _resetState = resetStateLocked();
        if (_resetState == ResetState::Unavailable) return false;
        // A directory in a reserved record path cannot be overwritten safely.
        for (const char* path : RESET_PATHS)
            if (probe(FLASH_BASE_PATH, path) == Presence::Directory) return false;
    }
    bool originalScope = false;
    if (resetScope(originalScope)) return originalScope == includesSD;
    if (_resetState == ResetState::Pending && !replaceInvalidScope) return false;
    uint8_t bytes[12]{};
    memcpy(bytes, RESET_MAGIC, sizeof(RESET_MAGIC));
    bytes[8] = includesSD ? 1 : 0; bytes[9] = uint8_t(~bytes[8]);
    const bool written = writeAtomic(RESET_SCOPE, bytes, sizeof(bytes));
    {
        FSLock lock; _resetState = resetStateLocked();
    }
    return written && resetScope(originalScope) && originalScope == includesSD;
}

bool FlashStore::wipeForReset() {
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    bool scope = false;
    if (!lease.held() || !_ready || !resetScope(scope)) return false;
    if (!handheld::storage::wipeTreeContents(*this, "/", resetArtifact)) return false;
    FSLock lock;
    return handheld::storage::reset::containsOnly(FLASH_BASE_PATH, resetArtifact);
}

bool FlashStore::completeReset() {
    handheld::storage::assertOwner();
    handheld::storage::StorageLease lease;
    bool scope = false;
    if (!lease.held() || !_ready || !resetScope(scope)) return false;
    {
        FSLock lock;
        if (!handheld::storage::reset::containsOnly(FLASH_BASE_PATH, resetArtifact)) return false;
    }
    // Keep the full original scope in the primary until the final removal.
    // In particular, a failed artifact cleanup never discards that metadata.
    for (unsigned i = 1; i < 3; ++i) {
        handheld::storage::reset::Presence state;
        { FSLock lock; state = handheld::storage::reset::probe(FLASH_BASE_PATH, RESET_PATHS[i]); }
        if (state == handheld::storage::reset::Presence::Absent) continue;
        if (state != handheld::storage::reset::Presence::Other || !remove(RESET_PATHS[i])) return false;
    }
    if (!remove(RESET_SCOPE)) return false;
    FSLock lock;
    _resetState = resetStateLocked();
    return _resetState == ResetState::Clear;
}

size_t FlashStore::totalBytes() const {
    handheld::storage::assertOwner(); handheld::storage::StorageLease lease;
    return lease.held() && _ready ? LittleFS.totalBytes() : 0;
}

size_t FlashStore::usedBytes() const {
    handheld::storage::assertOwner(); handheld::storage::StorageLease lease;
    return lease.held() && _ready ? LittleFS.usedBytes() : 0;
}
