// Direct port from Ratputer — node discovery and contact persistence
#include "AnnounceManager.h"
#include "runtime/TaskOwner.h"
#include "config/Config.h"
#include "storage/SDStore.h"
#include "storage/FlashStore.h"
#include "storage/AtomicStream.h"
#include "storage/StorageJsonAllocator.h"
#include "storage/StorageLease.h"
#include "transport/LoRaInterface.h"
#include "util/PerfTrace.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <string_view>
#include <cerrno>

namespace {
constexpr size_t ContactFileLimit = 32768; // Existing FlashStore/SDStore read limit.

// One bounded read buffer, shared by all scalar parses in a pass. ArduinoJson
// decodes strings/escapes; only the surrounding name-map punctuation is walked.
class BufferedInput {
public:
    explicit BufferedInput(File& file) : _file(file), _length(file.size()) {}
    int peek() {
        if (_offset == _count) {
            if (_loaded == _length) return -1;
            _count = _file.read(_bytes, std::min(sizeof(_bytes), _length - _loaded));
            _offset = 0; _loaded += _count;
            if (!_count) { _failed = true; return -1; }
        }
        return _bytes[_offset];
    }
    int read() { const int value = peek(); if (value >= 0) ++_offset; return value; }
    bool failed() const { return _failed; }
    int nonSpace() {
        int c = peek();
        while (c == ' ' || c == '\t' || c == '\r' || c == '\n') { read(); c = peek(); }
        return c;
    }
private:
    File& _file;
    size_t _length, _loaded = 0, _offset = 0, _count = 0;
    uint8_t _bytes[512];
    bool _failed = false;
};

bool hexText(const char* text, size_t size) {
    for (size_t i = 0; i < size; ++i)
        if (!((text[i] >= '0' && text[i] <= '9') ||
              (text[i] >= 'a' && text[i] <= 'f') || (text[i] >= 'A' && text[i] <= 'F'))) return false;
    return true;
}

bool contactFilename(String& name) {
    if (name.endsWith(".bak")) name = name.substring(0, name.length() - 4);
    return name.length() == 21 && name.endsWith(".json") && hexText(name.c_str(), 16);
}

struct ContactFilter {
    bool root = true, selected = true;
    bool allow() const { return selected; }
    bool allowArray() const { return false; }
    bool allowObject() const { return root; }
    bool allowValue() const { return selected; }
    ContactFilter operator[](JsonString key) const {
        return {false, root && (key == "hash" || key == "name" || key == "deleted")};
    }
    template<class Index> ContactFilter operator[](Index) const { return {false, false}; }
};

struct ContactInput {
    handheld::storage::JsonAllocator allocator{handheld::storage::Budget::JsonAllocator};
    JsonDocument document{&allocator};
    File file;

    template<class Store> bool open(Store& store, const char* root, const String& name) {
        const String path = String(root) + "/" + name;
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            const String candidate = attempt ? String(path + ".bak") : path;
            file = store.openFile(candidate.c_str());
            if (!file || file.isDirectory() || !file.size() || file.size() > ContactFileLimit) {
                file.close(); continue;
            }
            BufferedInput input(file);
            const auto error = deserializeJson(document, input, ContactFilter{});
            if (!error && input.nonSpace() < 0 && !input.failed() && document.is<JsonObject>()) {
                const JsonString hash = document["hash"].as<JsonString>();
                if (hash.size() == 32 && hexText(hash.c_str(), 32) &&
                    memcmp(hash.c_str(), name.c_str(), 16) == 0) return true;
            }
            file.close(); document.clear();
        }
        return false;
    }
    ~ContactInput() { file.close(); }
};

// The source is rewound for the existing atomic write and readback comparison.
// No complete input String or second JSON serialization is retained.
class ContactSource final : public handheld::storage::AtomicSource {
public:
    explicit ContactSource(File& file) : _file(file), _length(file.size()) {}
    size_t length() const override { return _length; }
    bool emit(handheld::storage::ByteSink& sink) const override {
        if (!_file.seek(0)) return false;
        uint8_t bytes[512];
        for (size_t offset = 0; offset < _length;) {
            const size_t count = std::min(sizeof(bytes), _length - offset);
            if (_file.read(bytes, count) != count || sink.write(bytes, count) != count) return false;
            offset += count;
        }
        return true;
    }
private:
    File& _file;
    size_t _length;
};

