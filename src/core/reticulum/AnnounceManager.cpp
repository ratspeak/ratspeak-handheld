// Direct port from Ratputer — node discovery and contact persistence
#include "AnnounceManager.h"
#include "runtime/TaskOwner.h"
#include "config/Config.h"
#include "config/ConfigMemory.h"
#include <new>
#include <type_traits>
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
static_assert(std::is_nothrow_move_constructible<DiscoveredNode>::value &&
              std::is_nothrow_move_assignable<DiscoveredNode>::value,
              "Peer publication must not allocate after persistence or eviction");
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
    explicit NameCacheSource(const std::map<std::string, std::string, std::less<>>& names) : _names(names) {
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
    const std::map<std::string, std::string, std::less<>>& _names;
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
// The result views the caller's announce bytes; nothing is copied.
static std::string_view extractMsgPackName(const uint8_t* data, size_t len) {
    if (len < 2) return {};
    uint8_t b = data[0];
    size_t pos = 0;
    size_t arrLen = 0;
    if ((b & 0xF0) == 0x90) { arrLen = b & 0x0F; if (arrLen == 0) return ""; pos = 1; }
    else if (b == 0xDC && len >= 3) { arrLen = ((size_t)data[1] << 8) | data[2]; pos = 3; }
    else return {};

    // Pass 1: scan for first non-empty STR element
    size_t savedPos = pos;
    for (size_t i = 0; i < arrLen && pos < len; i++) {
        b = data[pos];
        size_t slen = 0;
        if ((b & 0xE0) == 0xA0) { slen = b & 0x1F; pos++; }
        else if (b == 0xD9 && pos + 1 < len) { slen = data[pos + 1]; pos += 2; }
        else if (b == 0xDA && pos + 2 < len) { slen = ((size_t)data[pos + 1] << 8) | data[pos + 2]; pos += 3; }
        else { pos = mpSkipValue(data, len, pos); continue; }
        if (slen > 0 && pos + slen <= len) return {(const char*)&data[pos], slen};
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
        if (slen > 0 && pos + slen <= len) return {(const char*)&data[pos], slen};
        pos += slen;
    }
    return {};
}

// Character filter — safe displayable characters including UTF-8 multibyte
// (lvgl renders missing glyphs as fallback boxes — cosmetic, accepted).
// Up to 32 bytes plus one final multibyte sequence; the view borrows `clean`.
constexpr size_t NameBytes = 36;
static std::string_view sanitizeName(std::string_view raw, char (&clean)[NameBytes], size_t maxLen = 32) {
    size_t used = 0;
    const uint8_t* p = (const uint8_t*)raw.data();
    size_t sz = raw.size();
    for (size_t i = 0; i < sz && used < maxLen; ) {
        uint8_t c = p[i];
        size_t width = 0;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == ' ' || c == '-' || c == '_' || c == '.' || c == '\'' || c == '/') width = 1;
        // UTF-8 multibyte: pass through valid sequences (emoji/accents)
        else if ((c & 0xE0) == 0xC0 && i + 1 < sz && (p[i+1] & 0xC0) == 0x80) width = 2;
        else if ((c & 0xF0) == 0xE0 && i + 2 < sz &&
                 (p[i+1] & 0xC0) == 0x80 && (p[i+2] & 0xC0) == 0x80) width = 3;
        else if ((c & 0xF8) == 0xF0 && i + 3 < sz &&
                 (p[i+1] & 0xC0) == 0x80 && (p[i+2] & 0xC0) == 0x80 && (p[i+3] & 0xC0) == 0x80) width = 4;
        // Strip control chars, invalid bytes
        if (!width) { i++; continue; }
        memcpy(clean + used, p + i, width); used += width; i += width;
    }
    const std::string_view view(clean, used);
    const size_t start = view.find_first_not_of(' ');
    if (start == std::string_view::npos) return {};
    return view.substr(start, view.find_last_not_of(' ') - start + 1);
}

static std::string sanitizeName(std::string_view raw) {
    char clean[NameBytes];
    return std::string(sanitizeName(raw, clean));
}

// received_announce parity: MsgPack name, else bare UTF-8 app data (Rust raw announces).
static std::string_view announcedName(const uint8_t* appData, size_t appLen, char (&clean)[NameBytes]) {
    if (!appData || !appLen) return {};
    std::string_view raw = extractMsgPackName(appData, appLen);
    if (raw.empty()) {
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
        if (isText) raw = {(const char*)appData, appLen};
    }
    return sanitizeName(raw, clean);
}

