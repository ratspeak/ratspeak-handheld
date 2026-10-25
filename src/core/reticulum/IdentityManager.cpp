#include "IdentityManager.h"
#include "storage/StorageLease.h"
#include "runtime/TaskOwner.h"
#include "config/Config.h"
#include "config/UserConfig.h"
#include "protocol/RustEntropy.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <string.h>
#include "ratspeak_protocol.h"

// Derive the 32-hex identity hash from a raw 64-byte slot key via the FFI
// (replaces the retired micro load_private_key/hexhash pair; same format).
static bool identityHashFromKey(const uint8_t key[64], std::string& outHex) {
    uint8_t hash[16];
    if (rs_handheld_rns_validate_identity(key, hash, nullptr) != RS_HANDHELD_OK) return false;
    static const char* kHex = "0123456789abcdef";
    outHex.clear();
    for (int i = 0; i < 16; i++) {
        outHex += kHex[hash[i] >> 4];
        outHex += kHex[hash[i] & 0x0F];
    }
    return true;
}

bool IdentityManager::begin(FlashStore* flash, SDStore* sd) {
    handheld::assertDeviceOwner();
    _flash = flash;
    _sd = sd;
    if (!_flash || !_flash->isReady()) return false;
    _flash->ensureDir(ID_DIR);
    loadSlotMeta();
    if (!_metadataReadable) return false;

    // If no slots exist, import the current active identity as slot 0.
    if (_slots.empty()) {
        if (_flash->exists(PATH_IDENTITY)) {
            IdentitySlot slot;
            slot.keyPath = slotKeyPath(0);
            slot.active = true;

            // Load the key to get its hash
            uint8_t keyData[64];
            size_t bytesRead = 0;
            if (_flash->readFile(PATH_IDENTITY, keyData, sizeof(keyData), bytesRead) &&
                bytesRead == sizeof(keyData)) {
                identityHashFromKey(keyData, slot.hash);
            }
            // On a pre-slot installation, two disagreeing raw key copies give
            // no evidence that either is a newer identity. Preserve both for
            // recovery instead of replacing an intact NVS key during import.
            uint8_t nvsKey[64];
            const bool conflictingNvs = !slot.hash.empty() && readNvsIdentityKey(nvsKey) &&
                memcmp(nvsKey, keyData, sizeof(nvsKey)) != 0;
            memset(nvsKey, 0, sizeof(nvsKey));
            if (conflictingNvs) {
                memset(keyData, 0, sizeof(keyData));
                Serial.println("[IDMGR] Legacy identity copies disagree; preserving identity data");
                return false;
            }
            if (!slot.hash.empty()
                && _flash->writeAtomic(slot.keyPath.c_str(), keyData, sizeof(keyData))) {
                _slots.push_back(slot);
                _activeIdx = 0;
                _metadataDirty = true;
            }
        }
    } else {
        int activeIdx = -1;
        for (int i = 0; i < (int)_slots.size(); i++) {
            if (_slots[i].keyPath == PATH_IDENTITY) {
                String migratedPath = slotKeyPath(i);
                uint8_t keyData[64];
                size_t bytesRead = 0;
                std::string keyHash;
                if (_flash->readFile(PATH_IDENTITY, keyData, sizeof(keyData), bytesRead) &&
                    bytesRead == sizeof(keyData) && identityHashFromKey(keyData, keyHash)) {
                    if ((_slots[i].hash.empty() || _slots[i].hash == keyHash)
                        && _flash->writeAtomic(migratedPath.c_str(), keyData, sizeof(keyData))) {
                        _slots[i].keyPath = migratedPath;
                        if (_slots[i].hash.empty()) _slots[i].hash = keyHash;
                        _metadataDirty = true;
                    }
                }
            }
            if (_slots[i].active && activeIdx < 0) activeIdx = i;
        }
        _activeIdx = activeIdx;
    }

    // Validate the catalogued active slot before reconciling the switch mirror.
    // Stage an invalid marker without discarding independent recovery evidence.
    if (_activeIdx >= 0) {
        uint8_t activeKey[64];
        size_t activeLen = 0;
        std::string activeHash;
        const bool activeValid =
            _flash->readFile(_slots[_activeIdx].keyPath.c_str(), activeKey, sizeof(activeKey),
                             activeLen) &&
            activeLen == sizeof(activeKey) && identityHashFromKey(activeKey, activeHash) &&
            (_slots[_activeIdx].hash.empty() || _slots[_activeIdx].hash == activeHash);
        memset(activeKey, 0, sizeof(activeKey));
        if (!activeValid) {
            _slots[_activeIdx].active = false;
            _activeIdx = -1;
            // Stage the marker repair until a valid recovery source has been
            // selected. A contradictory mirror must not rewrite the catalogue.
            _metadataDirty = true;
        }
    }

    // PATH_IDENTITY is the switch commit point, but a raw 64-byte file is not
    // self-authenticating. Reconcile it against a catalogue hash or an exact
    // unlisted staged slot; an unknown mirror alone cannot replace an identity.
    if (_flash->exists(PATH_IDENTITY) && !adoptPathIdentityWhenNoActive()) return false;

    if (_activeIdx >= 0) mirrorActiveIdentityToNvs();

    Serial.printf("[IDMGR] %d identities, active=%d\n", (int)_slots.size(), _activeIdx);
    return flushPending();
}

