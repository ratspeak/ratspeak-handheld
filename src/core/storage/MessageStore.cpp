#include "MessageStore.h"
#include "runtime/TaskOwner.h"
#include "config/Config.h"
#include "util/PerfTrace.h"
#include <ArduinoJson.h>
#include <Preferences.h>

// Helper: check if filename ends with ".json"
static bool isJsonFile(const char* name) {
    size_t len = strlen(name);
    return len > 5 && strcmp(name + len - 5, ".json") == 0;
}

// An interrupted rename leaves the previous committed file under .bak.
// Include it in discovery immediately, without requiring a later write/trim.
static String messageFilename(const char* name) {
    String filename(name);
    if (filename.endsWith(".bak")) filename = filename.substring(0, filename.length() - 4);
    return isJsonFile(filename.c_str()) ? filename : String();
}

static bool messageRevision(const String& json, uint32_t& revision) {
    JsonDocument doc;
    if (json.isEmpty() || deserializeJson(doc, json) || !doc.is<JsonObject>() ||
        !doc["src"].is<const char*>() || !doc["dst"].is<const char*>() ||
        !doc["content"].is<const char*>()) return false;
    revision = doc["store_revision"] | uint32_t(0);
    return true;
}

uint32_t MessageStore::deletedThrough(const std::string& peer) const {
    const String path = conversationDir(peer) + "/.deleted";
    if (!_flash || (!_flash->exists(path.c_str()) && !_flash->exists((path + ".bak").c_str()))) return 0;
    String json = _flash->readString(path.c_str());
    JsonDocument doc;
    if (json.isEmpty() || deserializeJson(doc, json) || !doc["through"].is<uint32_t>()) {
        json = _flash->readString((path + ".bak").c_str());
        if (json.isEmpty() || deserializeJson(doc, json) || !doc["through"].is<uint32_t>()) return UINT32_MAX;
    }
    return doc["through"].as<uint32_t>();
}

