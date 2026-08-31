// Direct port from Ratputer — node discovery and contact persistence
#include "AnnounceManager.h"
#include "runtime/TaskOwner.h"
#include "config/Config.h"
#include "storage/SDStore.h"
#include "storage/FlashStore.h"
#include "transport/LoRaInterface.h"
#include "util/PerfTrace.h"
#include <ArduinoJson.h>
#include <LittleFS.h>

// Skip one MsgPack value at data[pos], return new pos (or len on error)
static size_t mpSkipValue(const uint8_t* data, size_t len, size_t pos) {
    if (pos >= len) return len;
    uint8_t b = data[pos];
    if (b <= 0x7F || b >= 0xE0) return pos + 1;
    if ((b & 0xF0) == 0x80) {
        size_t n = (b & 0x0F) * 2;
        pos++;
        for (size_t j = 0; j < n && pos < len; j++) pos = mpSkipValue(data, len, pos);
        return pos;
    }
    if ((b & 0xF0) == 0x90) {
        size_t n = b & 0x0F;
        pos++;
        for (size_t j = 0; j < n && pos < len; j++) pos = mpSkipValue(data, len, pos);
        return pos;
    }
    if ((b & 0xE0) == 0xA0) return pos + 1 + (b & 0x1F);
    if (b == 0xC0 || b == 0xC2 || b == 0xC3) return pos + 1;
    if (b == 0xC4 && pos + 1 < len) return pos + 2 + data[pos + 1];
    if (b == 0xC5 && pos + 2 < len) return pos + 3 + ((size_t)data[pos + 1] << 8 | data[pos + 2]);
    if (b == 0xCA) return pos + 5;
    if (b == 0xCB) return pos + 9;
    if (b == 0xCC || b == 0xD0) return pos + 2;
    if (b == 0xCD || b == 0xD1) return pos + 3;
    if (b == 0xCE || b == 0xD2) return pos + 5;
    if (b == 0xCF || b == 0xD3) return pos + 9;
    if (b == 0xD9 && pos + 1 < len) return pos + 2 + data[pos + 1];
    if (b == 0xDA && pos + 2 < len) return pos + 3 + ((size_t)data[pos + 1] << 8 | data[pos + 2]);
    return len;
}

// Two-pass: prefer str elements (display name), fall back to bin (NomadNet compat).
static std::string extractMsgPackName(const uint8_t* data, size_t len) {
    if (len < 2) return "";
    uint8_t b = data[0];
    size_t pos = 0;
    size_t arrLen = 0;
    if ((b & 0xF0) == 0x90) { arrLen = b & 0x0F; if (arrLen == 0) return ""; pos = 1; }
    else if (b == 0xDC && len >= 3) { arrLen = ((size_t)data[1] << 8) | data[2]; pos = 3; }
    else return "";

    // Pass 1: scan for first non-empty STR element
    size_t savedPos = pos;
    for (size_t i = 0; i < arrLen && pos < len; i++) {
        b = data[pos];
        size_t slen = 0;
        if ((b & 0xE0) == 0xA0) { slen = b & 0x1F; pos++; }
        else if (b == 0xD9 && pos + 1 < len) { slen = data[pos + 1]; pos += 2; }
        else if (b == 0xDA && pos + 2 < len) { slen = ((size_t)data[pos + 1] << 8) | data[pos + 2]; pos += 3; }
        else { pos = mpSkipValue(data, len, pos); continue; }
        if (slen > 0 && pos + slen <= len) return std::string((const char*)&data[pos], slen);
        pos += slen;
    }

    // Pass 2: scan for first non-empty BIN element (fallback)
    pos = savedPos;
    for (size_t i = 0; i < arrLen && pos < len; i++) {
        b = data[pos];
        size_t slen = 0;
        if (b == 0xC4 && pos + 1 < len) { slen = data[pos + 1]; pos += 2; }
        else if (b == 0xC5 && pos + 2 < len) { slen = ((size_t)data[pos + 1] << 8) | data[pos + 2]; pos += 3; }
        else { pos = mpSkipValue(data, len, pos); continue; }
        if (slen > 0 && pos + slen <= len) return std::string((const char*)&data[pos], slen);
        pos += slen;
    }
    return "";
}