template<class Visit> bool readNames(File& file, Visit visit) {
    if (!file || file.isDirectory() || !file.size() || file.size() > ContactFileLimit || !file.seek(0)) return false;
    BufferedInput input(file);
    handheld::storage::JsonAllocator allocator(handheld::storage::Budget::JsonAllocator);
    JsonDocument scalar(&allocator);
    if (input.nonSpace() != '{') return false;
    input.read();
    if (input.nonSpace() != '}') {
        for (;;) {
            if (input.nonSpace() != '"' || deserializeJson(scalar, input)) return false;
            const JsonString key = scalar.as<JsonString>();
            if (key.size() != 32 || !hexText(key.c_str(), key.size())) return false;
            char hash[33]; memcpy(hash, key.c_str(), 32); hash[32] = 0;
            if (input.nonSpace() != ':') return false;
            input.read();
            if (input.nonSpace() != '"' || deserializeJson(scalar, input)) return false;
            const JsonString value = scalar.as<JsonString>();
            visit(hash, value);
            const int separator = input.nonSpace();
            if (separator == '}') break;
            if (separator != ',') return false;
            input.read();
        }
    }
    input.read();
    return input.nonSpace() < 0 && !input.failed();
}

class NameCacheSource final : public handheld::storage::AtomicSource {
public:
    explicit NameCacheSource(const std::map<std::string, std::string>& names) : _names(names) {
        struct Counter final : handheld::storage::ByteSink {
            size_t count = 0;
            size_t write(const uint8_t*, size_t size) override { count += size; return size; }
        } counter;
        if (emit(counter)) _length = counter.count;
    }
    size_t length() const override { return _length; }
    bool emit(handheld::storage::ByteSink& sink) const override {
        handheld::storage::JsonAllocator allocator(handheld::storage::Budget::JsonAllocator);
        JsonDocument scalar(&allocator);
        if (sink.write('{') != 1) return false;
        bool first = true;
        for (const auto& entry : _names) {
            if (!first && sink.write(',') != 1) return false;
            first = false;
            if (!scalar.set(entry.first) || serializeJson(scalar, sink) != measureJson(scalar) ||
                sink.write(':') != 1) return false;
            scalar.clear();
            if (!scalar.set(entry.second) || serializeJson(scalar, sink) != measureJson(scalar)) return false;
            scalar.clear();
        }
        return sink.write('}') == 1;
    }
private:
    const std::map<std::string, std::string>& _names;
    size_t _length = 0;
};
} // namespace

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
static std::string sanitizeName(std::string_view raw, size_t maxLen = 32) {
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

AnnounceManager::~AnnounceManager() { closeContactMirrorDirectory(); }

void AnnounceManager::setStorage(SDStore* sd, FlashStore* flash) {
    handheld::assertDeviceOwner();
    closeContactMirrorDirectory();
    _contactMirrorRetry = _contactMirrorWaiting = false;
    _sd = sd; _flash = flash;
}

void AnnounceManager::cacheName(const std::string& hash, const std::string& name) {
    auto found = _nameCache.find(hash);
    if (found != _nameCache.end()) { found->second = name; return; }
    // Preserve live/saved names before historical cache-only names, matching
    // the existing announce eviction policy. The node limit is below this cap.
    if (_nameCache.size() >= MAX_NAME_CACHE) {
        auto victim = std::find_if(_nameCache.begin(), _nameCache.end(), [&](const auto& entry) {
            return !findNodeByHex(entry.first);
        });
        if (victim == _nameCache.end()) return;
        _nameCache.erase(victim);
    }
    _nameCache.emplace(hash, name);
}

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
        // A saved unresolved hash is still a placeholder, not a contact alias.
        // Let its first announced name reach both Peers and Direct lookup.
        const bool unnamed = node.name.empty() || node.name == destHex.substr(0, 12);
        if (!name.empty() && (unnamed || !node.saved || !HAS_CONTACT_RENAME)) node.name = name;
        node.identityHex = idHex;
        node.lastSeen = now;
        node.hops = hops;
        if (rssi != 0) node.rssi = rssi;
        if (snr != 0) node.snr = snr;
        if (node.saved) _contactsDirty = true;
        if (!name.empty()) {
            auto nc = _nameCache.find(destHex);
            if (nc == _nameCache.end() || nc->second != name) {
                cacheName(destHex, name);
                _nameCacheWrites.changed();
                saveNameCache();
            }
        }
        return;
    }

    if (!name.empty()) {
        auto nc = _nameCache.find(destHex);
        if (nc == _nameCache.end() || nc->second != name) {
            cacheName(destHex, name);
            _nameCacheWrites.changed();
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
    flushContactMirrors();
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
            cacheName(hexHash, node.name);
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
    closeContactMirrorDirectory();
    _contactMirrorsPending = _contactMirrorRetry = _contactMirrorWaiting = false;
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
    handheld::storage::StorageLease lease;
    if (!lease.held()) return false;
    const std::string filename = hexHash.substr(0, 16) + ".json";
    closeContactMirrorDirectory();
    if (!_flash || !_flash->ensureDir(PATH_CONTACTS) ||
        !_flash->writeString((String(PATH_CONTACTS) + "/" + filename.c_str()).c_str(), json)) return false;
    // An incomplete sweep starts afresh after canonical directory mutation.
    _contactMirrorRetry = _contactMirrorWaiting = false;
    if (!mirrorContact(filename.c_str())) _contactMirrorsPending = true;
    return true;
}

bool AnnounceManager::mirrorContact(const char* filename) {
    if (!_flash || !_sd || !_sd->isReady() || !_sd->ensureDir(SD_PATH_CONTACTS)) return false;
    ContactInput input;
    if (!input.open(*_flash, PATH_CONTACTS, String(filename))) return false;
    input.document.clear();
    const ContactSource source(input.file);
    return _sd->writeAtomic((String(SD_PATH_CONTACTS) + "/" + filename).c_str(), source) ==
        handheld::storage::Error::None;
}

void AnnounceManager::closeContactMirrorDirectory() {
    if (!_contactMirrorDirectory) return;
    FSLock lock;
    if (::closedir(_contactMirrorDirectory) != 0) _contactMirrorRetry = true;
    _contactMirrorDirectory = nullptr;
}

void AnnounceManager::flushContactMirrors() {
    if (!_contactMirrorsPending) return;
    handheld::storage::StorageLease lease;
    if (!lease.held()) return; // Owner never waits behind an active writer.
    if (!_flash || !_sd || !_sd->isReady()) { closeContactMirrorDirectory(); return; }
    const unsigned long now = millis();
    if (_contactMirrorWaiting && now - _lastContactMirrorAttempt < CONTACT_SAVE_INTERVAL_MS) return;
    _contactMirrorWaiting = false;
    if (!_contactMirrorDirectory) {
        FSLock lock;
        // Same pinned LittleFS VFS boundary used by ResetDirectory: Arduino's
        // openNextFile allocates a path and can conceal failure as EOF.
        _contactMirrorDirectory = ::opendir("/littlefs" PATH_CONTACTS);
        if (!_contactMirrorDirectory) {
            _contactMirrorWaiting = true; _lastContactMirrorAttempt = now; return;
        }
    }
    // Exactly one readdir per poll, including duplicates/invalid names. The
    // next name is copied before the next SDK call; no SD handle is retained.
    char filename[26] = {};
    bool ended = false, failed = false;
    {
        FSLock lock;
        errno = 0;
        const dirent* entry = ::readdir(_contactMirrorDirectory);
        if (!entry) { ended = true; failed = errno != 0; }
        else if (entry->d_type == DT_REG) {
            const size_t length = strnlen(entry->d_name, sizeof(filename));
            if (length < sizeof(filename)) memcpy(filename, entry->d_name, length + 1);
        }
    }
    if (ended) {
        closeContactMirrorDirectory();
        _contactMirrorsPending = _contactMirrorRetry || failed;
        _contactMirrorRetry = false;
        _contactMirrorWaiting = _contactMirrorsPending;
        _lastContactMirrorAttempt = now;
        return;
    }
    String name(filename); const bool backup = name.endsWith(".bak");
    if (!contactFilename(name)) return;
    if (backup && _flash->exists((String(PATH_CONTACTS) + "/" + name).c_str())) return;
    if (!mirrorContact(name.c_str())) _contactMirrorRetry = true;
}

bool AnnounceManager::saveContact(const DiscoveredNode& node) {
    handheld::assertDeviceOwner();
    JsonDocument doc;
    doc["hash"] = node.hash.toHex(); doc["name"] = node.name;
    String json;
    if (doc.overflowed() || serializeJson(doc, json) != measureJson(doc)) return false;
    doc.clear(); // Release the JSON pool before the bounded mirror parser.
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
    doc.clear();
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
        cacheName(hexHash, node.name);
        _nameCacheWrites.changed();
    }
    return true;
}