std::vector<String> MessageStore::messageFiles(const std::string& peer) const {
    std::vector<String> names;
    const uint32_t cutoff = deletedThrough(peer);
    auto collect = [&](File directory) {
        if (!directory || !directory.isDirectory()) return;
        for (File entry = directory.openNextFile(); entry; entry = directory.openNextFile()) {
            const String name = entry.isDirectory() ? String() : messageFilename(entry.name());
            if (!name.isEmpty() && strtoul(name.c_str(), nullptr, 10) > cutoff) names.push_back(name);
        }
    };
    if (_flash) collect(_flash->openDir(conversationDir(peer)));
    if (_externalStorageEnabled && _sd && _sd->isReady()) collect(_sd->openDir(sdConversationDir(peer).c_str()));
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

String MessageStore::readMessageJson(const std::string& peer, const String& filename) const {
    const String flashPath = conversationDir(peer) + "/" + filename;
    const String sdPath = sdConversationDir(peer) + "/" + filename;
    String flashJson = _flash ? _flash->readString(flashPath.c_str()) : String();
    String sdJson = _externalStorageEnabled && _sd && _sd->isReady() ? _sd->readString(sdPath.c_str()) : String();
    uint32_t flashRevision = 0, sdRevision = 0;
    bool flashValid = messageRevision(flashJson, flashRevision);
    bool sdValid = messageRevision(sdJson, sdRevision);
    if (!flashValid && _flash) {
        flashJson = _flash->readString((flashPath + ".bak").c_str());
        flashValid = messageRevision(flashJson, flashRevision);
    }
    if (!sdValid && _externalStorageEnabled && _sd && _sd->isReady()) {
        sdJson = _sd->readString((sdPath + ".bak").c_str());
        sdValid = messageRevision(sdJson, sdRevision);
    }
    if (sdValid && (!flashValid || sdRevision > flashRevision)) return sdJson;
    return flashValid ? flashJson : String();
}

bool MessageStore::writeMessageJson(const std::string& peer, const String& filename, const String& json) {
    ensureConvDirs(peer);
    const String flashPath = conversationDir(peer) + "/" + filename;
    const String sdPath = sdConversationDir(peer) + "/" + filename;
    uint32_t revision = 0;
    bool saved = _flash && (!_flash->exists(flashPath.c_str()) ||
        messageRevision(_flash->readString(flashPath.c_str()), revision)) && flashWriteJson(flashPath.c_str(), json);
    if (_externalStorageEnabled && _sd && _sd->isReady() && (!_sd->exists(sdPath.c_str()) ||
        messageRevision(_sd->readString(sdPath.c_str()), revision)))
        saved = sdWriteJson(sdPath.c_str(), json) || saved;
    return saved;
}

static bool hasDirectionSuffix(const String& name, char suffix) {
    int len = name.length();
    return len >= 7 && name[len - 6] == suffix;
}

static bool isPendingStatus(LXMFStatus status) {
    return status == LXMFStatus::QUEUED || status == LXMFStatus::SENDING;
}

static uint32_t counterFromFilename(const String& name) {
    return (uint32_t)strtoul(name.c_str(), nullptr, 10);
}

bool MessageStore::begin(FlashStore* flash, SDStore* sd, bool externalStorageEnabled) {
    handheld::assertDeviceOwner();
    _flash = flash;
    _sd = sd;
    _externalStorageEnabled = externalStorageEnabled;
    unsigned long beginMs = millis();
    _flash->ensureDir(PATH_MESSAGES);

    unsigned long phaseMs = millis();
    if (_externalStorageEnabled && _sd && _sd->isReady()) {
        _sd->ensureDir(SD_PATH_ROOT);
        _sd->ensureDir(SD_PATH_MESSAGES);
        migrateFlashToSD();
    }
    unsigned long migrateMs = millis() - phaseMs;

#if LEGACY_MSG_FILENAME_MIGRATION
    // Pre-2.0 filename rename pass; NVS fs_migrated marks it done (donor flag)
    {
        Preferences mp;
        mp.begin(NVS_NS_MSG, true);
        bool fsMigrated = mp.getBool("fs_migrated", false);
        mp.end();
        if (!fsMigrated) {
            const bool migrated = migrateOldFilenames();
            Preferences wp;
            wp.begin(NVS_NS_MSG, false);
            if (migrated) wp.putBool("fs_migrated", true);
            wp.end();
            Serial.println("[MSGSTORE] Filename migration complete, flag set");
        }
    }
#endif

    phaseMs = millis();
    migrateTruncatedDirs();
    unsigned long truncMs = millis() - phaseMs;

#if LEGACY_READ_CTR_MIGRATION
    migrateReadMarkers();
#endif

#if STORAGE_ASYNC_WRITES
    _writeQueue.begin(_sd, _flash);
#endif

    phaseMs = millis();
    initReceiveCounter();
    unsigned long counterMs = millis() - phaseMs;

    phaseMs = millis();
    refreshConversations();
    unsigned long refreshMs = millis() - phaseMs;

    phaseMs = millis();
    buildSummaries();
    unsigned long summaryMs = millis() - phaseMs;
    unsigned long totalMs = millis() - beginMs;
#if RSDECK_PERF_TRACE
    Serial.printf("[PERF] MSG begin: total=%lums migrate=%lums trunc=%lums counter=%lums refresh=%lums summaries=%lums convs=%d summary_count=%d ext=%s sd=%s\n",
                  totalMs, migrateMs, truncMs, counterMs, refreshMs, summaryMs,
                  (int)_conversations.size(), (int)_summaries.size(),
                  _externalStorageEnabled ? "on" : "off",
                  (_sd && _sd->isReady()) ? "ready" : "no");
#endif
    Serial.printf("[MSGSTORE] %d conversations found, receive counter=%lu\n",
                  (int)_conversations.size(), (unsigned long)_nextReceiveCounter);
    return true;
}

void MessageStore::migrateFlashToSD() {
    handheld::assertDeviceOwner();
    if (!_sd || !_sd->isReady() || !_flash) return;

    File dir = _flash->openDir(PATH_MESSAGES);
    if (!dir || !dir.isDirectory()) return;

    int migrated = 0;
    File peerDir = dir.openNextFile();
    while (peerDir) {
        if (peerDir.isDirectory()) {
            std::string peerHex = peerDir.name();
            String sdDir = sdConversationDir(peerHex);
            _sd->ensureDir(sdDir.c_str());

            File entry = peerDir.openNextFile();
            while (entry) {
                if (!entry.isDirectory() && isJsonFile(entry.name())) {
                    String sdPath = sdDir + "/" + entry.name();
                    if (!_sd->exists(sdPath.c_str())) {
                        size_t size = entry.size();
                        if (size > 0 && size <= 32768) {
                            String json = _flash->readString((conversationDir(peerHex) + "/" + entry.name()).c_str());
                            uint32_t revision = 0;
                            if (json.length() == size && messageRevision(json, revision) &&
                                _sd->writeString(sdPath.c_str(), json)) migrated++;
                            yield();
                        }
                    }
                }
                entry = peerDir.openNextFile();
            }
            enforceFlashLimit(peerHex);
        }
        peerDir = dir.openNextFile();
    }

    if (migrated > 0) {
        Serial.printf("[MSGSTORE] Migrated %d messages from flash to SD\n", migrated);
    }
}

void MessageStore::initReceiveCounter() {
    handheld::assertDeviceOwner();
    Preferences prefs;
    prefs.begin(NVS_NS_MSG, true);
    _nextReceiveCounter = prefs.getUInt("msgctr", 0);
    prefs.end();

    if (_nextReceiveCounter > 0) {
#if STORAGE_ASYNC_WRITES
        _persistedCounterCeiling = _nextReceiveCounter;  // NVS value IS the ceiling
#endif
        Serial.printf("[MSGSTORE] receive counter=%lu (from NVS)\n",
                      (unsigned long)_nextReceiveCounter);
        return;
    }

    // NVS has no counter — scan existing files to find highest prefix (first boot only)
    uint32_t maxPrefix = 0;

    auto scanDir = [&](File& dir) {
        File entry = dir.openNextFile();
        while (entry) {
            if (!entry.isDirectory()) {
                String name = entry.name();
                unsigned long val = strtoul(name.c_str(), nullptr, 10);
                if (val > maxPrefix && val < 1000000000) maxPrefix = (uint32_t)val;
            }
            entry = dir.openNextFile();
        }
    };

    // Scan SD conversations
    if (_externalStorageEnabled && _sd && _sd->isReady()) {
        File dir = _sd->openDir(SD_PATH_MESSAGES);
        if (dir && dir.isDirectory()) {
            File peerDir = dir.openNextFile();
            while (peerDir) {
                if (peerDir.isDirectory()) scanDir(peerDir);
                peerDir = dir.openNextFile();
            }
        }
    }

    // Scan flash conversations
    File dir = _flash->openDir(PATH_MESSAGES);
    if (dir && dir.isDirectory()) {
        File peerDir = dir.openNextFile();
        while (peerDir) {
            if (peerDir.isDirectory()) scanDir(peerDir);
            peerDir = dir.openNextFile();
        }
    }

    _nextReceiveCounter = maxPrefix + 1;

    Preferences p;
    p.begin(NVS_NS_MSG, false);
    p.putUInt("msgctr", _nextReceiveCounter);
    p.end();
#if STORAGE_ASYNC_WRITES
    _persistedCounterCeiling = _nextReceiveCounter;
#endif

    Serial.printf("[MSGSTORE] Initialized receive counter to %lu from existing files\n",
                  (unsigned long)_nextReceiveCounter);
}

// Migrate old 16-char truncated directories to full 32-char hex names
void MessageStore::migrateTruncatedDirs() {
    handheld::assertDeviceOwner();
    auto migrateInDir = [&](auto openFn, auto renameFn, auto readStringFn, const char* basePath) {
        File dir = openFn(basePath);
        if (!dir || !dir.isDirectory()) return;

        // Collect dirs that need renaming (can't rename while iterating)
        std::vector<std::pair<String, String>> renames; // old path -> new path

        File entry = dir.openNextFile();
        while (entry) {
            if (entry.isDirectory()) {
                std::string dirName = entry.name();
                // Old dirs are exactly 16 hex chars; new ones are 32
                if (dirName.length() == 16) {
                    // Read first JSON file inside to get the full hash
                    String oldDir = String(basePath) + "/" + dirName.c_str();
                    File inner = openFn(oldDir.c_str());
                    if (inner && inner.isDirectory()) {
                        File jsonFile = inner.openNextFile();
                        std::string fullHash;
                        while (jsonFile) {
                            if (!jsonFile.isDirectory() && isJsonFile(jsonFile.name())) {
                                String jsonPath = oldDir + "/" + jsonFile.name();
                                String json = readStringFn(jsonPath.c_str());
                                if (json.length() > 0) {
                                    JsonDocument doc;
                                    if (!deserializeJson(doc, json)) {
                                        // Use src for incoming, dst for outgoing
                                        bool incoming = doc["incoming"] | false;
                                        std::string hash = incoming ?
                                            (doc["src"] | "") : (doc["dst"] | "");
                                        if (hash.length() == 32) {
                                            fullHash = hash;
                                        }
                                    }
                                }
                                jsonFile.close();
                                break;
                            }
                            jsonFile.close();
                            jsonFile = inner.openNextFile();
                        }
                        inner.close();

                        if (!fullHash.empty() && fullHash.substr(0, 16) == dirName) {
                            String newDir = String(basePath) + "/" + fullHash.c_str();
                            renames.push_back({oldDir, newDir});
                        }
                    }
                }
            }
            entry.close();
            entry = dir.openNextFile();
        }
        dir.close();

        for (auto& [oldPath, newPath] : renames) {
            if (renameFn(oldPath.c_str(), newPath.c_str())) {
                Serial.printf("[MSGSTORE] Migrated %s -> %s\n", oldPath.c_str(), newPath.c_str());
            }
        }
    };

    // Migrate flash directories
    migrateInDir(
        [this](const char* p) { return _flash->openDir(p); },
        [this](const char* a, const char* b) { return _flash->rename(a, b); },
        [this](const char* p) { return _flash ? _flash->readString(p) : String(""); },
        PATH_MESSAGES
    );

    // Migrate SD directories
    if (_externalStorageEnabled && _sd && _sd->isReady()) {
        migrateInDir(
            [this](const char* p) { return _sd->openDir(p); },
            [this](const char* a, const char* b) { return _sd->rename(a, b); },
            [this](const char* p) { return _sd->readString(p); },
            SD_PATH_MESSAGES
        );
    }
}

void MessageStore::refreshConversations() {
    handheld::assertDeviceOwner();
    _conversations.clear();

    if (_externalStorageEnabled && _sd && _sd->isReady()) {
        File dir = _sd->openDir(SD_PATH_MESSAGES);
        if (dir && dir.isDirectory()) {
            File entry = dir.openNextFile();
            while (entry) {
                if (entry.isDirectory()) {
                    _conversations.push_back(entry.name());
                }
                entry = dir.openNextFile();
            }
        }
    }

    File dir = _flash->openDir(PATH_MESSAGES);
    if (dir && dir.isDirectory()) {
        File entry = dir.openNextFile();
        while (entry) {
            if (entry.isDirectory()) {
                std::string name = entry.name();
                bool found = false;
                for (auto& c : _conversations) {
                    if (c == name) { found = true; break; }
                }
                if (!found) _conversations.push_back(name);
            }
            entry = dir.openNextFile();
        }
    }
    _conversations.erase(std::remove_if(_conversations.begin(), _conversations.end(),
        [&](const std::string& peer) { return deletedThrough(peer) && messageFiles(peer).empty(); }), _conversations.end());
}

void MessageStore::ensureConvDirs(const std::string& peerHex) {
    handheld::assertDeviceOwner();
    if (_ensuredDirs.count(peerHex)) return;

    if (_externalStorageEnabled && _sd && _sd->isReady()) {
        String sdDir = sdConversationDir(peerHex);
        _sd->ensureDir(sdDir.c_str());
    }
    if (_flash) {
        String flashDir = conversationDir(peerHex);
        _flash->ensureDir(flashDir.c_str());
    }

    // Cap cache to prevent unbounded growth
    if (_ensuredDirs.size() >= 64) {
        _ensuredDirs.clear();
    }
    _ensuredDirs.insert(peerHex);
}

// JSON rewrite sinks: synchronous on shipped LVGL boards, queued where
// STORAGE_ASYNC_WRITES opts in (packet-callback-safe on the no-PSRAM board).
bool MessageStore::sdWriteJson(const char* path, const String& data) {
    handheld::assertDeviceOwner();
#if STORAGE_ASYNC_WRITES
    return _writeQueue.enqueue(path, data, WriteBackend::SD_ONLY);
#else
    return _sd && _sd->writeString(path, data);
#endif
}

bool MessageStore::flashWriteJson(const char* path, const String& data) {
    handheld::assertDeviceOwner();
#if STORAGE_ASYNC_WRITES
    return _writeQueue.enqueue(path, data, WriteBackend::FLASH_ONLY);
#else
    return _flash && _flash->writeString(path, data);
#endif
}

bool MessageStore::saveMessage(LXMFMessage& msg) {
    handheld::assertDeviceOwner();
    if (!_flash) return false;
    unsigned long startMs = millis();

    std::string peerHex = msg.incoming ?
        msg.sourceHash.toHex() : msg.destHash.toHex();
    const uint32_t cutoff = deletedThrough(peerHex);
    if (cutoff == UINT32_MAX) return false;
    if (_nextReceiveCounter <= cutoff) _nextReceiveCounter = cutoff + 1;

    unsigned long serializeStartMs = millis();
    JsonDocument doc;
    doc["src"] = msg.sourceHash.toHex();
    doc["dst"] = msg.destHash.toHex();
    doc["ts"] = msg.timestamp;
    doc["content"] = msg.content;
    doc["title"] = msg.title;
    doc["incoming"] = msg.incoming;
    doc["status"] = (int)msg.status;
    doc["read"] = msg.incoming ? msg.read : true;
    doc["store_revision"] = 1;
    if (msg.messageId.size() > 0) {
        doc["msgid"] = msg.messageId.toHex();
    }

    String json;
    serializeJson(doc, json);
    if (doc.overflowed() || json.isEmpty() || json.length() != measureJson(doc) || json.length() > 32768) return false;
    unsigned long serializeMs = millis() - serializeStartMs;
    size_t jsonBytes = json.length();

    // Counter-based filename: unique, monotonic, sorts correctly
    uint32_t counter;
    char filename[64];
    do {
        if (_nextReceiveCounter == UINT32_MAX) return false;
        counter = _nextReceiveCounter++;
        snprintf(filename, sizeof(filename), "%013lu_%c.json", (unsigned long)counter, msg.incoming ? 'i' : 'o');
    } while (_flash->exists((conversationDir(peerHex) + "/" + filename).c_str()) ||
             _flash->exists((conversationDir(peerHex) + "/" + filename + ".bak").c_str()) ||
             (_sd && _sd->isReady() && (_sd->exists((sdConversationDir(peerHex) + "/" + filename).c_str()) ||
              _sd->exists((sdConversationDir(peerHex) + "/" + filename + ".bak").c_str()))));

    unsigned long nvsMs = 0;
#if STORAGE_ASYNC_WRITES
    // Reserve counters ahead so NVS is ALWAYS >= any counter a written file uses:
    // one commit per COUNTER_RESERVE messages instead of per message, and no
    // crash window (the old 30s WriteQueue batch could reuse counters after a
    // reboot and silently overwrite existing message files). Boot resumes at the
    // ceiling; skipped values are harmless gaps in the sort order.
    if (_nextReceiveCounter > _persistedCounterCeiling) {
        unsigned long nvsStartMs = millis();
        const uint32_t ceiling = _nextReceiveCounter > UINT32_MAX - COUNTER_RESERVE ? UINT32_MAX : uint32_t(_nextReceiveCounter) + COUNTER_RESERVE;
        Preferences p;
        if (!p.begin(NVS_NS_MSG, false)) return false;
        const bool reserved = p.putUInt("msgctr", ceiling) == sizeof(uint32_t) && p.getUInt("msgctr", 0) == ceiling;
        p.end();
        if (!reserved) return false;
        _persistedCounterCeiling = ceiling;
        nvsMs = millis() - nvsStartMs;
    }
#else
    // Persist counter to NVS before the file write (sync path: exact value)
    unsigned long nvsStartMs = millis();
    {
        Preferences p;
        if (!p.begin(NVS_NS_MSG, false)) return false;
        const bool reserved = p.putUInt("msgctr", _nextReceiveCounter) == sizeof(uint32_t) && p.getUInt("msgctr", 0) == _nextReceiveCounter;
        p.end();
        if (!reserved) return false;
    }
    nvsMs = millis() - nvsStartMs;
#endif

    bool sdOk = false;
    bool flashOk = false;
    bool sdAttempted = false;
    unsigned long sdMs = 0;
    unsigned long flashMs = 0;

    ensureConvDirs(peerHex);

#if STORAGE_ASYNC_WRITES
    // Non-blocking: one queued job writes flash (+SD when active)
    String flashPath = conversationDir(peerHex) + "/" + filename;
    bool saved;
    if (_externalStorageEnabled && _sd && _sd->isReady()) {
        sdAttempted = true;
        String sdPath = sdConversationDir(peerHex) + "/" + filename;
        saved = _writeQueue.enqueue(sdPath.c_str(), flashPath.c_str(), json, WriteBackend::BOTH);
        sdOk = saved;
    } else {
        saved = _writeQueue.enqueue(nullptr, flashPath.c_str(), json, WriteBackend::FLASH_ONLY);
    }
    flashOk = saved;
    if (!saved) {
        _nextReceiveCounter--;
        Serial.printf("[MSGSTORE] Write queue rejected message for %s\n", peerHex.substr(0, 8).c_str());
        return false;
    }
#else
    if (_externalStorageEnabled && _sd && _sd->isReady()) {
        sdAttempted = true;
        unsigned long sdStartMs = millis();
        String sdPath = sdConversationDir(peerHex) + "/" + filename;
        sdOk = _sd->writeString(sdPath.c_str(), json);
        sdMs = millis() - sdStartMs;
    }

    unsigned long flashStartMs = millis();
    String flashPath = conversationDir(peerHex) + "/" + filename;
    flashOk = _flash->writeString(flashPath.c_str(), json);
    flashMs = millis() - flashStartMs;
    bool saved = sdOk || flashOk;
#endif
    if (!saved) return false;
    msg.savedCounter = counter;

    bool found = false;
    for (auto& c : _conversations) {
        if (c == peerHex) { found = true; break; }
    }
    if (!found) _conversations.push_back(peerHex);

    unsigned long enforceStartMs = millis();
#if STORAGE_ASYNC_WRITES
    // Donor cadence: trim active stores every 30s instead of scanning per save.
    {
        static unsigned long lastStoreTrim = 0;
        unsigned long trimNow = millis();
        if (trimNow - lastStoreTrim >= 30000) {
            lastStoreTrim = trimNow;
            for (auto& conv : _conversations) {
                enforceFlashLimit(conv);
                enforceSDLimit(conv);
                yield();
            }
        }
    }
#else
    if (sdOk) enforceSDLimit(peerHex);
    if (flashOk) enforceFlashLimit(peerHex);
#endif
    unsigned long enforceMs = millis() - enforceStartMs;

    // Update summary cache
    unsigned long summaryStartMs = millis();
    {
        auto& s = _summaries[peerHex];
        s.lastTimestamp = msg.timestamp;
        s.lastIncoming = msg.incoming;
        std::string prefix = msg.incoming ? "Them: " : "You: ";
        std::string content = msg.content;
        if (content.size() > 15) content = content.substr(0, 15) + "...";
        s.lastPreview = prefix + content;
        s.totalCount++;
        if (msg.incoming && !msg.read) s.unreadCount++;
        if (!msg.incoming) {
            s.hasOutgoing = true;
            s.lastOutgoingStatus = msg.status;
            s.lastOutgoingCounter = counter;
            if (isPendingStatus(msg.status)) s.hasPending = true;
            if (msg.status == LXMFStatus::FAILED) s.hasFailed = true;
            if (isPendingStatus(msg.status) && s.pendingCount < UINT16_MAX) s.pendingCount++;
            if (msg.status == LXMFStatus::FAILED && s.failedCount < UINT16_MAX) s.failedCount++;
        }
        int limit = RSDECK_MAX_MESSAGES_PER_CONV;
        if (s.totalCount > limit) {
            rebuildSummary(peerHex);
        }
    }
    unsigned long summaryMs = millis() - summaryStartMs;

    if (saved) bumpRevision();
    unsigned long elapsed = millis() - startMs;
    if (PerfTrace::shouldLog(elapsed, RSDECK_PERF_MSG_TRACE_MS) ||
        PerfTrace::shouldLog(sdMs, RSDECK_PERF_WRITE_TRACE_MS) ||
        PerfTrace::shouldLog(flashMs, RSDECK_PERF_WRITE_TRACE_MS) ||
        !saved) {
        Serial.printf("[PERF] MSG save: peer=%s dir=%c bytes=%u counter=%lu sd=%s/%lums flash=%s/%lums serialize=%lums nvs=%lums enforce=%lums summary=%lums total=%lums\n",
                      peerHex.substr(0, 8).c_str(), msg.incoming ? 'i' : 'o',
                      (unsigned)jsonBytes, (unsigned long)counter,
                      sdAttempted ? (sdOk ? "ok" : "fail") : "skip", sdMs,
                      flashOk ? "ok" : "fail", flashMs,
                      serializeMs, nvsMs, enforceMs, summaryMs, elapsed);
    }
    return saved;
}

void MessageStore::beginHistory(HistoryCursor& cursor, const std::string& peer) const {
    handheld::assertDeviceOwner();
    cursor = {};
    cursor.peer = peer;
    cursor.deletedThrough = deletedThrough(peer);
    cursor.revision = _revision;
    if (_externalStorageEnabled && _sd && _sd->isReady()) {
        cursor.path = sdConversationDir(peer);
        cursor.directory = _sd->openDir(cursor.path.c_str());
        cursor.sd = cursor.directory && cursor.directory.isDirectory();
    }
    if (!cursor.sd && _flash) {
        cursor.path = conversationDir(peer);
        cursor.directory = _flash->openDir(cursor.path);
    }
    cursor.ready = !cursor.directory || !cursor.directory.isDirectory();
}

MessageStore::HistoryStep MessageStore::stepHistory(HistoryCursor& cursor) const {
    handheld::assertDeviceOwner();
    if (cursor.revision != _revision) return HistoryStep::Stale;
    if (cursor.ready) return HistoryStep::Ready;
    for (unsigned count = 0; count < 8; ++count) {
        File entry = cursor.directory.openNextFile();
        if (!entry) {
            cursor.directory.close();
            if (cursor.sd && _flash) {
                cursor.sd = false;
                cursor.path = conversationDir(cursor.peer);
                cursor.directory = _flash->openDir(cursor.path);
                if (cursor.directory && cursor.directory.isDirectory()) continue;
            }
            cursor.ready = true;
            return HistoryStep::Ready;
        }
        if (entry.isDirectory()) continue;
        String name = messageFilename(entry.name());
        if (name.isEmpty() || counterFromFilename(name) <= cursor.deletedThrough) continue;
        auto begin = cursor.filenames.begin(), end = begin + cursor.count;
        if (std::binary_search(begin, end, name)) continue;
        if (cursor.count == cursor.filenames.size()) {
            if (name <= cursor.filenames[0]) continue;
            std::move(begin + 1, end, begin); --cursor.count; --end;
        }
        auto at = std::lower_bound(begin, end, name);
        std::move_backward(at, end, end + 1); *at = std::move(name); ++cursor.count;
    }
    return HistoryStep::Pending;
}

bool MessageStore::readHistory(const HistoryCursor& cursor, size_t index, LXMFMessage& msg) const {
    handheld::assertDeviceOwner();
    if (!cursor.ready || cursor.revision != _revision || index >= cursor.count) return false;
    const String json = readMessageJson(cursor.peer, cursor.filenames[index]);
    JsonDocument doc;
    if (json.isEmpty() || deserializeJson(doc, json)) return false;
    msg = {};
    msg.sourceHash.assignHex(doc["src"] | "");
    msg.destHash.assignHex(doc["dst"] | "");
    msg.timestamp = doc["ts"] | 0.0;
    msg.content = doc["content"] | ""; msg.title = doc["title"] | "";
    msg.incoming = doc["incoming"] | false;
    msg.status = LXMFStatus(doc["status"] | 0);
    msg.read = doc["read"] | false;
    msg.savedCounter = counterFromFilename(cursor.filenames[index]);
    msg.messageId.assignHex(doc["msgid"] | "");
    return true;
}

std::vector<LXMFMessage> MessageStore::loadConversation(const std::string& peerHex) const {
    handheld::assertDeviceOwner();
    return loadConversationTail(peerHex, 0);
}

std::vector<LXMFMessage> MessageStore::loadConversationTail(const std::string& peerHex, size_t maxMessages) const {
    handheld::assertDeviceOwner();
    const auto startMs = millis();
    std::vector<LXMFMessage> messages;
    const auto files = messageFiles(peerHex);
    const size_t first = maxMessages && files.size() > maxMessages ? files.size() - maxMessages : 0;
    for (size_t i = first; i < files.size(); ++i) {
        const String json = readMessageJson(peerHex, files[i]);
        JsonDocument doc;
        if (json.isEmpty() || deserializeJson(doc, json)) continue;
        LXMFMessage msg;
        msg.sourceHash.assignHex(doc["src"] | "");
        msg.destHash.assignHex(doc["dst"] | "");
        msg.timestamp = doc["ts"] | 0.0;
        msg.content = doc["content"] | ""; msg.title = doc["title"] | "";
        msg.incoming = doc["incoming"] | false;
        msg.status = LXMFStatus(doc["status"] | 0); msg.read = doc["read"] | false;
        msg.savedCounter = counterFromFilename(files[i]);
        msg.messageId.assignHex(doc["msgid"] | "");
        messages.push_back(std::move(msg));
    }
    const auto elapsed = millis() - startMs;
    if (PerfTrace::shouldLog(elapsed, RSDECK_PERF_MSG_TRACE_MS))
        Serial.printf("[PERF] MSG loadConversation: files=%u msgs=%u total=%lums\n",
            unsigned(files.size()), unsigned(messages.size()), elapsed);
    return messages;
}

std::vector<LXMFMessage> MessageStore::loadPendingOutgoing() const {
    handheld::assertDeviceOwner();
    std::vector<LXMFMessage> pending;
    for (const auto& peerHex : _conversations) {
        std::vector<LXMFMessage> messages = loadConversation(peerHex);
        for (auto& msg : messages) {
            if (msg.incoming) continue;
            if (msg.status == LXMFStatus::QUEUED || msg.status == LXMFStatus::SENDING) {
                msg.status = LXMFStatus::QUEUED;
                pending.push_back(msg);
            }
        }
    }
    std::sort(pending.begin(), pending.end(), [](const LXMFMessage& a, const LXMFMessage& b) {
        return a.savedCounter < b.savedCounter;
    });
    return pending;
}

std::vector<std::string> MessageStore::loadRecentMessageIds(size_t maxIds) const {
    handheld::assertDeviceOwner();
    std::vector<std::pair<uint32_t, std::string>> ordered;
    for (const auto& peerHex : _conversations) {
        std::vector<LXMFMessage> messages = loadConversation(peerHex);
        for (const auto& msg : messages) {
            if (msg.messageId.size() == 0) continue;
            ordered.push_back({msg.savedCounter, msg.messageId.toHex()});
        }
    }
    std::sort(ordered.begin(), ordered.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });
    if (ordered.size() > maxIds) {
        ordered.erase(ordered.begin(), ordered.end() - maxIds);
    }

    std::set<std::string> seen;
    std::vector<std::string> ids;
    ids.reserve(ordered.size());
    for (const auto& item : ordered) {
        if (seen.insert(item.second).second) ids.push_back(item.second);
    }
    return ids;
}