// Character filter — safe displayable characters including UTF-8 multibyte
// (lvgl renders missing glyphs as fallback boxes — cosmetic, accepted)
static std::string sanitizeName(const std::string& raw, size_t maxLen = 32) {
    std::string clean;
    clean.reserve(std::min(raw.size(), maxLen));
    const uint8_t* p = (const uint8_t*)raw.data();
    size_t sz = raw.size();
    for (size_t i = 0; i < sz && clean.size() < maxLen; ) {
        uint8_t c = p[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == ' ' || c == '-' || c == '_' || c == '.' || c == '\'' || c == '/') {
            clean += (char)c;
            i++;
        }
        // UTF-8 multibyte: pass through valid sequences (emoji/accents)
        else if ((c & 0xE0) == 0xC0 && i + 1 < sz && (p[i+1] & 0xC0) == 0x80) {
            clean.append((const char*)&p[i], 2); i += 2;
        }
        else if ((c & 0xF0) == 0xE0 && i + 2 < sz &&
                 (p[i+1] & 0xC0) == 0x80 && (p[i+2] & 0xC0) == 0x80) {
            clean.append((const char*)&p[i], 3); i += 3;
        }
        else if ((c & 0xF8) == 0xF0 && i + 3 < sz &&
                 (p[i+1] & 0xC0) == 0x80 && (p[i+2] & 0xC0) == 0x80 && (p[i+3] & 0xC0) == 0x80) {
            clean.append((const char*)&p[i], 4); i += 4;
        }
        // Strip control chars, invalid bytes
        else { i++; }
    }
    size_t start = clean.find_first_not_of(' ');
    if (start == std::string::npos) return "";
    size_t end = clean.find_last_not_of(' ');
    return clean.substr(start, end - start + 1);
}

AnnounceManager::AnnounceManager(const char* aspectFilter) {
    (void)aspectFilter;  // aspect filtering moved into the FFI announce bridge
    _nodes.reserve(MAX_NODES);
    _hashIndex.reserve(MAX_NODES);
}

void AnnounceManager::setStorage(SDStore* sd, FlashStore* flash) {
    handheld::assertDeviceOwner(); _sd = sd; _flash = flash; }

