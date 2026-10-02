#pragma once

#include "AtomicStream.h"
#include "Hex.h"
#include "StorageJson.h"
#include "StorageJsonAllocator.h"
#include "StorageJsonArena.h"
#include "LegacyMessageArena.h"
#include "AudioRecord.h"
#include <Arduino.h>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <optional>

namespace handheld::storage {

inline bool recordHeader(JsonVariantConst document, StoredRecordHeader& header) {
    if (!document.is<JsonObjectConst>() || !document["src"].is<const char*>() ||
        !document["dst"].is<const char*>() || !document["content"].is<const char*>() ||
        (!document["title"].isNull() && !document["title"].is<const char*>())) return false;
    const auto src = document["src"].as<JsonString>(), dst = document["dst"].as<JsonString>();
    if (!decodeHex(src.c_str(), src.size(), header.source, 16) ||
        !decodeHex(dst.c_str(), dst.size(), header.destination, 16)) return false;
    if (!document["store_revision"].isNull() && !document["store_revision"].is<uint32_t>()) return false;
    if (!document["status"].isNull() && (!document["status"].is<uint8_t>() ||
        document["status"].as<uint8_t>() > messaging::LastStatus)) return false;
    if (!document["delivery_policy"].isNull() &&
        (!document["delivery_policy"].is<uint8_t>() || document["delivery_policy"].as<uint8_t>() > 2)) return false;
    if (!document["incoming"].isNull() && !document["incoming"].is<bool>()) return false;
    if (!document["read"].isNull() && !document["read"].is<bool>()) return false;
    if (!document["prop_prepared"].isNull() && !document["prop_prepared"].is<bool>()) return false;
    header.revision = document["store_revision"] | uint32_t(0);
    header.status = document["status"] | uint8_t(0);
    header.timestamp = document["ts"] | 0.0;
    if (!std::isfinite(header.timestamp)) return false;
    header.incoming = document["incoming"] | false;
    header.deliveryPolicy = messaging::DeliveryPolicy(document["delivery_policy"] | uint8_t(0));
    if (!messaging::validDelivery(header.status, header.deliveryPolicy, header.incoming)) return false;
    header.read = document["read"] | false;
    header.titleLength = document["title"].as<JsonString>().size();
    header.contentLength = document["content"].as<JsonString>().size();
    header.hasMessageId = !document["msgid"].isNull();
    header.prepared = document["prop_prepared"] | false;
    header.audio = {};
    if (!document["audio"].isNull()) {
        const auto media = document["audio"].as<JsonArrayConst>();
        if (media.size() != 4 || !media[0].is<uint8_t>() || !media[1].is<uint8_t>() ||
            !media[2].is<uint16_t>() || !media[3].is<uint32_t>()) return false;
        header.audio = {media[3].as<uint32_t>(), media[2].as<uint16_t>(),
                        media[1].as<uint8_t>(), media[0].as<uint8_t>()};
        if (!audio::valid(header.audio)) return false;
    }
    if (header.prepared && (header.incoming || !header.hasMessageId ||
                            header.deliveryPolicy == messaging::DeliveryPolicy::DirectOnly)) return false;
    if (header.hasMessageId) {
        const auto id = document["msgid"].as<JsonString>();
        if (!decodeHex(id.c_str(), id.size(), header.messageId, 32)) return false;
    }
    return true;
}

inline bool recordAudio(JsonDocument& document, const AudioMetadata& media) {
    if (!audio::valid(media)) return false;
    if (!media.state) { document.remove("audio"); return true; }
    auto value = document["audio"].to<JsonArray>();
    value.add(media.state); value.add(media.mode); value.add(media.length); value.add(media.checksum);
    return !document.overflowed();
}

// The pinned JsonDeserializer resolves keys/nesting before invoking its filter.
// This allow-all filter only arms capture of the root timestamp's numeric bytes;
// it does not parse JSON or decide which values the SDK accepts. The SDK's own
// numeric buffer holds at most 63 characters, so no accepted token is truncated.
struct TimestampToken {
    char bytes[64] = {};
    uint8_t length = 0;
    bool seen = false, active = false, started = false, numeric = false, complete = false, invalid = false;
    bool jsonNumber() const {
        // Syntax only: the SDK remains responsible for numeric conversion.
        // Its permissive reader also accepts '+' prefixes and leading zeros,
        // which must not be emitted as a raw standard JSON number.
        size_t position = 0;
        auto digit = [&](size_t at) { return at < length && bytes[at] >= '0' && bytes[at] <= '9'; };
        if (position < length && bytes[position] == '-') ++position;
        if (!digit(position)) return false;
        if (bytes[position] == '0') ++position;
        else while (digit(position)) ++position;
        if (position < length && bytes[position] == '.') {
            ++position; if (!digit(position)) return false;
            while (digit(position)) ++position;
        }
        if (position < length && (bytes[position] == 'e' || bytes[position] == 'E')) {
            ++position;
            if (position < length && (bytes[position] == '+' || bytes[position] == '-')) ++position;
            if (!digit(position)) return false;
            while (digit(position)) ++position;
        }
        return position == length;
    }
    void begin() { *this = {}; seen = active = true; }
    void observe(int value) {
        if (!active) return;
        if (!started && (value == ' ' || value == '\t' || value == '\n' || value == '\r')) return;
        if (!started) {
            started = true;
            numeric = (value >= '0' && value <= '9') || value == '-' || value == '+' || value == '.';
            if (!numeric) { active = false; complete = true; return; }
        }
        const bool part = (value >= '0' && value <= '9') || value == '-' || value == '+' ||
                          value == '.' || value == 'e' || value == 'E';
        if (!part) { active = false; complete = true; return; }
        if (length == sizeof(bytes) - 1) { invalid = true; return; }
        bytes[length++] = char(value); bytes[length] = 0;
    }
};

struct TimestampFilter {
    TimestampToken* token;
    uint8_t depth = 0;
    bool allow() const { return true; }
    bool allowArray() const { return true; }
    bool allowObject() const { return true; }
    bool allowValue() const { return true; }
    TimestampFilter operator[](JsonString key) const {
        if (!depth && key.size() == 2 && !memcmp(key.c_str(), "ts", 2)) token->begin();
        return {token, uint8_t(depth + 1)};
    }
    template<class Index> TimestampFilter operator[](Index) const { return {token, uint8_t(depth + 1)}; }
};

class BufferedFileReader {
public:
    explicit BufferedFileReader(File& file, TimestampToken* token = nullptr) : _file(file), _token(token) {}
    int read() {
        if (_position == _length) {
            _length = _file.read(_bytes, sizeof(_bytes)); _position = 0;
            if (!_length) {
                // A zero read before a known end is a transient I/O failure,
                // not malformed JSON. Positive short reads may continue.
                _failed |= _file.position() < _file.size();
                if (_token) _token->observe(-1);
                return -1;
            }
        }
        const int value = _bytes[_position++];
        if (_token) _token->observe(value);
        return value;
    }
    size_t readBytes(char* output, size_t length) {
        size_t done = 0;
        for (; done < length; ++done) { const int value = read(); if (value < 0) break; output[done] = char(value); }
        return done;
    }
    void reset() { _position = _length = 0; _failed = false; }
    bool failed() const { return _failed; }
    bool ended() {
        for (int byte = read(); byte >= 0; byte = read())
            if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n') return false;
        return _file.position() == _file.size();
    }
private:
    File& _file;
    TimestampToken* _token;
    uint8_t _bytes[512];
    size_t _position = 0, _length = 0;
    bool _failed = false;
};

// One bounded document at a time. Normal records first try the approved owned
// JSON allocation cap. Exceptional existing records may retry by streaming into
// the reserved legacy arena. The input/output file is never retained as String.
class MessageDocument {
public:
    MessageDocument() : _normal(0) { normal(); }
    ~MessageDocument() { reset(); }
    MessageDocument(const MessageDocument&) = delete;
    MessageDocument& operator=(const MessageDocument&) = delete;
    JsonDocument& document() { return *_document; }
    const TimestampToken& timestampToken() const { return _timestamp; }
    bool exceptional() const { return _exceptional; }
    void reset() {
        _document.reset(); _arena.reset();
        if (_borrowed) legacyMessageArena().release();
        else freeAlignedStorage(_memory);
        _memory = nullptr; _exceptional = _borrowed = false;
    }
    void normal() {
        _timestamp = {};
        _document.reset(); _arena.reset();
        if (_exceptional) {
            if (_borrowed) legacyMessageArena().release();
            else freeAlignedStorage(_memory);
            _memory = nullptr; _borrowed = false;
        }
        _exceptional = false;
        if (!_memory) _memory = allocateAlignedStorage(Budget::JsonAllocator);
        if (_memory) {
            _arena.emplace(_memory, Budget::JsonAllocator); _document.emplace(&*_arena);
        } else _document.emplace(&_normal); // zero-budget allocator reports failure
    }
    Error parse(File& file) {
        normal();
        if (!file || !file.size() || file.size() > Budget::MaxStoredFile) return Error::InvalidRecord;
        BufferedFileReader reader(file, &_timestamp);
        const auto error = _memory ? deserializeJson(*_document, reader, TimestampFilter{&_timestamp})
                                   : DeserializationError(DeserializationError::NoMemory);
        if (!error) {
            const bool ended = reader.ended();
            return reader.failed() ? Error::Read : ended ? Error::None : Error::InvalidRecord;
        }
        if (reader.failed()) return Error::Read;
        if (error != DeserializationError::NoMemory) return Error::InvalidRecord;
        reset();
        if (legacyMessageArena().available()) {
            // Already reserved before networking/worker startup. Do not charge
            // the same block to a second free-heap admission check.
            _memory = legacyMessageArena().borrow();
            _borrowed = _memory != nullptr;
        } else {
            if (!Budget::canReserveLegacy(ESP.getFreeHeap(), ESP.getMaxAllocHeap())) {
                _document.emplace(&_normal); return Error::Allocation;
            }
            _memory = allocateAlignedStorage(Budget::LegacyScratch);
        }
        if (!_memory) { _document.emplace(&_normal); return Error::Allocation; }
        _exceptional = true;
        _arena.emplace(_memory, Budget::LegacyScratch);
        _document.emplace(&*_arena);
        if (!file.seek(0)) return Error::Read;
        reader.reset(); _timestamp = {};
        const auto retry = deserializeJson(*_document, reader, TimestampFilter{&_timestamp});
        // The exceptional arena was successfully reserved. Exhausting this
        // fixed schema envelope is a preserved recovery error, not transient
        // heap backpressure that could retry forever.
        const bool ended = !retry && reader.ended();
        return reader.failed() ? Error::Read : ended ? Error::None : Error::InvalidRecord;
    }
private:
    JsonAllocator _normal;
    void* _memory = nullptr;
    bool _exceptional = false;
    bool _borrowed = false;
    std::optional<JsonArena> _arena;
    std::optional<JsonDocument> _document;
    TimestampToken _timestamp;
};

// ArduinoJson's default floating formatter keeps nine significant digits,
// which rounds contemporary epoch timestamps to seconds. Only this known
// numeric field needs a precise token; the ordinary JSON traversal, strings
// and schema stay with ArduinoJson. New values are formatted once; loaded
// values retain their original token so unrelated mutations cannot change the
// SDK-decoded timestamp (and an outgoing retry's LXMF message ID).
class PreciseTimestamp {
public:
    explicit PreciseTimestamp(JsonVariant document, const TimestampToken* retained = nullptr) : _value(document["ts"]) {
        if (_value.isNull() || _value.is<int64_t>() || _value.is<uint64_t>()) return;
        if (!_value.is<double>()) { _error = Error::InvalidRecord; return; }
        _original = _value.as<double>();
        if (!std::isfinite(_original)) { _error = Error::InvalidRecord; return; }
        if (retained && retained->seen && retained->numeric) {
            if (retained->invalid || !retained->complete || !retained->jsonNumber()) { _error = Error::InvalidRecord; return; }
            if (sameValue(ArduinoJson::detail::parseNumber<double>(retained->bytes), _original)) {
                _changed = true;
                if (!_value.set(serialized(retained->bytes, retained->length))) _error = Error::Allocation;
                return;
            }
        }
        // JSON integer -0 loses its sign in the SDK. Preserve the floating
        // representation, including intentional edits between the two zeros.
        char token[32]; const int length = _original == 0 && std::signbit(_original)
            ? std::snprintf(token, sizeof(token), "-0.0")
            : std::snprintf(token, sizeof(token), "%.17g", _original);
        if (length <= 0 || size_t(length) >= sizeof(token)) { _error = Error::InvalidRecord; return; }
        // %g supplies the numeric grammar. Refuse locale-specific decimal
        // separators (or any other non-JSON token) before mutating the node.
        for (int i = 0; i < length; ++i) {
            const char byte = token[i];
            if ((byte < '0' || byte > '9') && byte != '-' && byte != '+' && byte != '.' && byte != 'e' && byte != 'E') {
                _error = Error::InvalidRecord; return;
            }
        }
        // This is the numeric reader used by the pinned JsonDeserializer.
        // It has ordinary ULP rounding and does not support every subnormal;
        // reject destructive overflow/underflow without excluding normal
        // timestamps merely because its decimal conversion is not bit-exact.
        const double readable = ArduinoJson::detail::parseNumber<double>(token);
        if (!std::isfinite(readable) || (_original != 0 && readable == 0)) {
            _error = Error::InvalidRecord; return;
        }
        _changed = true;
        if (!_value.set(serialized(token, size_t(length)))) _error = Error::Allocation;
    }
    ~PreciseTimestamp() { restore(); }
    Error error() const { return _error; }
    bool restore() {
        if (!_changed) return true;
        _changed = false;
        // Reuse the numeric slot released by raw-token substitution. No body
        // copy is retained; callers verify this even when token allocation fails.
        return _value.set(_original) && sameValue(_value.as<double>(), _original);
    }
    PreciseTimestamp(const PreciseTimestamp&) = delete;
    PreciseTimestamp& operator=(const PreciseTimestamp&) = delete;
private:
    static bool sameValue(double left, double right) {
        return left == right && std::signbit(left) == std::signbit(right);
    }
    JsonVariant _value;
    double _original = 0;
    Error _error = Error::None;
    bool _changed = false;
};

class JsonSource final : public AtomicSource {
public:
    explicit JsonSource(JsonVariant document) : _document(document) {
        measure();
    }
    explicit JsonSource(MessageDocument& document) : _document(document.document()), _retained(&document.timestampToken()) {
        measure();
    }
    Error error() const { return _error; }
    size_t length() const override { return _length; }
    bool emit(ByteSink& sink) const override {
        if (_error != Error::None) return false;
        // Scope substitution to each pass. A migration comparison may reuse
        // its parser document before this source descriptor leaves scope.
        PreciseTimestamp timestamp(_document, _retained);
        const bool emitted = timestamp.error() == Error::None && writeStoredJson(_document, sink, _length);
        return timestamp.restore() && emitted;
    }
private:
    void measure() {
        PreciseTimestamp timestamp(_document, _retained); _error = timestamp.error();
        if (_error == Error::None) _length = measureStoredJson(_document);
        if (!timestamp.restore()) _error = Error::Allocation;
    }
    JsonVariant _document;
    const TimestampToken* _retained = nullptr;
    size_t _length = 0;
    Error _error = Error::None;
};

inline bool createRecord(JsonDocument& document, const Request& request, const uint8_t* body) {
    char hash[65];
    encodeHex(request.source, 16, hash);
    document["src"] = JsonString(hash, size_t(32), false);
    encodeHex(request.destination, 16, hash);
    document["dst"] = JsonString(hash, size_t(32), false);
    document["ts"] = request.timestamp;
    document["title"] = JsonString(reinterpret_cast<const char*>(body), request.titleLength, false);
    document["content"] = JsonString(reinterpret_cast<const char*>(body + request.titleLength), request.contentLength, false);
    document["incoming"] = request.operation == Operation::CreateIncoming;
    document["read"] = request.operation == Operation::CreateOutgoing || request.read;
    document["status"] = request.status;
    document["delivery_policy"] = uint8_t(request.deliveryPolicy);
    document["store_revision"] = uint32_t(1);
    if (request.hasMessageId) {
        encodeHex(request.messageId, 32, hash);
        document["msgid"] = JsonString(hash, size_t(64), false);
    }
    return !document.overflowed();
}

} // namespace handheld::storage