bool IdentityManager::adoptPathIdentityWhenNoActive() {
    handheld::assertDeviceOwner();
    if (!_flash || !_flash->exists(PATH_IDENTITY)) return false;
    uint8_t key[64];
    size_t bytesRead = 0;
    std::string keyHash;
    if (!_flash->readFile(PATH_IDENTITY, key, sizeof(key), bytesRead) || bytesRead != sizeof(key) ||
        !identityHashFromKey(key, keyHash)) {
        return false;
    }

    int index = -1;
    for (int i = 0; i < (int)_slots.size(); i++) {
        if (_slots[i].hash == keyHash) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        if ((int)_slots.size() >= MAX_IDENTITIES) return false;
        // createIdentityFromRaw commits a separate slot key before the active
        // mirror and metadata. Recover that interrupted operation only when its
        // exact independent slot bytes still exist. The finite slot namespace
        // is the same one used by nextFreeSlotNum; no directory or body cache.
        String stagedPath;
        for (int slotNum = 0; slotNum < MAX_IDENTITIES * 2; ++slotNum) {
            const String candidate = slotKeyPath(slotNum);
            bool listed = false;
            for (const auto& slot : _slots) if (slot.keyPath == candidate) { listed = true; break; }
            if (listed || !_flash->exists(candidate.c_str())) continue;
            uint8_t staged[64]; size_t length = 0;
            const bool matches = _flash->readFileFully(candidate.c_str(), staged, sizeof(staged), length)
                && length == sizeof(staged) && memcmp(key, staged, sizeof(staged)) == 0;
            memset(staged, 0, sizeof(staged));
            if (matches) { stagedPath = candidate; break; }
        }
        if (stagedPath.isEmpty()) {
            memset(key, 0, sizeof(key));
            Serial.println("[IDMGR] Active mirror has no verified slot; preserving identity data");
            return false;
        }
        IdentitySlot slot;
        slot.hash = keyHash;
        slot.keyPath = stagedPath;
        slot.active = false;
        _slots.push_back(slot);
        index = (int)_slots.size() - 1;
    }
    if (index == _activeIdx) {
        uint8_t existing[64]; size_t length = 0;
        const bool matches = _flash->readFile(_slots[index].keyPath.c_str(), existing,
            sizeof(existing), length) && length == sizeof(existing) && memcmp(key, existing, sizeof(key)) == 0;
        memset(existing, 0, sizeof(existing));
        if (matches) { memset(key, 0, sizeof(key)); return true; }
    }
    if (!_flash->writeAtomic(_slots[index].keyPath.c_str(), key, sizeof(key))) return false;
    for (auto& slot : _slots) slot.active = false;
    _slots[index].active = true;
    _activeIdx = index;
    memset(key, 0, sizeof(key));
    if (!saveSlotMeta()) return false;
    Serial.printf("[IDMGR] Adopted PATH_IDENTITY as active slot %d\n", index);
    return true;
}

String IdentityManager::slotKeyPath(int slotNum) const {
    handheld::assertDeviceOwner();
    char path[48];
    snprintf(path, sizeof(path), "/identity/id_%d.key", slotNum);
    return String(path);
}