int MessageStore::messageCount(const std::string& peerHex) const {
    handheld::assertDeviceOwner();
    return int(messageFiles(peerHex).size());
}

bool MessageStore::deleteConversation(const std::string& peerHex) {
    handheld::assertDeviceOwner();
    if (!_flash || !_flash->isReady()) return false;
    uint32_t cutoff = _nextReceiveCounter > 0 ? uint32_t(_nextReceiveCounter) - 1 : 0;
    cutoff = std::max(cutoff, deletedThrough(peerHex));
    for (const auto& name : messageFiles(peerHex)) cutoff = std::max(cutoff, counterFromFilename(name));
    JsonDocument marker; marker["through"] = cutoff;
    String json;
    if (marker.overflowed() || serializeJson(marker, json) != measureJson(marker) ||
        !_flash->ensureDir(conversationDir(peerHex).c_str()) ||
        !_flash->writeString((conversationDir(peerHex) + "/.deleted").c_str(), json)) return false;
    bool ok = true;
    auto eraseDir = [&](auto& store, const String& path) {
        if (!store.exists(path.c_str())) return true;
        File dir = store.openDir(path.c_str());
        if (!dir || !dir.isDirectory()) return false;
        bool erased = true;
        for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
            if (String(entry.name()).startsWith(".deleted")) continue;
            const String child = path + "/" + entry.name();
            entry.close();
            erased &= store.remove(child.c_str());
        }
        dir.close();
        return erased;
    };
    if (_externalStorageEnabled && _sd && _sd->isReady())
        ok &= eraseDir(*_sd, sdConversationDir(peerHex));
    if (_flash) ok &= eraseDir(*_flash, conversationDir(peerHex));
    else ok = false;
    _ensuredDirs.erase(peerHex);
    _conversations.erase(std::remove(_conversations.begin(), _conversations.end(), peerHex), _conversations.end());
    _summaries.erase(peerHex);
    bumpRevision();
    return ok;
}