static void hexOf(const uint8_t bytes[16], char (&out)[33]) {
    static constexpr char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 16; ++i) { out[2*i] = digits[bytes[i] >> 4]; out[2*i+1] = digits[bytes[i] & 15]; }
    out[32] = 0;
}

AnnounceManager::AnnounceManager(const char* aspectFilter) {
    (void)aspectFilter;  // aspect filtering moved into the FFI announce bridge
    _nodes.reserve(MAX_NODES);
}

AnnounceManager::~AnnounceManager() { closeContactMirrorDirectory(); }

void AnnounceManager::setStorage(SDStore* sd, FlashStore* flash) {
    handheld::assertDeviceOwner();
    closeContactMirrorDirectory();
    _contactMirrorRetry = _contactMirrorWaiting = false;
    _sd = sd; _flash = flash;
}

bool AnnounceManager::cacheName(std::string_view hash, std::string_view name) {
    auto found = _nameCache.find(hash);
    if (found != _nameCache.end() && found->second == name) return true;
    if (!handheld::config::Memory::admits(0)) return false;
    try {
        if (found != _nameCache.end()) {
            std::string prepared(name);
            if (!handheld::config::Memory::admits(0)) return false;
            found->second.swap(prepared);
            return true;
        }
        // Stage before evicting so allocation refusal cannot destroy the old
        // cached name. Insertion leaves existing map iterators valid.
        auto victim = _nameCache.end();
        if (_nameCache.size() >= MAX_NAME_CACHE) {
            victim = std::find_if(_nameCache.begin(), _nameCache.end(), [&](const auto& entry) {
                return !findNodeByHex(entry.first);
            });
            if (victim == _nameCache.end()) return false;
        }
        auto inserted = _nameCache.emplace(std::string(hash), std::string(name));
        if (!handheld::config::Memory::admits(0)) {
            _nameCache.erase(inserted.first); return false;
        }
        if (victim != _nameCache.end()) _nameCache.erase(victim);
        return true;
    } catch (const std::bad_alloc&) { return false; }
}

int AnnounceManager::findNodeIndex(const uint8_t* hash, size_t length) const {
    for (size_t i = 0; i < _nodes.size(); ++i) {
        const auto& candidate = _nodes[i].hash;
        if (candidate.size() == length && (!length || !memcmp(candidate.data(), hash, length)))
            return int(i);
    }
    return -1;
}

int AnnounceManager::findNodeIndexHex(const std::string& hash) const {
    // Preserve Bytes::assignHex's complete-pair parsing, including uppercase,
    // a trailing odd nibble and stopping at the first non-hex pair. Live
    // destination hashes are exactly 16 bytes; no temporary vector is needed.
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    uint8_t bytes[16]; size_t count = 0;
    const char* text = hash.c_str();
    while (text[0] && text[1]) {
        const int high = nibble(text[0]), low = nibble(text[1]);
        if (high < 0 || low < 0) break;
        if (count == sizeof bytes) return -1;
        bytes[count++] = uint8_t((high << 4) | low);
        text += 2;
    }
    return count == sizeof bytes ? findNodeIndex(bytes, count) : -1;
}