void AnnounceManager::loadContacts() {
    handheld::assertDeviceOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return;
    closeContactMirrorDirectory();
    auto loadFrom = [&](auto& store, const char* root, bool canonical) {
        File dir = store.openDir(root);
        if (!dir || !dir.isDirectory()) return;
        for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
            if (entry.isDirectory()) { entry.close(); continue; }
            String name = entry.name(); const bool backup = name.endsWith(".bak");
            entry.close();
            if (!contactFilename(name)) continue;
            const String path = String(root) + "/" + name;
            // A primary processes its own rollback candidate exactly once.
            // Canonical presence vetoes stale SD data even if its JSON is bad.
            if (backup && store.exists(path.c_str())) continue;
            if (!canonical && _flash &&
                (_flash->exists((String(PATH_CONTACTS) + "/" + name).c_str()) ||
                 _flash->exists((String(PATH_CONTACTS) + "/" + name + ".bak").c_str()))) continue;
            ContactInput input;
            if (!input.open(store, root, name)) continue;
            const std::string hexHash = input.document["hash"].as<std::string>();
            const bool deleted = input.document["deleted"] | false;
            const std::string safeName = sanitizeName(input.document["name"] | "");
            input.document.clear();
            if (!canonical) {
                const ContactSource source(input.file);
                if (!_flash || !_flash->ensureDir(PATH_CONTACTS) ||
                    _flash->writeAtomic((String(PATH_CONTACTS) + "/" + name).c_str(), source) !=
                        handheld::storage::Error::None) continue;
            }
            _contactMirrorsPending = true;
            if (deleted) continue;
            rs::Bytes hash; hash.assignHex(hexHash.c_str());
            const std::string key = makeKey(hash);
            if (_hashIndex.count(key) || _nodes.size() >= MAX_NODES) continue;
            DiscoveredNode node;
            node.hash = hash;
            node.name = safeName.empty() ? hexHash.substr(0, 12) : safeName;
            node.saved = true;
            _hashIndex[key] = (int)_nodes.size();
            _nodes.push_back(node);
        }
        dir.close();
    };
    if (_flash) loadFrom(*_flash, PATH_CONTACTS, true);
    if (_sd && _sd->isReady()) loadFrom(*_sd, SD_PATH_CONTACTS, false);
    // Startup traversal is synchronous; optional mirrors progress one entry at
    // a time in loop(), rather than a second full scan before boot can finish.
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
    // Final maintenance flush must not retain a directory across media teardown.
    closeContactMirrorDirectory();
    _contactMirrorRetry = _contactMirrorWaiting = false;
    const bool contacts = !_contactsDirty || saveContacts();
    const bool names = !_nameCacheWrites.dirty() || saveNameCache();
    closeContactMirrorDirectory();
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
    handheld::storage::StorageLease lease;
    if (!lease.held()) return false;
    _nameCacheWrites.require((_flash ? MirrorWriteState::Flash : 0) |
                            (_sd && _sd->isReady() ? MirrorWriteState::SD : 0));
    if (ESP.getFreeHeap() < 20000) return false;
    const NameCacheSource source(_nameCache);
    if (!source.length()) return false;
    if (_nameCacheWrites.needs(MirrorWriteState::Flash) && _flash)
        _nameCacheWrites.completed(MirrorWriteState::Flash,
            _flash->writeAtomic("/config/names.json", source) == handheld::storage::Error::None);
    if (_nameCacheWrites.needs(MirrorWriteState::Flash)) return false;
    if (_nameCacheWrites.needs(MirrorWriteState::SD) && _sd && _sd->isReady())
        _nameCacheWrites.completed(MirrorWriteState::SD,
            _sd->writeAtomic(SD_PATH_CONFIG_DIR "/names.json", source) == handheld::storage::Error::None);
    const bool ok = _nameCacheWrites.settle();
    if (!ok) Serial.println("[ANNOUNCE] Name cache persistence incomplete; retry pending");
    return ok;
}