bool MessageStore::markConversationRead(const std::string& peerHex) {
    handheld::assertDeviceOwner();
    bool ok = true, changed = false;
    for (const auto& filename : messageFiles(peerHex)) {
        if (!hasDirectionSuffix(filename, 'i')) continue;
        JsonDocument doc;
        const String json = readMessageJson(peerHex, filename);
        if (json.isEmpty() || deserializeJson(doc, json)) { ok = false; continue; }
        if (doc["read"] | false) continue;
        const uint32_t revision = doc["store_revision"] | uint32_t(0);
        if (revision == UINT32_MAX) { ok = false; continue; }
        doc["read"] = true; doc["store_revision"] = revision + 1;
        String updated;
        if (doc.overflowed() || serializeJson(doc, updated) != measureJson(doc) ||
            !writeMessageJson(peerHex, filename, updated)) { ok = false; continue; }
        changed = true;
    }
    if (changed) { rebuildSummary(peerHex); bumpRevision(); }
    return ok;
}

bool MessageStore::updateMessageStatus(const std::string& peerHex, double timestamp, bool incoming, LXMFStatus newStatus) {
    handheld::assertDeviceOwner();
    const auto files = messageFiles(peerHex);
    for (auto it = files.rbegin(); it != files.rend(); ++it) {
        if (!hasDirectionSuffix(*it, incoming ? 'i' : 'o')) continue;
        JsonDocument doc;
        const String json = readMessageJson(peerHex, *it);
        if (json.isEmpty() || deserializeJson(doc, json)) continue;
        if ((doc["ts"] | 0.0) == timestamp)
            return updateMessageStatusByCounter(peerHex, counterFromFilename(*it), incoming, newStatus);
    }
    return false;
}