void AnnounceManager::receivedAnnounceEvent(const uint8_t destHash[16], const uint8_t identityHash[16],
                                            const uint8_t* appData, size_t appLen, int rssi,
                                            float snr, uint8_t hops) {
    handheld::assertDeviceOwner();
    rs::Bytes dh(destHash, 16);
    if (_localDestHash.size() > 0 && dh == _localDestHash) return;

    // Global announce rate limit (same policy as received_announce).
    unsigned long now = millis();
    if (now - _globalAnnounceWindowStart >= 1000) {
        _globalAnnounceWindowStart = now;
        _globalAnnounceCount = 0;
    }
    if (++_globalAnnounceCount > MAX_GLOBAL_ANNOUNCES_PER_SEC) return;
    if (ESP.getFreeHeap() < 25000) evictStale(300000);

    std::string name;
    if (appData && appLen > 0) {
        std::string rawName = extractMsgPackName(appData, appLen);
        if (rawName.empty()) {
            // received_announce parity: bare UTF-8 app_data used as name (Rust raw announces)
            bool isText = appLen <= 64;
            for (size_t i = 0; isText && i < appLen; ) {
                uint8_t c = appData[i];
                if ((c >= 0x20 && c <= 0x7E) || c == 0x09) { i++; }
                else if ((c & 0xE0) == 0xC0 && i + 1 < appLen &&
                         (appData[i+1] & 0xC0) == 0x80) { i += 2; }
                else if ((c & 0xF0) == 0xE0 && i + 2 < appLen &&
                         (appData[i+1] & 0xC0) == 0x80 &&
                         (appData[i+2] & 0xC0) == 0x80) { i += 3; }
                else if ((c & 0xF8) == 0xF0 && i + 3 < appLen &&
                         (appData[i+1] & 0xC0) == 0x80 &&
                         (appData[i+2] & 0xC0) == 0x80 &&
                         (appData[i+3] & 0xC0) == 0x80) { i += 4; }
                else { isText = false; }
            }
            if (isText) rawName = std::string((const char*)appData, appLen);
        }
        name = sanitizeName(rawName);
    }
    static const char* H = "0123456789abcdef";
    std::string idHex;
    idHex.reserve(32);
    for (int i = 0; i < 16; i++) {
        idHex.push_back(H[identityHash[i] >> 4]);
        idHex.push_back(H[identityHash[i] & 0xF]);
    }
    std::string key = makeKey(dh);
    std::string destHex = dh.toHex();

    auto it = _hashIndex.find(key);
    if (it != _hashIndex.end()) {
        auto& node = _nodes[it->second];
        if (node.lastSeen != 0 && now >= node.lastSeen &&
            now - node.lastSeen < ANNOUNCE_MIN_INTERVAL_MS)
            return;
        // Boards with a rename UI preserve the user's alias. Boards without
        // one keep saved contacts aligned with their latest announced name.
        if (!name.empty() && (!node.saved || !HAS_CONTACT_RENAME)) node.name = name;
        node.identityHex = idHex;
        node.lastSeen = now;
        node.hops = hops;
        if (rssi != 0) node.rssi = rssi;
        if (snr != 0) node.snr = snr;
        if (node.saved) _contactsDirty = true;
        if (!name.empty()) {
            auto nc = _nameCache.find(destHex);
            if (nc == _nameCache.end() || nc->second != name) {
                _nameCache[destHex] = name;
                _nameCacheWrites.changed();
                saveNameCache();
            }
        }
        return;
    }

    if (!name.empty()) {
        auto nc = _nameCache.find(destHex);
        if (nc == _nameCache.end() || nc->second != name) {
            _nameCache[destHex] = name;
            _nameCacheWrites.changed();
            if ((int)_nameCache.size() > MAX_NAME_CACHE) {
                for (auto nit = _nameCache.begin();
                     nit != _nameCache.end() && (int)_nameCache.size() > MAX_NAME_CACHE;) {
                    rs::Bytes h;
                    h.assignHex(nit->first.c_str());
                    if (!findNode(h)) nit = _nameCache.erase(nit);
                    else ++nit;
                }
            }
            saveNameCache();
        }
    }

    if ((int)_nodes.size() >= MAX_NODES) {
        evictStale();
        if ((int)_nodes.size() >= MAX_NODES) {
            // received_announce parity: evict the worst non-saved node (max hops, then oldest)
            uint8_t maxHops = 0;
            unsigned long oldest = ULONG_MAX;
            int evictIdx = -1;
            for (int i = 0; i < (int)_nodes.size(); i++) {
                if (_nodes[i].saved) continue;
                if (_nodes[i].hops > maxHops ||
                    (_nodes[i].hops == maxHops && _nodes[i].lastSeen < oldest)) {
                    maxHops = _nodes[i].hops;
                    oldest = _nodes[i].lastSeen;
                    evictIdx = i;
                }
            }
            if (evictIdx >= 0) {
                int lastIdx = (int)_nodes.size() - 1;
                if (evictIdx != lastIdx) {
                    std::string swapKey = makeKey(_nodes[lastIdx].hash);
                    _hashIndex[swapKey] = evictIdx;
                    std::swap(_nodes[evictIdx], _nodes[lastIdx]);
                }
                _hashIndex.erase(makeKey(_nodes[lastIdx].hash));
                _nodes.pop_back();
            }
        }
        if ((int)_nodes.size() >= MAX_NODES) return;
    }
    DiscoveredNode node;
    node.hash = dh;
    node.name = !name.empty() ? name : ((_nameCache.count(destHex) && !_nameCache[destHex].empty())
                                            ? _nameCache[destHex]
                                            : destHex.substr(0, 12));
    node.identityHex = idHex;
    node.lastSeen = now;
    node.hops = hops;
    node.rssi = rssi;
    node.snr = snr;
    _hashIndex[key] = (int)_nodes.size();
    _nodes.push_back(node);
    Serial.printf("[ANNOUNCE-RUST] new peer %s name=\"%s\" hops=%u\n", destHex.c_str(), node.name.c_str(),
                  (unsigned)hops);
}

void AnnounceManager::loop() {
    handheld::assertDeviceOwner();
    unsigned long now = millis();
    if (_contactsDirty && now - _lastContactSave >= CONTACT_SAVE_INTERVAL_MS) {
        _lastContactSave = now;
        saveContacts();
    }
    // Deferral safety net only — saves normally happen inline on change.
    // Pre-advance the stamp so a low-heap deferral retries at cadence, not every pass.
    if (_nameCacheWrites.dirty() && now - _lastNameCacheSave >= CONTACT_SAVE_INTERVAL_MS) {
        _lastNameCacheSave = now;
        saveNameCache();
    }
}


int AnnounceManager::nodesOnlineSince(unsigned long maxAgeMs) const {
    handheld::assertDeviceOwner();
    unsigned long now = millis();
    int count = 0;
    for (const auto& n : _nodes) {
        if (n.lastSeen != 0 && now >= n.lastSeen && now - n.lastSeen <= maxAgeMs) count++;
    }
    return count;
}