int IdentityManager::nextFreeSlotNum() const {
    handheld::assertDeviceOwner();
    // Never derive from _slots.size(): after a mid-vector delete that count
    // collides with a surviving slot's file and would overwrite its key.
    for (int n = 0; n < MAX_IDENTITIES * 2; n++) {
        String kp = slotKeyPath(n);
        bool taken = _flash && _flash->exists(kp.c_str());
        for (const auto& s : _slots) {
            if (s.keyPath == kp) { taken = true; break; }
        }
        if (!taken) return n;
    }
    return -1;
}

String IdentityManager::importIdentityPath() const {
    handheld::assertDeviceOwner();
    if (!_sd || !_sd->isReady()) return "";
    if (_sd->exists(SD_PATH_IMPORT_IDENTITY)) return String(SD_PATH_IMPORT_IDENTITY);
    if (_sd->exists(SD_PATH_IMPORT_ID)) return String(SD_PATH_IMPORT_ID);

    File dir = _sd->openDir(SD_PATH_IDENTITY_DIR);
    if (!dir) return "";
    String candidate;
    int candidates = 0;
    while (true) {
        File entry = dir.openNextFile();
        if (!entry) break;
        if (entry.isDirectory()) {
            entry.close();
            continue;
        }
        String name = entry.name();
        entry.close();
        String lower = name;
        lower.toLowerCase();
        bool reservedIdentityFile = lower == "identity.key" || lower.endsWith("/identity.key") ||
            lower == "identity.identity" || lower.endsWith("/identity.identity");
        bool importCandidate = !reservedIdentityFile &&
            (lower.endsWith(".identity") || lower.endsWith(".key"));
        if (importCandidate) {
            candidate = name;
            if (!candidate.startsWith("/")) {
                int slash = candidate.lastIndexOf('/');
                if (slash >= 0) candidate = candidate.substring(slash + 1);
                candidate = String(SD_PATH_IDENTITY_DIR) + "/" + candidate;
            }
            candidates++;
        }
    }
    dir.close();
    if (candidates == 1) return candidate;
    if (candidates > 1) Serial.println("[IDMGR] Multiple import identity files found on SD");
    return "";
}

int IdentityManager::importIdentity(const String& displayName) {
    handheld::assertDeviceOwner();
    (void)displayName;
    if (!_sd || !_sd->isReady()) return -1;
    if ((int)_slots.size() >= MAX_IDENTITIES) return -1;

    String importPath = importIdentityPath();
    if (importPath.isEmpty()) {
        Serial.println("[IDMGR] No import identity file found on SD");
        return -1;
    }

    uint8_t buffer[64];
    size_t bytesRead = 0;
    if (!_sd->readFile(importPath.c_str(), buffer, sizeof(buffer), bytesRead) || bytesRead != sizeof(buffer)) {
        Serial.println("[IDMGR] Failed to read 64-byte import identity from SD");
        return -1;
    }

    std::string importedHash;
    if (!identityHashFromKey(buffer, importedHash)) {
        Serial.println("[IDMGR] Import identity key failed validation");
        return -1;
    }
    for (const auto& slot : _slots) {
        if (slot.hash == importedHash) {
            Serial.printf("[IDMGR] Imported identity already exists: %s\n",
                          importedHash.substr(0, 16).c_str());
            return -1;
        }
    }

    int slotNum = nextFreeSlotNum();
    if (slotNum < 0) return -1;
    String keyPath = slotKeyPath(slotNum);
    if (!_flash->writeAtomic(keyPath.c_str(), buffer, sizeof(buffer))) {
        Serial.println("[IDMGR] Failed to save imported identity key");
        return -1;
    }

    IdentitySlot slot;
    slot.hash = importedHash;
    slot.displayName = "";
    slot.keyPath = keyPath;
    slot.active = false;
    _slots.push_back(slot);

    if (!saveSlotMeta()) return -1;
    mirrorActiveIdentityToNvs();
    Serial.printf("[IDMGR] Imported identity %d from %s: %s\n",
                  slotNum, importPath.c_str(), slot.hash.substr(0, 16).c_str());
    return (int)_slots.size() - 1;  // vector index (slot file number may differ)
}