void AnnounceManager::loadNameCache() {
    handheld::assertDeviceOwner();
    handheld::storage::StorageLease lease;
    if (!lease.held()) return;
    File file;
    if (_flash) {
        file = _flash->openFile("/config/names.json");
        if (!file) file = _flash->openFile("/config/names.json.bak");
    }
    if ((!file || !file.size()) && _sd && _sd->isReady()) {
        file.close(); file = _sd->openFile(SD_PATH_CONFIG_DIR "/names.json");
        if (!file) file = _sd->openFile(SD_PATH_CONFIG_DIR "/names.json.bak");
    }
    // Validate the entire map before applying anything. A second linear pass
    // decodes only one key/value at a time; there is no full-file String/DOM or
    // unbounded temporary map. Duplicate keys retain JSON's last-value policy.
    if (!readNames(file, [](const char*, JsonString) {})) { file.close(); return; }
    const bool complete = readNames(file, [&](const char* hash, JsonString value) {
        cacheName(hash, sanitizeName(std::string_view(value.c_str(), value.size())));
    });
    file.close();
    if (!complete) return; // Never persist a partial read after media failure.
    _nameCacheWrites.changed(); // Repair an older SD mirror from canonical flash.
    Serial.printf("[ANNOUNCE] Name cache loaded (%d entries)\n", (int)_nameCache.size());
}