const DiscoveredNode* AnnounceManager::findNode(const rs::Bytes& hash) const {
    handheld::assertDeviceOwner();
    auto it = _hashIndex.find(makeKey(hash));
    if (it != _hashIndex.end()) return &_nodes[it->second];
    return nullptr;
}

const DiscoveredNode* AnnounceManager::findNodeByHex(const std::string& hexHash) const {
    handheld::assertDeviceOwner();
    rs::Bytes target;
    target.assignHex(hexHash.c_str());
    auto it = _hashIndex.find(makeKey(target));
    if (it != _hashIndex.end()) return &_nodes[it->second];
    // Prefix match fallback (for truncated 16-char conversation hashes)
    for (const auto& n : _nodes) {
        std::string nodeHex = n.hash.toHex();
        if (hexHash.length() < nodeHex.length() &&
            nodeHex.substr(0, hexHash.length()) == hexHash) return &n;
    }
    return nullptr;
}

bool AnnounceManager::addManualContact(const std::string& hexHash, const std::string& name) {
    handheld::assertDeviceOwner();
    rs::Bytes hash;
    hash.assignHex(hexHash.c_str());
    if (hash.size() != 16) return false;
    std::string safeName = sanitizeName(name);
    std::string key = makeKey(hash);

    auto it = _hashIndex.find(key);
    if (it != _hashIndex.end()) {
        auto& node = _nodes[it->second];
        auto candidate = node;
        if (!safeName.empty()) candidate.name = safeName;
        candidate.saved = true;
        if (!saveContact(candidate)) return false;
        node = std::move(candidate);
        return true;
    }

    DiscoveredNode node;
    node.hash = hash;
    if (!safeName.empty()) {
        node.name = safeName;
    } else {
        auto cached = _nameCache.find(hexHash);
        node.name = (cached != _nameCache.end() && !cached->second.empty())
            ? cached->second
            : hexHash.substr(0, 12);
    }
    node.lastSeen = millis();
    node.saved = true;
    if (_nodes.size() >= MAX_NODES) return false;
    if (!saveContact(node)) return false;
    _hashIndex[key] = (int)_nodes.size();
    _nodes.push_back(node);
    return true;
}

bool AnnounceManager::saveNode(const std::string& hexHash) {
    handheld::assertDeviceOwner();
    rs::Bytes hash;
    hash.assignHex(hexHash.c_str());
    auto it = _hashIndex.find(makeKey(hash));
    if (it != _hashIndex.end()) {
        auto& node = _nodes[it->second];
        if (!saveContact(node)) return false;
        node.saved = true;
        // Always persist contact name to name cache
        if (!node.name.empty()) {
            _nameCache[hexHash] = node.name;
            _nameCacheWrites.changed();
        }
        return true;
    }
    return false;
}

void AnnounceManager::unsaveNode(const std::string& hexHash) {
    handheld::assertDeviceOwner();
    rs::Bytes hash;
    hash.assignHex(hexHash.c_str());
    auto it = _hashIndex.find(makeKey(hash));
    if (it != _hashIndex.end()) {
        if (!removeContact(hexHash)) return;
        _nodes[it->second].saved = false;
        Serial.printf("[ANNOUNCE] Removed contact: %s\n", _nodes[it->second].name.c_str());
    }
}

void AnnounceManager::evictStale(unsigned long maxAgeMs) {
    handheld::assertDeviceOwner();
    unsigned long now = millis();
    _nodes.erase(std::remove_if(_nodes.begin(), _nodes.end(),
        [now, maxAgeMs](const DiscoveredNode& n) {
            return !n.saved && (n.lastSeen == 0 || now < n.lastSeen || now - n.lastSeen > maxAgeMs);
        }), _nodes.end());
    rebuildIndex();
}

void AnnounceManager::clearTransientNodes() {
    handheld::assertDeviceOwner();
    int before = _nodes.size();
    _nodes.erase(std::remove_if(_nodes.begin(), _nodes.end(),
        [](const DiscoveredNode& n) { return !n.saved; }), _nodes.end());
    int removed = before - (int)_nodes.size();
    if (removed > 0) {
        Serial.printf("[ANNOUNCE] Cleared %d transient nodes\n", removed);
    }
    rebuildIndex();
}

void AnnounceManager::clearAll() {
    handheld::assertDeviceOwner();
    _nodes.clear();
    _hashIndex.clear();
    _nameCache.clear();
    _contactsDirty = false;
    _contactMirrorsPending.clear();
    _nameCacheWrites.reset();
    _lastNameCacheSave = 0;
    Serial.println("[ANNOUNCE] Cleared all nodes and name cache");
}