int IdentityManager::createIdentity(const String& displayName) {
    handheld::assertDeviceOwner();
    (void)displayName;
    if ((int)_slots.size() >= MAX_IDENTITIES) return -1;

    // Generate new identity from TRNG entropy via the FFI
    uint8_t entropy[64];
    uint8_t privKey[64];
    uint8_t identityHash[16];
    RustEntropy::fill(entropy, sizeof(entropy));
    rs_handheld_status_t st = rs_handheld_rns_create_identity(entropy, privKey, identityHash);
    memset(entropy, 0, sizeof(entropy));
    if (st != RS_HANDHELD_OK) return -1;
    std::string newHash;
    if (!identityHashFromKey(privKey, newHash)) return -1;

    int slotNum = nextFreeSlotNum();
    if (slotNum < 0) return -1;
    String keyPath = slotKeyPath(slotNum);

    if (!_flash->writeAtomic(keyPath.c_str(), privKey, sizeof(privKey))) {
        Serial.println("[IDMGR] Failed to save new identity key");
        return -1;
    }

    IdentitySlot slot;
    slot.hash = newHash;
    slot.displayName = "";  // New identity starts unnamed
    slot.keyPath = keyPath;
    slot.active = false;
    _slots.push_back(slot);

    if (!saveSlotMeta()) return -1;
    mirrorActiveIdentityToNvs();
    Serial.printf("[IDMGR] Created identity %d: %s\n", slotNum, slot.hash.substr(0, 16).c_str());
    return (int)_slots.size() - 1;  // vector index (slot file number may differ)
}

bool IdentityManager::deleteIdentity(int index) {
    handheld::assertDeviceOwner();
    if (index < 0 || index >= (int)_slots.size()) return false;
    if ((int)_slots.size() <= 1) return false;  // Can't delete last identity
    if (_slots[index].active) return false;      // Can't delete active identity

    const auto previous = _slots;
    const int previousActive = _activeIdx;
    const String keyPath = _slots[index].keyPath;
    _slots.erase(_slots.begin() + index);

    // Adjust active index
    if (_activeIdx > index) _activeIdx--;

    // Commit the catalogue before deleting key bytes. A metadata failure must
    // never strand a still-listed identity with its private key removed.
    if (!saveSlotMeta()) {
        _slots = previous; _activeIdx = previousActive; _metadataDirty = false;
        return false;
    }
    if (!_flash->remove(keyPath.c_str())) return false;
    Serial.printf("[IDMGR] Deleted identity %d\n", index);
    return true;
}

bool IdentityManager::switchTo(int index) {
    handheld::assertDeviceOwner();
    if (index < 0 || index >= (int)_slots.size()) return false;
    if (index == _activeIdx) return mirrorActiveIdentityToNvs();

    // Load the key
    uint8_t keyData[64];
    size_t bytesRead = 0;
    if (!_flash->readFile(_slots[index].keyPath.c_str(), keyData, sizeof(keyData), bytesRead) ||
        bytesRead != sizeof(keyData)) {
        Serial.printf("[IDMGR] Failed to read key for slot %d\n", index);
        return false;
    }

    std::string keyHash;
    if (!identityHashFromKey(keyData, keyHash)) {
        Serial.printf("[IDMGR] Failed to load key for slot %d\n", index);
        return false;
    }
    if (!_slots[index].hash.empty() && _slots[index].hash != keyHash) {
        Serial.printf("[IDMGR] Slot %d key hash mismatch; refusing switch\n", index);
        return false;
    }

    // Plain persistence rule: the active slot is always mirrored to PATH_IDENTITY.
    if (!_flash->writeAtomic(PATH_IDENTITY, keyData, sizeof(keyData))) {
        Serial.printf("[IDMGR] Failed to write active identity for slot %d\n", index);
        return false;
    }

    // Mark old active as inactive
    if (_activeIdx >= 0 && _activeIdx < (int)_slots.size()) {
        _slots[_activeIdx].active = false;
    }

    _slots[index].active = true;
    _activeIdx = index;

    if (!saveSlotMeta()) return false;
    if (!mirrorIdentityToNvs(keyData)) return false;
    Serial.printf("[IDMGR] Switched to identity %d: %s\n", index, _slots[index].hash.substr(0, 16).c_str());
    return true;
}