bool MessageStore::updateMessageStatusByCounter(const std::string& peerHex, uint32_t counter, bool incoming, LXMFStatus newStatus) {
    handheld::assertDeviceOwner();
    if (!counter) return false;
    char filename[64];
    snprintf(filename, sizeof(filename), "%013lu_%c.json", (unsigned long)counter, incoming ? 'i' : 'o');
    JsonDocument doc;
    const String json = readMessageJson(peerHex, String(filename));
    if (json.isEmpty() || deserializeJson(doc, json)) return false;
    const auto oldStatus = LXMFStatus(doc["status"] | 0);
    const uint32_t revision = doc["store_revision"] | uint32_t(0);
    if (revision == UINT32_MAX) return false;
    doc["status"] = int(newStatus); doc["store_revision"] = revision + 1;
    String updated;
    if (doc.overflowed() || serializeJson(doc, updated) != measureJson(doc) ||
        !writeMessageJson(peerHex, String(filename), updated)) return false;
    if (!incoming) updateSummaryStatus(peerHex, counter, oldStatus, newStatus);
    else bumpRevision();
    return true;
}

ConversationSummary MessageStore::buildSummaryForPeer(
    const std::string& peerHex,
    std::vector<LXMFMessage>* pendingOut,
    std::vector<std::pair<uint32_t, std::string>>* recentIds) const {
    handheld::assertDeviceOwner();
    unsigned long startMs = millis();
    ConversationSummary summary;

    const auto files = messageFiles(peerHex);
    summary.totalCount = (int)files.size();
    if (files.empty()) return summary;

    const bool collectStartup = pendingOut || recentIds;

    bool lastDone = false;
    for (int i = (int)files.size() - 1; i >= 0; i--) {
        const String& fname = files[i];
        const bool incomingFile = hasDirectionSuffix(fname, 'i');
        const bool outgoingFile = hasDirectionSuffix(fname, 'o');
        const bool needIncoming = incomingFile;
        const bool needOutgoing = outgoingFile;
        const bool needLast = !lastDone;
        if (!collectStartup && !needLast && !needIncoming && !needOutgoing) continue;

        String fjson = readMessageJson(peerHex, fname);
        if (fjson.length() == 0) continue;

        JsonDocument fdoc;
        if (deserializeJson(fdoc, fjson)) continue;
        uint32_t counter = counterFromFilename(fname);

        if (needLast) {
            summary.lastTimestamp = fdoc["ts"] | 0.0;
            std::string content = fdoc["content"] | "";
            summary.lastIncoming = fdoc["incoming"] | false;
            std::string prefix = summary.lastIncoming ? "Them: " : "You: ";
            if (content.size() > 15) content = content.substr(0, 15) + "...";
            summary.lastPreview = prefix + content;
            lastDone = true;
        }

        if (recentIds) {
            std::string msgIdHex = fdoc["msgid"] | "";
            if (!msgIdHex.empty()) {
                // Bounded (heap: no-PSRAM boards) — keep the NEWEST by counter;
                // consumers read the last <=100 once at boot.
                if (recentIds->size() < STARTUP_RECENT_CAP) {
                    recentIds->push_back({counter, msgIdHex});
                } else {
                    auto minIt = std::min_element(recentIds->begin(), recentIds->end(),
                        [](const auto& a, const auto& b) { return a.first < b.first; });
                    if (counter > minIt->first) *minIt = {counter, msgIdHex};
                }
            }
        }

        if (needIncoming) {
            bool isRead = fdoc["read"] | false;
            if (!isRead) summary.unreadCount++;
        }

        if (needOutgoing) {
            LXMFStatus status = (LXMFStatus)(fdoc["status"] | 0);
            if (!summary.hasOutgoing) {
                summary.hasOutgoing = true;
                summary.lastOutgoingStatus = status;
                summary.lastOutgoingCounter = counter;
            }
            if (isPendingStatus(status)) {
                summary.hasPending = true;
                if (summary.pendingCount < UINT16_MAX) summary.pendingCount++;
                if (pendingOut) {
                    LXMFMessage msg;
                    std::string srcHex = fdoc["src"] | "";
                    std::string dstHex = fdoc["dst"] | "";
                    if (!srcHex.empty()) {
                        msg.sourceHash = rs::Bytes();
                        msg.sourceHash.assignHex(srcHex.c_str());
                    }
                    if (!dstHex.empty()) {
                        msg.destHash = rs::Bytes();
                        msg.destHash.assignHex(dstHex.c_str());
                    }
                    msg.timestamp = fdoc["ts"] | 0.0;
                    msg.content = fdoc["content"] | "";
                    msg.title = fdoc["title"] | "";
                    msg.incoming = fdoc["incoming"] | false;
                    msg.status = status;
                    msg.read = fdoc["read"] | false;
                    msg.savedCounter = counter;
                    std::string msgIdHex = fdoc["msgid"] | "";
                    if (!msgIdHex.empty()) {
                        msg.messageId = rs::Bytes();
                        msg.messageId.assignHex(msgIdHex.c_str());
                    }
                    // Bounded (heap: no-PSRAM boards) — keep the OLDEST by counter,
                    // matching the requeue order; consumers cap at the queue size anyway.
                    if (pendingOut->size() < STARTUP_PENDING_CAP) {
                        pendingOut->push_back(msg);
                    } else {
                        auto maxIt = std::max_element(pendingOut->begin(), pendingOut->end(),
                            [](const LXMFMessage& a, const LXMFMessage& b) {
                                return a.savedCounter < b.savedCounter;
                            });
                        if (msg.savedCounter < maxIt->savedCounter) *maxIt = std::move(msg);
                    }
                }
            }
            if (status == LXMFStatus::FAILED) {
                summary.hasFailed = true;
                if (summary.failedCount < UINT16_MAX) summary.failedCount++;
            }
        }
    }

    unsigned long elapsed = millis() - startMs;
    if (elapsed > 25) {
        Serial.printf("[PERF] MSG summary: %s files=%d unread=%d pending=%u failed=%u in %lums\n",
                      peerHex.substr(0, 8).c_str(), (int)files.size(), summary.unreadCount,
                      (unsigned)summary.pendingCount, (unsigned)summary.failedCount,
                      (unsigned long)elapsed);
    }
    return summary;
}