void AnnounceManager::rebuildIndex() {
    handheld::assertDeviceOwner();
    _hashIndex.clear();
    for (int i = 0; i < (int)_nodes.size(); i++) {
        _hashIndex[makeKey(_nodes[i].hash)] = i;
    }
}

bool AnnounceManager::commitContact(const std::string& hexHash, const String& json) {
    const std::string filename = hexHash.substr(0, 16) + ".json";
    if (!_flash || !_flash->ensureDir(PATH_CONTACTS) ||
        !_flash->writeString((String(PATH_CONTACTS) + "/" + filename.c_str()).c_str(), json)) return false;
    _contactMirrorsPending.insert(filename);
    flushContactMirrors();
    return true;
}

void AnnounceManager::flushContactMirrors() {
    if (!_flash || !_sd || !_sd->isReady()) return;
    if (!_sd->ensureDir(SD_PATH_CONTACTS)) return;
    for (auto it = _contactMirrorsPending.begin(); it != _contactMirrorsPending.end();) {
        const String json = _flash->readString((String(PATH_CONTACTS) + "/" + it->c_str()).c_str());
        if (!json.isEmpty() && _sd->writeString((String(SD_PATH_CONTACTS) + "/" + it->c_str()).c_str(), json))
            it = _contactMirrorsPending.erase(it);
        else ++it;
    }
}

bool AnnounceManager::saveContact(const DiscoveredNode& node) {
    handheld::assertDeviceOwner();
    JsonDocument doc;
    doc["hash"] = node.hash.toHex(); doc["name"] = node.name;
    String json;
    if (doc.overflowed() || serializeJson(doc, json) != measureJson(doc)) return false;
    return commitContact(node.hash.toHex(), json);
}

bool AnnounceManager::removeContact(const std::string& hexHash) {
    handheld::assertDeviceOwner();
    // Keep a durable deletion marker so an old/removable SD backup cannot
    // resurrect a contact. Saving this contact again replaces the marker.
    JsonDocument doc;
    doc["hash"] = hexHash; doc["deleted"] = true;
    String json;
    if (doc.overflowed() || serializeJson(doc, json) != measureJson(doc)) return false;
    return commitContact(hexHash, json);
}

bool AnnounceManager::deleteContact(int nodeIdx) {
    handheld::assertDeviceOwner();
    if (nodeIdx < 0 || nodeIdx >= (int)_nodes.size()) return false;
    std::string hexHash = _nodes[nodeIdx].hash.toHex();
    if (!removeContact(hexHash)) return false;
    _nameCache.erase(hexHash);
    _nameCacheWrites.changed();
    _nodes.erase(_nodes.begin() + nodeIdx);
    rebuildIndex();
    Serial.printf("[ANNOUNCE] Deleted contact %s\n", hexHash.substr(0, 12).c_str());
    return true;
}

bool AnnounceManager::deleteContactByHex(const std::string& hexHash) {
    handheld::assertDeviceOwner();
    rs::Bytes hash;
    hash.assignHex(hexHash.c_str());
    auto it = _hashIndex.find(makeKey(hash));
    if (it == _hashIndex.end()) return false;
    return deleteContact(it->second);
}

bool AnnounceManager::setContactName(const std::string& hexHash, const std::string& name) {
    handheld::assertDeviceOwner();
    rs::Bytes hash;
    hash.assignHex(hexHash.c_str());
    auto it = _hashIndex.find(makeKey(hash));
    if (it == _hashIndex.end()) return false;
    auto& node = _nodes[it->second];
    std::string safeName = sanitizeName(name);
    auto candidate = node;
    if (!safeName.empty()) candidate.name = safeName;
    candidate.saved = true;
    if (!saveContact(candidate)) return false;
    node = std::move(candidate);
    if (!node.name.empty()) {
        _nameCache[hexHash] = node.name;
        _nameCacheWrites.changed();
    }
    return true;
}