bool IdentityManager::setDisplayName(int index, const String& name, bool complete) {
    handheld::assertDeviceOwner();
    if (!_metadataReadable || index < 0 || index >= (int)_slots.size()) return false;
    String publication;
    if (!UserConfig::trySetString(publication, name.c_str(), name.length())) return false;
    // Serialize a staged name directly into the existing catalogue. A failed
    // write must not publish an attempted name or make dirty retry adopt it.
    const bool wasDirty = _metadataDirty;
    if (!saveSlotMeta(index, &name, complete)) { _metadataDirty = wasDirty; return false; }
    std::swap(_slots[index].displayName, publication);
    _slots[index].nameComplete = complete;
    return _slots[index].displayName == name;
}

bool IdentityManager::validateSlot(int index) const {
    handheld::assertDeviceOwner();
    if (!_metadataReadable || !_flash || index < 0 || index >= (int)_slots.size()) return false;
    for (int other = 0; other < (int)_slots.size(); ++other)
        if (other != index && _slots[other].hash == _slots[index].hash) return false;
    uint8_t key[64], hash[16]; size_t size = 0;
    if (!_flash->readFileFully(_slots[index].keyPath.c_str(), key, sizeof key, size) || size != sizeof key ||
        rs_handheld_rns_validate_identity(key, hash, nullptr) != RS_HANDHELD_OK) return false;
    static constexpr char digits[] = "0123456789abcdef";
    char hex[33]{};
    for (size_t i = 0; i < sizeof hash; ++i) { hex[2*i] = digits[hash[i] >> 4]; hex[2*i+1] = digits[hash[i] & 15]; }
    return _slots[index].hash == hex;
}

String IdentityManager::getDisplayName(int index) const {
    handheld::assertDeviceOwner();
    if (index < 0 || index >= (int)_slots.size()) return "";
    return _slots[index].displayName;
}

bool IdentityManager::syncNameFromActive(String& outName) const {
    handheld::assertDeviceOwner();
    if (_activeIdx < 0 || _activeIdx >= (int)_slots.size()) return false;
    outName = _slots[_activeIdx].displayName;
    return true;
}

void IdentityManager::refresh() {
    handheld::assertDeviceOwner();
    loadSlotMeta();
}

namespace {
constexpr size_t MetadataBytes = 32768; // Existing complete-record read limit.
// The pinned parser retains each string's geometric capacity and can overlap
// old/new blocks while growing the current string: <3x selected input bytes,
// plus the bounded <=8-row nodes, allocator headers and SDK pool reserve.
// This is a ceiling, not an up-front allocation; Card admits each increment.
constexpr size_t MetadataJsonBytes = 3 * MetadataBytes + 8192;

// The pinned SDK owns all JSON parsing. Keep only the five fields consumed by
// this catalogue, so ignored legacy extensions cannot grow the metadata DOM.
// This filter is a stateless value: constructing it cannot allocate or fail.
struct SlotMetadataFilter {
    enum Level : uint8_t { Root, Slots, Slot, Value, Discard } level = Root;
    bool allow() const { return level != Discard; }
    bool allowArray() const { return level == Slots; }
    bool allowObject() const { return level == Root || level == Slot; }
    bool allowValue() const { return level != Discard; }
    SlotMetadataFilter operator[](JsonString key) const {
        const auto is = [&](const char* text) {
            const size_t length = strlen(text);
            return key.size() == length && !memcmp(key.c_str(), text, length);
        };
        if (level == Root && is("slots")) return {Slots};
        if (level == Slot && (is("hash") || is("name") || is("name_complete") ||
                             is("path") || is("active"))) return {Value};
        return {Discard};
    }
    template<class Index> SlotMetadataFilter operator[](Index) const {
        return {level == Slots ? Slot : Discard};
    }
};

bool stageSlotHash(std::string& value, const char* bytes) {
    using handheld::config::Memory;
    const size_t length = strlen(bytes);
    // libstdc++ may double the fresh string's SSO capacity on first growth.
    // Reserve that actual upper bound before assigning; no unchecked copy is
    // used to publish a catalogue row.
    if (length > value.capacity()) {
        const size_t capacity = std::max(length, 2 * value.capacity());
        if (!Memory::admits(Memory::charge(capacity + 1))) return false;
        value.reserve(length);
    }
    value.assign(bytes, length);
    return value.size() == length && (!length || !memcmp(value.data(), bytes, length)) &&
        Memory::admits(0);
}
}