void MessageStore::rebuildSummary(const std::string& peerHex) {
    handheld::assertDeviceOwner();
    _summaries[peerHex] = buildSummaryForPeer(peerHex);
}

void MessageStore::updateSummaryStatus(const std::string& peerHex, uint32_t counter,
                                       LXMFStatus oldStatus, LXMFStatus newStatus) {
    handheld::assertDeviceOwner();
    if (oldStatus == newStatus) {
        return;
    }

    auto it = _summaries.find(peerHex);
    if (it == _summaries.end() || counter == 0) {
        rebuildSummary(peerHex);
        bumpRevision();
        return;
    }

    ConversationSummary& s = it->second;
    if (isPendingStatus(oldStatus) && s.pendingCount > 0) s.pendingCount--;
    if (oldStatus == LXMFStatus::FAILED && s.failedCount > 0) s.failedCount--;
    if (isPendingStatus(newStatus) && s.pendingCount < UINT16_MAX) s.pendingCount++;
    if (newStatus == LXMFStatus::FAILED && s.failedCount < UINT16_MAX) s.failedCount++;

    s.hasPending = s.pendingCount > 0;
    s.hasFailed = s.failedCount > 0;
    if (counter >= s.lastOutgoingCounter) {
        s.hasOutgoing = true;
        s.lastOutgoingCounter = counter;
        s.lastOutgoingStatus = newStatus;
    }
    bumpRevision();
}

