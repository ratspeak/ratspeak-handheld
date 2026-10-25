#pragma once

#include "ConfigMemory.h"
#include "storage/AtomicStream.h"
#include <cstring>

namespace handheld::config {

// Count first, then write to a pre-reserved snapshot String or the atomic file
// sink. Serialize only the current value, never a tree of every setting.
// This keeps JSON escaping/number formatting identical to the reader's library.
class JsonWriter {
public:
    explicit JsonWriter(size_t limit, String* output = nullptr)
        : _limit(limit), _output(output) {}
    JsonWriter(size_t limit, storage::ByteSink& output)
        : _limit(limit), _sink(&output) {}

    template<typename T> void member(const char* key, const T& value) {
        prefix(key); scalar(value);
    }
    void object(const char* key = nullptr) { begin(key, '{'); }
    void array(const char* key) { begin(key, '['); }
    void endObject() { end('}'); }
    void endArray() { end(']'); }
    size_t size() const { return _size; }
    bool ok() const { return _ok && _depth == 0; }
    bool unavailable() const { return _unavailable; }

    size_t write(uint8_t byte) { return write(&byte, 1); }
    size_t write(const uint8_t* bytes, size_t length) {
        if (!_ok) return 0;
        if (length > _limit - _size) { _ok = false; return 0; }
        if (_output && !_output->concat(reinterpret_cast<const char*>(bytes), length)) {
            _ok = false; _unavailable = true; return 0;
        }
        if (_sink && _sink->write(bytes, length) != length) {
            _ok = false; return 0;
        }
        _size += length;
        return length;
    }

private:
    template<typename T> void scalar(const T& value) {
        if (!_ok) return;
        JsonAllocator allocator(UserValueLimit);
        JsonDocument doc(&allocator);
        if (!doc.set(value) || doc.overflowed()) {
            _ok = false; _unavailable = true; return;
        }
        serializeJson(doc, *this);
    }
    void scalar(const String& value) {
        // A linked ArduinoJson string uses strlen. Preserve legacy embedded
        // NULs with an owned length-aware value instead of truncating them.
        const char* bytes = value.c_str();
        const bool linked = !value.length() || !std::memchr(bytes, 0, value.length());
        scalar(JsonString(bytes ? bytes : "", value.length(), linked));
    }
    template<size_t N> void scalar(const char (&value)[N]) {
        scalar(JsonString(value, true));
    }
    void prefix(const char* key) {
        if (!_depth || !_ok) return;
        if (_members[_depth - 1]) write(',');
        _members[_depth - 1] = true;
        if (key) { scalar(JsonString(key, true)); write(':'); }
    }
    void begin(const char* key, uint8_t token) {
        if (_depth == sizeof _members) { _ok = false; return; }
        prefix(key); write(token); _members[_depth++] = false;
    }
    void end(uint8_t token) {
        if (!_depth) { _ok = false; return; }
        write(token); --_depth;
    }
    static constexpr size_t UserValueLimit = 65536;
    size_t _limit, _size = 0;
    String* _output = nullptr;
    storage::ByteSink* _sink = nullptr;
    bool _members[3]{};
    size_t _depth = 0;
    bool _ok = true, _unavailable = false;
};

} // namespace handheld::config