void AnnounceManager::receivedAnnounceEvent(const uint8_t destHash[16], const uint8_t identityHash[16],
                                            const uint8_t* appData, size_t appLen, int rssi,
                                            float snr, uint8_t hops) {
    handheld::assertDeviceOwner();
    if (_localDestHash.size() == 16 && !memcmp(destHash, _localDestHash.data(), 16)) return;
    const unsigned long now = millis();
    if (now - _globalAnnounceWindowStart >= 1000) {
        _globalAnnounceWindowStart = now; _globalAnnounceCount = 0;
    }
    if (++_globalAnnounceCount > MAX_GLOBAL_ANNOUNCES_PER_SEC) return;
    if (ESP.getFreeHeap() < 25000) evictStale(300000);
    const int existing = findNodeIndex(destHash, 16);
    if (existing >= 0) {
        auto& node = _nodes[size_t(existing)];
        if (node.lastSeen && now >= node.lastSeen && now - node.lastSeen < ANNOUNCE_MIN_INTERVAL_MS) return;
        // Liveness and RF data never depend on allocating another display name.
        node.lastSeen = now; node.hops = hops;
        if (rssi) node.rssi = rssi;
        if (snr) node.snr = snr;
    }
    // Background display/cache growth must leave the same reserve as Settings.
    // The protocol already owns the authenticated announce independently.
    if (!handheld::config::Memory::admits(0)) return;
    // Name and hex keys are staged on the stack. Refreshing a known peer with
    // unchanged metadata allocates nothing, so retained blocks never migrate.
    char clean[NameBytes], destHex[33], idHex[33];
    const std::string_view name = announcedName(appData, appLen, clean);
    hexOf(destHash, destHex); hexOf(identityHash, idHex);
    const std::string_view destination(destHex, 32), identity(idHex, 32);
    try {
        // Optional names persist from loop(), coalescing bursts into one write.
        const auto rememberName = [&] {
            if (name.empty()) return;
            const auto found = _nameCache.find(destination);
            if ((found == _nameCache.end() || found->second != name) && cacheName(destination, name))
                _nameCacheWrites.changed();
        };
        if (existing >= 0) {
            auto& node = _nodes[size_t(existing)];
            const bool unnamed = node.name.empty() ||
                (node.name.size() == 12 && !memcmp(node.name.data(), destHex, 12));
            rememberName();
            const bool rename = !name.empty() && (unnamed || !node.saved || !HAS_CONTACT_RENAME) && node.name != name;
            const bool rebind = node.identityHex != identity;
            if (!rename && !rebind) return;
            // Name and identity remain one prepared metadata publication.
            std::string nextName(rename ? name : std::string_view()), nextIdentity(rebind ? identity : std::string_view());
            if (!handheld::config::Memory::admits(0)) return;
            if (rename) node.name.swap(nextName);
            if (rebind) node.identityHex.swap(nextIdentity);
            if (rename && node.saved) _contactsDirty = true; // Contact records hold only hash and name.
            return;
        }

        // Prepare the entire replacement before evicting any live peer.
        DiscoveredNode node;
        node.hash = rs::Bytes(destHash, 16);
        if (!name.empty()) node.name.assign(name.data(), name.size());
        else {
            const auto cached = _nameCache.find(destination);
            if (cached != _nameCache.end() && !cached->second.empty()) node.name = cached->second;
            else node.name.assign(destHex, 12);
        }
        node.identityHex.assign(identity.data(), identity.size());
        node.lastSeen = now; node.hops = hops; node.rssi = rssi; node.snr = snr;
        if (!handheld::config::Memory::admits(0)) return;
        rememberName();
        if (_nodes.size() >= MAX_NODES) {
            evictStale();
            if (_nodes.size() >= MAX_NODES) {
                uint8_t maxHops = 0; unsigned long oldest = ULONG_MAX; int victim = -1;
                for (size_t i = 0; i < _nodes.size(); ++i) {
                    if (_nodes[i].saved) continue;
                    if (_nodes[i].hops > maxHops || (_nodes[i].hops == maxHops && _nodes[i].lastSeen < oldest)) {
                        maxHops = _nodes[i].hops; oldest = _nodes[i].lastSeen; victim = int(i);
                    }
                }
                if (victim < 0) return;
                if (size_t(victim) != _nodes.size() - 1) std::swap(_nodes[size_t(victim)], _nodes.back());
                _nodes.pop_back();
            }
        }
        // Constructor reserves the bounded table. A move adds no heap allocation.
        _nodes.push_back(std::move(node));
        Serial.printf("[ANNOUNCE-RUST] new peer %s name=\"%s\" hops=%u\n", destHex, _nodes.back().name.c_str(), unsigned(hops));
    } catch (const std::bad_alloc&) {
        // A later announce may refresh metadata; existing peers remain usable.
    }
}

void AnnounceManager::loop() {
    handheld::assertDeviceOwner();
    try { flushContactMirrors(); }
    catch (const std::bad_alloc&) {
        closeContactMirrorDirectory();
        _contactMirrorsPending = _contactMirrorWaiting = true;
        _lastContactMirrorAttempt = millis();
    }
    unsigned long now = millis();
    if (_contactsDirty && now - _lastContactSave >= CONTACT_SAVE_INTERVAL_MS) {
        _lastContactSave = now;
        saveContacts();
    }
    // Optional names save here, at most once per interval; a burst of learned
    // names becomes one write. Pre-advance the stamp so a low-heap deferral
    // retries at cadence, not every pass.
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
    const int index = findNodeIndex(hash.data(), hash.size());
    return index >= 0 ? &_nodes[size_t(index)] : nullptr;
}

const DiscoveredNode* AnnounceManager::findNodeByHex(const std::string& hexHash) const {
    handheld::assertDeviceOwner();
    const int index = findNodeIndexHex(hexHash);
    if (index >= 0) return &_nodes[size_t(index)];
    // Legacy conversation prefixes are compared against lowercase hex. Keep
    // this case-sensitive fallback without generating a String for every node.
    static constexpr char digits[] = "0123456789abcdef";
    for (const auto& node : _nodes) {
        if (hexHash.size() >= node.hash.size() * 2) continue;
        bool matches = true;
        for (size_t i = 0; i < hexHash.size(); ++i) {
            const uint8_t byte = node.hash.data()[i / 2];
            if (hexHash[i] != digits[i % 2 ? byte & 15 : byte >> 4]) { matches = false; break; }
        }
        if (matches) return &node;
    }
    return nullptr;
}