void MessageStore::buildSummaries() {
    handheld::assertDeviceOwner();
    _summaries.clear();
    _startupPendingOutgoing.clear();
    _startupRecentMessageIds.clear();
    std::vector<std::pair<uint32_t, std::string>> recentIds;
    for (const auto& peerHex : _conversations) {
        _summaries[peerHex] = buildSummaryForPeer(peerHex, &_startupPendingOutgoing, &recentIds);
        yield();
    }

    std::sort(_startupPendingOutgoing.begin(), _startupPendingOutgoing.end(),
        [](const LXMFMessage& a, const LXMFMessage& b) {
            return a.savedCounter < b.savedCounter;
        });

    std::sort(recentIds.begin(), recentIds.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });
    std::set<std::string> seen;
    for (const auto& item : recentIds) {
        if (seen.insert(item.second).second) _startupRecentMessageIds.push_back(item.second);
    }

    Serial.printf("[MSGSTORE] Built summaries for %d conversations\n", (int)_summaries.size());
}

std::vector<std::string> MessageStore::startupRecentMessageIds(size_t maxIds) const {
    handheld::assertDeviceOwner();
    if (maxIds == 0 || _startupRecentMessageIds.size() <= maxIds) {
        return _startupRecentMessageIds;
    }
    return std::vector<std::string>(
        _startupRecentMessageIds.end() - maxIds,
        _startupRecentMessageIds.end());
}

const ConversationSummary* MessageStore::getSummary(const std::string& peerHex) const {
    handheld::assertDeviceOwner();
    auto it = _summaries.find(peerHex);
    return (it != _summaries.end()) ? &it->second : nullptr;
}

int MessageStore::totalUnreadCount() const {
    handheld::assertDeviceOwner();
    int total = 0;
    for (const auto& kv : _summaries) total += kv.second.unreadCount;
    return total;
}

void MessageStore::bumpRevision() {
    handheld::assertDeviceOwner();
    _revision++;
    if (_revision == 0) _revision = 1;
}

String MessageStore::conversationDir(const std::string& peerHex) const {
    handheld::assertDeviceOwner();
    return String(PATH_MESSAGES) + "/" + peerHex.c_str();
}

String MessageStore::sdConversationDir(const std::string& peerHex) const {
    handheld::assertDeviceOwner();
    return String(SD_PATH_MESSAGES) + "/" + peerHex.c_str();
}

void MessageStore::enforceFlashLimit(const std::string& peerHex) {
    handheld::assertDeviceOwner();
    if (!_flash) return;
    String dir = conversationDir(peerHex);
    std::vector<String> files;
    File d = _flash->openDir(dir);
    if (!d || !d.isDirectory()) return;
    std::vector<String> junk;
    File entry = d.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) {
            if (isJsonFile(entry.name())) {
                files.push_back(String(dir) + "/" + entry.name());
            } else {
                junk.push_back(String(dir) + "/" + entry.name());
            }
        }
        entry = d.openNextFile();
    }
    // Interrupted-writeAtomic artifacts: an orphaned .bak whose primary is gone is
    // the old content of a rename-window crash — restore it (lazy equivalent of the
    // /config-class boot recovery); everything else is stale and removed.
    for (auto& j : junk) {
        if (j.endsWith(".bak")) {
            String primary = j.substring(0, j.length() - 4);
            if (!_flash->exists(primary.c_str())) {
                if (_flash->rename(j.c_str(), primary.c_str())) files.push_back(primary);
                // A failed restore must never delete the only committed copy.
                continue;
            }
            uint32_t revision = 0;
            if (!messageRevision(_flash->readString(primary.c_str()), revision)) continue;
        }
        if (j.endsWith(".tmp") || j.endsWith(".bak")) _flash->remove(j);
    }
    int limit = (_externalStorageEnabled && _sd && _sd->isReady()) ? FLASH_MSG_CACHE_LIMIT : RSDECK_MAX_MESSAGES_PER_CONV;
    if ((int)files.size() <= limit) return;
    std::sort(files.begin(), files.end());
    int excess = files.size() - limit;
    for (size_t i = 0; i < files.size() && excess > 0; i++) {
        if (_externalStorageEnabled && _sd && _sd->isReady()) {
            // The small flash cache may discard only a verified SD mirror,
            // never the sole copy left by a failed/partial SD write.
            const String filename = files[i].substring(files[i].lastIndexOf('/') + 1);
            const String flashJson = _flash->readString(files[i].c_str());
            const String sdJson = _sd->readString((sdConversationDir(peerHex) + "/" + filename).c_str());
            uint32_t revision = 0;
            if (!messageRevision(flashJson, revision) || flashJson != sdJson) continue;
        }
        if (_flash->remove(files[i])) --excess;
    }
}

void MessageStore::enforceSDLimit(const std::string& peerHex) {
    handheld::assertDeviceOwner();
    if (!_externalStorageEnabled || !_sd || !_sd->isReady()) return;
    String dir = sdConversationDir(peerHex);
    std::vector<String> files;
    File d = _sd->openDir(dir.c_str());
    if (!d || !d.isDirectory()) return;
    std::vector<String> junk;
    File entry = d.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) {
            if (isJsonFile(entry.name())) {
                files.push_back(dir + "/" + entry.name());
            } else {
                junk.push_back(dir + "/" + entry.name());
            }
        }
        entry = d.openNextFile();
    }
    // Orphaned .bak with no primary = rename-window crash remnant: restore, don't delete.
    for (auto& j : junk) {
        if (j.endsWith(".bak")) {
            String primary = j.substring(0, j.length() - 4);
            if (!_sd->exists(primary.c_str())) {
                if (_sd->rename(j.c_str(), primary.c_str())) files.push_back(primary);
                continue;
            }
            uint32_t revision = 0;
            if (!messageRevision(_sd->readString(primary.c_str()), revision)) continue;
        }
        if (j.endsWith(".tmp") || j.endsWith(".bak")) _sd->remove(j.c_str());
    }
    if ((int)files.size() <= RSDECK_MAX_MESSAGES_PER_CONV) return;
    std::sort(files.begin(), files.end());
    int excess = files.size() - RSDECK_MAX_MESSAGES_PER_CONV;
    for (int i = 0; i < excess; i++) {
        _sd->remove(files[i].c_str());
    }
}