void IdentityManager::loadSlotMeta() {
    handheld::assertDeviceOwner();
    _metadataReadable = false;
    if (!_flash) return;
    try {
        using handheld::config::Memory;
        handheld::config::JsonAllocator allocator(MetadataJsonBytes);
        JsonDocument doc(&allocator);
        {
            String json;
            const auto source = _flash->readRecord(META_PATH, json, MetadataBytes, Memory::reserveString);
            if (source == FlashStore::RecordSource::Absent) {
                _slots.clear(); _activeIdx = -1; _metadataReadable = true; return;
            }
            if (source != FlashStore::RecordSource::Primary && source != FlashStore::RecordSource::Backup) return;
            if (deserializeJson(doc, json.c_str(), json.length(), SlotMetadataFilter{}) ||
                !doc["slots"].is<JsonArray>()) return;
        } // SDK-owned DOM strings outlive this complete input; free it before copies.
        JsonArray arr = doc["slots"];
        if (arr.size() > MAX_IDENTITIES) return;
        for (JsonVariant value : arr) {
            const char* path = value["path"] | "";
            if (!value.is<JsonObject>() || strncmp(path, "/identity/", 10) || strstr(path, "..")) return;
        }
        std::vector<IdentitySlot> prepared;
        if (!Memory::admits(Memory::charge(arr.size() * sizeof(IdentitySlot)))) return;
        prepared.reserve(arr.size());
        if (!Memory::admits(0)) return;
        int active = -1;
        for (JsonObject obj : arr) {
            prepared.emplace_back();
            auto& slot = prepared.back();
            const char* name = obj["name"] | "";
            const char* path = obj["path"] | "";
            if (!stageSlotHash(slot.hash, obj["hash"] | "") ||
                !UserConfig::trySetString(slot.displayName, name, strlen(name)) ||
                !UserConfig::trySetString(slot.keyPath, path, strlen(path))) return;
            slot.nameComplete = obj["name_complete"] | !slot.displayName.isEmpty();
            slot.active = obj["active"] | false;
            if (slot.active && active < 0) active = int(prepared.size()) - 1;
        }
        _slots.swap(prepared);
        _activeIdx = active;
        _metadataReadable = true;
    } catch (const std::bad_alloc&) {
        // Retain the complete old catalogue and block metadata writes until retry.
    }
}

bool IdentityManager::flushPending() {
    handheld::assertDeviceOwner();
    if (!_metadataReadable) return false;
    const bool metadata = !_metadataDirty || saveSlotMeta();
    const bool mirror = !_nvsMirrorPending || mirrorActiveIdentityToNvs();
    return metadata && mirror;
}