bool AnnounceManager::addManualContact(const std::string& hexHash, const std::string& name) {
    try {
        handheld::assertDeviceOwner();
        rs::Bytes hash;
        hash.assignHex(hexHash.c_str());
        if (hash.size() != 16) return false;
        std::string safeName = sanitizeName(name);

        const int existing = findNodeIndex(hash.data(), hash.size());
        if (existing >= 0) {
            auto& node = _nodes[size_t(existing)];
            auto candidate = node;
            if (!safeName.empty()) candidate.name = safeName;
            candidate.saved = true;
            if (!handheld::config::Memory::admits(0) || !saveContact(candidate)) return false;
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
        if (_nodes.size() >= MAX_NODES || !handheld::config::Memory::admits(0)) return false;
        if (!saveContact(node)) return false;
        _nodes.push_back(std::move(node));
        return true;
    } catch (const std::bad_alloc&) { return false; }
}

bool AnnounceManager::saveNode(const std::string& hexHash) {
    try {
        handheld::assertDeviceOwner();
        const int index = findNodeIndexHex(hexHash);
        if (index >= 0) {
            auto& node = _nodes[size_t(index)];
            if (!saveContact(node)) return false;
            node.saved = true;
            // Always persist contact name to name cache
            if (!node.name.empty()) {
                if (cacheName(hexHash, node.name)) _nameCacheWrites.changed();
            }
            return true;
        }
        return false;
    } catch (const std::bad_alloc&) { return false; }
}

void AnnounceManager::unsaveNode(const std::string& hexHash) {
    handheld::assertDeviceOwner();
    const int index = findNodeIndexHex(hexHash);
    if (index >= 0) {
        if (!removeContact(hexHash)) return;
        _nodes[size_t(index)].saved = false;
        Serial.printf("[ANNOUNCE] Removed contact: %s\n", _nodes[size_t(index)].name.c_str());
    }
}

void AnnounceManager::evictStale(unsigned long maxAgeMs) {
    handheld::assertDeviceOwner();
    unsigned long now = millis();
    _nodes.erase(std::remove_if(_nodes.begin(), _nodes.end(),
        [now, maxAgeMs](const DiscoveredNode& n) {
            return !n.saved && (n.lastSeen == 0 || now < n.lastSeen || now - n.lastSeen > maxAgeMs);
        }), _nodes.end());
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
}

void AnnounceManager::clearAll() {
    handheld::assertDeviceOwner();
    _nodes.clear();
    _nameCache.clear();
    _contactsDirty = false;
    closeContactMirrorDirectory();
    _contactMirrorsPending = _contactMirrorRetry = _contactMirrorWaiting = false;
    _nameCacheWrites.reset();
    _nameCachePersistenceBlocked = false;
    _lastNameCacheSave = 0;
    Serial.println("[ANNOUNCE] Cleared all nodes and name cache");
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
    try {
        if (!mirrorContact(filename.c_str())) _contactMirrorsPending = true;
    } catch (const std::bad_alloc&) {
        // Canonical persistence already succeeded; optional mirroring retries.
        _contactMirrorsPending = true;
    }
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
    try {
        handheld::assertDeviceOwner();
        JsonDocument doc;
        doc["hash"] = node.hash.toHex(); doc["name"] = node.name;
        String json;
        if (doc.overflowed() || serializeJson(doc, json) != measureJson(doc)) return false;
        doc.clear(); // Release the JSON pool before the bounded mirror parser.
        return commitContact(node.hash.toHex(), json);
    } catch (const std::bad_alloc&) { return false; }
}

bool AnnounceManager::removeContact(const std::string& hexHash) {
    try {
        handheld::assertDeviceOwner();
        // Keep a durable deletion marker so an old/removable SD backup cannot
        // resurrect a contact. Saving this contact again replaces the marker.
        JsonDocument doc;
        doc["hash"] = hexHash; doc["deleted"] = true;
        String json;
        if (doc.overflowed() || serializeJson(doc, json) != measureJson(doc)) return false;
        doc.clear();
        return commitContact(hexHash, json);
    } catch (const std::bad_alloc&) { return false; }
}

bool AnnounceManager::deleteContact(int nodeIdx) {
    try {
        handheld::assertDeviceOwner();
        if (nodeIdx < 0 || nodeIdx >= (int)_nodes.size()) return false;
        std::string hexHash = _nodes[nodeIdx].hash.toHex();
        if (!removeContact(hexHash)) return false;
        _nameCache.erase(hexHash);
        _nameCacheWrites.changed();
        _nodes.erase(_nodes.begin() + nodeIdx);
        Serial.printf("[ANNOUNCE] Deleted contact %.12s\n", hexHash.c_str());
        return true;
    } catch (const std::bad_alloc&) { return false; }
}

bool AnnounceManager::deleteContactByHex(const std::string& hexHash) {
    handheld::assertDeviceOwner();
    const int index = findNodeIndexHex(hexHash);
    return index >= 0 && deleteContact(index);
}

bool AnnounceManager::setContactName(const std::string& hexHash, const std::string& name) {
    try {
        handheld::assertDeviceOwner();
        const int index = findNodeIndexHex(hexHash);
        if (index < 0) return false;
        auto& node = _nodes[size_t(index)];
        std::string safeName = sanitizeName(name);
        auto candidate = node;
        if (!safeName.empty()) candidate.name = safeName;
        candidate.saved = true;
        if (!handheld::config::Memory::admits(0) || !saveContact(candidate)) return false;
        node = std::move(candidate);
        if (!node.name.empty()) {
            if (cacheName(hexHash, node.name)) _nameCacheWrites.changed();
        }
        return true;
    } catch (const std::bad_alloc&) { return false; }
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
            if (findNode(hash) || _nodes.size() >= MAX_NODES) continue;
            DiscoveredNode node;
            node.hash = hash;
            node.name = safeName.empty() ? hexHash.substr(0, 12) : safeName;
            node.saved = true;
            _nodes.push_back(std::move(node));
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
    // A cache restored only partially is display-only for this boot. Preserve
    // its durable source without making optional names block maintenance.
    const bool names = _nameCachePersistenceBlocked || !_nameCacheWrites.dirty() || saveNameCache();
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
    try {
        handheld::assertDeviceOwner();
        _lastNameCacheSave = millis();
        if (_nameCachePersistenceBlocked) return false;
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
    } catch (const std::bad_alloc&) { return false; }
}

void AnnounceManager::loadNameCache() {
    try {
        handheld::assertDeviceOwner();
        handheld::storage::StorageLease lease;
        if (!lease.held()) { _nameCachePersistenceBlocked = true; return; }
        File file;
        if (_flash) {
            if (!_flash->isReady()) { _nameCachePersistenceBlocked = true; return; }
            const bool present = _flash->exists("/config/names.json") || _flash->exists("/config/names.json.bak");
            file = _flash->openFile("/config/names.json");
            if (!file) file = _flash->openFile("/config/names.json.bak");
            if (!file && present) { _nameCachePersistenceBlocked = true; return; }
        }
        if ((!file || !file.size()) && _sd && _sd->isReady()) {
            const bool present = _sd->exists(SD_PATH_CONFIG_DIR "/names.json") || _sd->exists(SD_PATH_CONFIG_DIR "/names.json.bak");
            file.close(); file = _sd->openFile(SD_PATH_CONFIG_DIR "/names.json");
            if (!file) file = _sd->openFile(SD_PATH_CONFIG_DIR "/names.json.bak");
            if (!file && present) { _nameCachePersistenceBlocked = true; return; }
        }
        // Validate the entire map before applying anything. A second linear pass
        // decodes only one key/value at a time; there is no full-file String/DOM or
        // unbounded temporary map. Duplicate keys retain JSON's last-value policy.
        if (!file) return;
        if (!readNames(file, [](const char*, JsonString) {})) {
            _nameCachePersistenceBlocked = true; file.close(); return;
        }
        bool admitted = true;
        const bool complete = readNames(file, [&](const char* hash, JsonString value) {
            if (admitted) admitted = cacheName(hash, sanitizeName(std::string_view(value.c_str(), value.size())));
        });
        file.close();
        // A partial cache remains useful for display, but cannot replace its full
        // durable source. A subsequent successful reload or explicit clear resets it.
        _nameCachePersistenceBlocked = !complete || !admitted;
        if (_nameCachePersistenceBlocked) return;
        _nameCacheWrites.changed(); // Repair an older SD mirror from canonical flash.
        Serial.printf("[ANNOUNCE] Name cache loaded (%d entries)\n", (int)_nameCache.size());
    } catch (const std::bad_alloc&) { _nameCachePersistenceBlocked = true; }
}