void AnnounceManager::loadContacts() {
    handheld::assertDeviceOwner();
    std::set<std::string> seen;
    auto loadFrom = [&](auto& store, const char* root, bool canonical) {
        File dir = store.openDir(root);
        if (!dir || !dir.isDirectory()) return;
        for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
            if (entry.isDirectory()) continue;
            String name = entry.name();
            if (name.endsWith(".bak")) name = name.substring(0, name.length() - 4);
            if (!name.endsWith(".json") || !seen.insert(name.c_str()).second) continue;
            const String path = String(root) + "/" + name;
            String json = store.readString(path.c_str());
            JsonDocument doc;
            if (json.isEmpty() || deserializeJson(doc, json)) {
                json = store.readString((path + ".bak").c_str());
                if (json.isEmpty() || deserializeJson(doc, json)) continue;
            }
            std::string hexHash = doc["hash"] | "";
            rs::Bytes hash; hash.assignHex(hexHash.c_str());
            if (hash.size() != 16 || hexHash.substr(0, 16) + ".json" != name.c_str()) continue;
            if (!canonical && !commitContact(hexHash, json)) continue;
            if (canonical) _contactMirrorsPending.insert(name.c_str());
            if (doc["deleted"] | false) continue;
            const std::string key = makeKey(hash);
            if (_hashIndex.count(key) || _nodes.size() >= MAX_NODES) continue;
            DiscoveredNode node;
            node.hash = hash;
            node.name = sanitizeName(doc["name"] | "");
            if (node.name.empty()) node.name = hexHash.substr(0, 12);
            // Reachability is boot-relative; persist only address-book data.
            node.saved = true;
            _hashIndex[key] = (int)_nodes.size();
            _nodes.push_back(node);
        }
    };
    if (_flash) loadFrom(*_flash, PATH_CONTACTS, true);
    if (_sd && _sd->isReady()) loadFrom(*_sd, SD_PATH_CONTACTS, false);
    flushContactMirrors();
}

bool AnnounceManager::saveContacts() {
    handheld::assertDeviceOwner();
    bool ok = true;
    for (const auto& n : _nodes) { if (n.saved) ok &= saveContact(n); }
    _contactsDirty = !ok;
    return ok;
}

bool AnnounceManager::flushPending() {
    handheld::assertDeviceOwner();
    flushContactMirrors();
    const bool contacts = !_contactsDirty || saveContacts();
    const bool names = !_nameCacheWrites.dirty() || saveNameCache();
    return contacts && names;
}

std::string AnnounceManager::lookupName(const std::string& hexHash) const {
    handheld::assertDeviceOwner();
    // Check live nodes first
    const DiscoveredNode* node = findNodeByHex(hexHash);
    if (node && !node->name.empty()) return node->name;
    // Fall back to cached names
    auto it = _nameCache.find(hexHash);
    if (it != _nameCache.end()) return it->second;
    return "";
}

bool AnnounceManager::saveNameCache() {
    handheld::assertDeviceOwner();
    _lastNameCacheSave = millis();
    if (!_nameCacheWrites.dirty()) return true;
    _nameCacheWrites.require((_flash ? MirrorWriteState::Flash : 0) |
                            (_sd && _sd->isReady() ? MirrorWriteState::SD : 0));
    if (ESP.getFreeHeap() < 20000) return false;
    JsonDocument doc;
    for (const auto& kv : _nameCache) doc[kv.first] = kv.second;
    String json;
    if (doc.overflowed() || !serializeJson(doc, json)) return false;
    if (_nameCacheWrites.needs(MirrorWriteState::Flash) && _flash)
        _nameCacheWrites.completed(MirrorWriteState::Flash,
            _flash->writeString("/config/names.json", json));
    if (_nameCacheWrites.needs(MirrorWriteState::Flash)) return false;
    if (_nameCacheWrites.needs(MirrorWriteState::SD) && _sd && _sd->isReady())
        _nameCacheWrites.completed(MirrorWriteState::SD,
            _sd->writeString(SD_PATH_CONFIG_DIR "/names.json", json));
    const bool ok = _nameCacheWrites.settle();
    if (!ok) Serial.println("[ANNOUNCE] Name cache persistence incomplete; retry pending");
    return ok;
}

void AnnounceManager::loadNameCache() {
    handheld::assertDeviceOwner();
    String json;
    if (_flash) json = _flash->readString("/config/names.json");
    if (json.isEmpty() && _sd && _sd->isReady()) json = _sd->readString(SD_PATH_CONFIG_DIR "/names.json");
    if (json.isEmpty()) return;
    JsonDocument doc;
    if (deserializeJson(doc, json)) return;
    for (JsonPair kv : doc.as<JsonObject>()) {
        _nameCache[kv.key().c_str()] = kv.value().as<std::string>();
    }
    _nameCacheWrites.changed(); // Repair an older SD mirror from canonical flash.
    Serial.printf("[ANNOUNCE] Name cache loaded (%d entries)\n", (int)_nameCache.size());
}