bool IdentityManager::saveSlotMeta(int nameIndex, const String* name, bool complete) {
    handheld::assertDeviceOwner();
    _metadataDirty = true;
    if (!_metadataReadable || !_flash || _slots.size() > MAX_IDENTITIES) return false;
    try {
        // Character data can approach the complete 32 KiB legacy record;
        // the same bounded/admitted allocator used by settings covers it.
        handheld::config::JsonAllocator allocator(2 * MetadataBytes);
        JsonDocument doc(&allocator);
        JsonArray arr = doc["slots"].to<JsonArray>();
        for (size_t i = 0; i < _slots.size(); ++i) {
            const auto& slot = _slots[i];
            JsonObject obj = arr.add<JsonObject>();
            obj["hash"] = slot.hash;
            obj["name"] = name && int(i) == nameIndex ? *name : slot.displayName;
            obj["name_complete"] = name && int(i) == nameIndex ? complete : slot.nameComplete;
            obj["path"] = slot.keyPath;
            obj["active"] = slot.active;
        }
        if (doc.overflowed()) return false;
        const size_t length = measureJson(doc);
        String json;
        if (!length || length > MetadataBytes ||
            !handheld::config::Memory::reserveString(json, length) ||
            serializeJson(doc, json) != length || json.length() != length ||
            !handheld::config::Memory::admits(0)) return false;
        _metadataDirty = !_flash->writeString(META_PATH, json);
        return !_metadataDirty;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

bool IdentityManager::readActiveIdentityKey(uint8_t out[64]) const {
    handheld::assertDeviceOwner();
    if (_activeIdx < 0 || _activeIdx >= (int)_slots.size() || !_flash) return false;
    size_t bytesRead = 0;
    if (!_flash->readFile(_slots[_activeIdx].keyPath.c_str(), out, 64, bytesRead) || bytesRead != 64) {
        Serial.printf("[IDMGR] Active slot %d key read failed (%u bytes)\n",
                      _activeIdx, (unsigned)bytesRead);
        return false;
    }
    return true;
}

bool IdentityManager::readNvsIdentityKey(uint8_t out[64]) const {
    handheld::assertDeviceOwner();
    if (!out) return false;
    Preferences prefs;
    if (!prefs.begin(NVS_NS_IDENTITY, true)) return false;
    const size_t len = prefs.getBytesLength("privkey");
    const size_t read = len == 64 ? prefs.getBytes("privkey", out, 64) : 0;
    prefs.end();
    return read == 64;
}

bool IdentityManager::hasIdentityData() const {
    if (!_flash || !_flash->isReady()) return true;
    File dir = _flash->openDir(ID_DIR);
    if (!dir || !dir.isDirectory()) return true;
    for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile())
        if (!entry.isDirectory()) return true;
    Preferences prefs;
    if (!prefs.begin(NVS_NS_IDENTITY, false)) return true;
    const bool present = prefs.getBytesLength("privkey") != 0;
    prefs.end();
    return present;
}

bool IdentityManager::mirrorIdentityToNvs(const uint8_t key[64]) const {
    handheld::assertDeviceOwner();
    if (!key) return false;
    handheld::storage::StorageLease lease;
    if (!lease.held()) { _nvsMirrorPending = true; return false; }
    Preferences prefs;
    if (!prefs.begin(NVS_NS_IDENTITY, false)) { _nvsMirrorPending = true; return false; }
    const bool ok = prefs.putBytes("privkey", key, 64) == 64;
    prefs.end();
    _nvsMirrorPending = !ok;
    if (!ok) Serial.println("[IDMGR] NVS identity mirror failed");
    return ok;
}

bool IdentityManager::mirrorActiveIdentityToNvs() {
    handheld::assertDeviceOwner();
    uint8_t key[64];
    if (!readActiveIdentityKey(key)) return false;
    std::string keyHash;
    if (!identityHashFromKey(key, keyHash) ||
        (!_slots[_activeIdx].hash.empty() && _slots[_activeIdx].hash != keyHash)) {
        memset(key, 0, sizeof(key));
        return false;
    }
    const bool ok = mirrorIdentityToNvs(key);
    memset(key, 0, sizeof(key));
    return ok;
}

int IdentityManager::createIdentityFromRaw(const uint8_t key[64], const char* hashHex) {
    handheld::assertDeviceOwner();
    if (!_flash || (int)_slots.size() >= MAX_IDENTITIES) return -1;
    std::string verifiedHash;
    if (!identityHashFromKey(key, verifiedHash) || (hashHex && verifiedHash != hashHex)) return -1;

    int slotNum = nextFreeSlotNum();
    if (slotNum < 0) return -1;
    String keyPath = slotKeyPath(slotNum);
    if (!_flash->writeAtomic(keyPath.c_str(), key, 64)) {
        Serial.println("[IDMGR] Failed to save rust-created identity key");
        return -1;
    }
    // Plain persistence rule (switchTo semantics): the active slot is always
    // mirrored to PATH_IDENTITY (existing devices + SD-import path rely on it).
    if (!_flash->writeAtomic(PATH_IDENTITY, key, 64)) {
        Serial.println("[IDMGR] Failed to mirror rust-created identity to legacy path");
        return -1;
    }

    IdentitySlot slot;
    slot.hash = verifiedHash;
    slot.displayName = "";
    slot.keyPath = keyPath;
    slot.active = true;
    if (_activeIdx >= 0 && _activeIdx < (int)_slots.size()) _slots[_activeIdx].active = false;
    _slots.push_back(slot);
    _activeIdx = (int)_slots.size() - 1;  // vector index (slot file number may differ)
    if (!saveSlotMeta() || !mirrorIdentityToNvs(key)) return -1;
    Serial.printf("[IDMGR] Created identity %d (rust): %s\n",
                  slotNum, slot.hash.substr(0, 16).c_str());
    return _activeIdx;
}