#if LEGACY_MSG_FILENAME_MIGRATION
// Pre-2.0 cardputer filenames lacked the 13-digit zero-padded counter prefix.
// Rename pass preserving chronological order (donor migrateOldFilenames).
// Crash-idempotent: the counter seeds from the highest already-migrated prefix
// (a re-run after a mid-pass crash must never rename onto run-1 outputs).
bool MessageStore::migrateOldFilenames() {
    handheld::assertDeviceOwner();
    int totalMigrated = 0;
    bool complete = true;

    auto isOldFormat = [](const String& name) -> bool {
        if (!name.endsWith("_i.json") && !name.endsWith("_o.json")) return false;
        int up = name.indexOf('_');
        if (up <= 0) return false;
        // New format: exactly 13 zero-padded digits before '_'
        if (up == 13 && name.charAt(0) == '0') return false;
        return true;
    };

    uint32_t maxNewPrefix = 0;
    auto scanNewFormat = [&](File d) {
        if (d && d.isDirectory()) {
            File entry = d.openNextFile();
            while (entry) {
                if (!entry.isDirectory()) {
                    String name = entry.name();
                    if (!isOldFormat(name)) {
                        unsigned long val = strtoul(name.c_str(), nullptr, 10);
                        if (val > maxNewPrefix && val < 1000000000) maxNewPrefix = (uint32_t)val;
                    }
                }
                entry.close();
                entry = d.openNextFile();
            }
        }
        if (d) d.close();
    };

    struct OldFile { String name; unsigned long prefix; };

    auto collectPeerDirs = [](File root, const char* basePath) {
        std::vector<String> peerDirs;
        if (root && root.isDirectory()) {
            File peerDir = root.openNextFile();
            while (peerDir) {
                if (peerDir.isDirectory()) {
                    peerDirs.push_back(String(basePath) + "/" + peerDir.name());
                }
                peerDir.close();
                peerDir = root.openNextFile();
            }
        }
        if (root) root.close();
        return peerDirs;
    };

    auto collectOldFiles = [&](File d) {
        std::vector<OldFile> oldFiles;
        if (d && d.isDirectory()) {
            File entry = d.openNextFile();
            while (entry) {
                if (!entry.isDirectory()) {
                    String name = entry.name();
                    if (isOldFormat(name)) {
                        oldFiles.push_back({name, strtoul(name.c_str(), nullptr, 10)});
                    }
                }
                entry.close();
                entry = d.openNextFile();
            }
        }
        if (d) d.close();
        std::sort(oldFiles.begin(), oldFiles.end(),
            [](const OldFile& a, const OldFile& b) { return a.prefix < b.prefix; });
        return oldFiles;
    };

    // Seed past anything a previous (crashed) pass already produced, both tiers.
    if (_externalStorageEnabled && _sd && _sd->isReady()) {
        for (auto& peerPath : collectPeerDirs(_sd->openDir(SD_PATH_MESSAGES), SD_PATH_MESSAGES)) {
            scanNewFormat(_sd->openDir(peerPath.c_str()));
        }
    }
    if (_flash) {
        for (auto& peerPath : collectPeerDirs(_flash->openDir(PATH_MESSAGES), PATH_MESSAGES)) {
            scanNewFormat(_flash->openDir(peerPath));
        }
    }
    uint32_t counter = maxNewPrefix + 1;

    if (_externalStorageEnabled && _sd && _sd->isReady()) {
        for (auto& peerPath : collectPeerDirs(_sd->openDir(SD_PATH_MESSAGES), SD_PATH_MESSAGES)) {
            for (auto& f : collectOldFiles(_sd->openDir(peerPath.c_str()))) {
                int up = f.name.indexOf('_');
                String suffix = f.name.substring(up);
                char newName[64];
                snprintf(newName, sizeof(newName), "%013lu%s", (unsigned long)counter, suffix.c_str());
                counter++;

                String oldPath = peerPath + "/" + f.name;
                String newPath = peerPath + "/" + newName;
                String json = _sd->readString(oldPath.c_str());
                if (json.length() > 0) {
                    if (_sd->writeString(newPath.c_str(), json) && _sd->remove(oldPath.c_str())) totalMigrated++;
                    else complete = false;
                } else complete = false;
                yield();
            }
        }
    }

    if (_flash) {
        for (auto& peerPath : collectPeerDirs(_flash->openDir(PATH_MESSAGES), PATH_MESSAGES)) {
            for (auto& f : collectOldFiles(_flash->openDir(peerPath))) {
                int up = f.name.indexOf('_');
                String suffix = f.name.substring(up);
                char newName[64];
                snprintf(newName, sizeof(newName), "%013lu%s", (unsigned long)counter, suffix.c_str());
                counter++;

                if (_flash->rename(peerPath + "/" + f.name, peerPath + "/" + newName)) totalMigrated++;
                else complete = false;
                yield();
            }
        }
    }

    if (totalMigrated > 0) {
        // A donor-era msgctr smaller than the prefixes this pass produced would
        // collide on the next save — bump NVS past everything we just named.
        Preferences p;
        p.begin(NVS_NS_MSG, false);
        if (p.getUInt("msgctr", 0) < counter) p.putUInt("msgctr", counter);
        p.end();
        Serial.printf("[MSGSTORE] Migrated %d old filenames to new format\n", totalMigrated);
    }
    return complete;
}
#endif  // LEGACY_MSG_FILENAME_MIGRATION

#if LEGACY_READ_CTR_MIGRATION
// One-time cardputer upgrade: fold per-conversation .read_ctr markers into the
// read:true flag core stores inside each incoming JSON. Idempotent/resumable:
// files are rewritten first, the marker is deleted last.
void MessageStore::migrateReadMarkers() {
    handheld::assertDeviceOwner();
    auto migrateDir = [&](auto openFn, auto readFn, auto writeFn, auto removeFn, const String& convDir) {
        String marker = convDir + "/.read_ctr";
        String val = readFn(marker.c_str());
        if (val.length() == 0) return;
        uint32_t lastRead = strtoul(val.c_str(), nullptr, 10);

        std::vector<String> incomingFiles;
        File d = openFn(convDir.c_str());
        if (d && d.isDirectory()) {
            File entry = d.openNextFile();
            while (entry) {
                if (!entry.isDirectory()) {
                    String name = entry.name();
                    if (isJsonFile(name.c_str()) && hasDirectionSuffix(name, 'i') &&
                        counterFromFilename(name) <= lastRead) {
                        incomingFiles.push_back(name);
                    }
                }
                entry.close();
                entry = d.openNextFile();
            }
        }
        if (d) d.close();

        int rewritten = 0;
        bool complete = true;
        for (const auto& fname : incomingFiles) {
            String path = convDir + "/" + fname;
            String json = readFn(path.c_str());
            if (json.length() == 0) { complete = false; continue; }
            JsonDocument doc;
            if (deserializeJson(doc, json)) { complete = false; continue; }
            if (doc["read"] | false) continue;
            doc["read"] = true;
            String updated;
            if (doc.overflowed() || serializeJson(doc, updated) != measureJson(doc) ||
                !writeFn(path.c_str(), updated)) { complete = false; continue; }
            rewritten++;
            yield();
        }
        if (complete) removeFn(marker.c_str());
        Serial.printf("[MSGSTORE] read_ctr migrated: %s ctr=%lu rewrote=%d\n",
                      convDir.c_str(), (unsigned long)lastRead, rewritten);
    };

    auto collectConvDirs = [](File root, const char* basePath) {
        std::vector<String> dirs;
        if (root && root.isDirectory()) {
            File peer = root.openNextFile();
            while (peer) {
                if (peer.isDirectory()) dirs.push_back(String(basePath) + "/" + peer.name());
                peer.close();
                peer = root.openNextFile();
            }
        }
        if (root) root.close();
        return dirs;
    };

    if (_externalStorageEnabled && _sd && _sd->isReady()) {
        for (auto& dir : collectConvDirs(_sd->openDir(SD_PATH_MESSAGES), SD_PATH_MESSAGES)) {
            migrateDir([&](const char* p) { return _sd->openDir(p); },
                       [&](const char* p) { return _sd->readString(p); },
                       [&](const char* p, const String& s) { return _sd->writeString(p, s); },
                       [&](const char* p) { _sd->remove(p); },
                       dir);
        }
    }
    if (_flash) {
        for (auto& dir : collectConvDirs(_flash->openDir(PATH_MESSAGES), PATH_MESSAGES)) {
            migrateDir([&](const char* p) { return _flash->openDir(p); },
                       [&](const char* p) { return _flash->readString(p); },
                       [&](const char* p, const String& s) { return _flash->writeString(p, s); },
                       [&](const char* p) { _flash->remove(p); },
                       dir);
        }
    }
}
#endif  // LEGACY_READ_CTR_MIGRATION
