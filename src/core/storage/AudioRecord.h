#pragma once

#include "StorageContract.h"
#include "AtomicStream.h"
#include <cstring>

namespace handheld::storage::audio {

inline bool valid(const AudioMetadata& value) {
    if (value.state == 1) return value.length <= Budget::MaxMessageBody;
    return (value.state == 0 || value.state == 2) &&
        !value.mode && !value.length && !value.checksum;
}
inline bool validBody(size_t title, size_t content, const AudioMetadata& value) {
    return valid(value) && Budget::validBody(title, content) &&
        value.length <= Budget::MaxMessageBody - title - content;
}
inline uint32_t checksum(uint32_t crc, const uint8_t* bytes, size_t length) {
    while (length--) {
        crc ^= *bytes++;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return crc;
}
// Domain, identity pair, direction, local counter, codec and exact byte count
// bind the JSON reference to its immutable raw sidecar. CRC detects local damage;
// incoming LXMF authentication remains the protocol owner's responsibility.
inline uint32_t seed(const RecordKey& key, const uint8_t source[16],
                     const uint8_t destination[16], const AudioMetadata& meta) {
    const uint8_t header[] = {'H', 'A', '1', uint8_t(key.incoming),
        uint8_t(key.counter), uint8_t(key.counter >> 8), uint8_t(key.counter >> 16), uint8_t(key.counter >> 24),
        meta.mode, uint8_t(meta.length), uint8_t(meta.length >> 8)};
    return checksum(checksum(checksum(UINT32_MAX, header, sizeof(header)), source, 16), destination, 16);
}
inline uint32_t digest(const RecordKey& key, const uint8_t source[16], const uint8_t destination[16],
                       const AudioMetadata& meta, const uint8_t* bytes) {
    return ~checksum(seed(key, source, destination, meta), bytes, meta.length);
}
inline bool verify(File& file, const RecordKey& key, const StoredRecordHeader& header,
                   uint8_t* output = nullptr, size_t offset = 0, size_t length = 0) {
    if (!file || header.audio.state != 1 || !valid(header.audio) ||
        offset > header.audio.length || length > header.audio.length - offset ||
        (length && !output) || file.size() != header.audio.length || !file.seek(0)) return false;
    uint32_t crc = seed(key, header.source, header.destination, header.audio);
    uint8_t chunk[256]; size_t remaining = header.audio.length, position = 0;
    while (remaining) {
        const size_t count = std::min(remaining, sizeof(chunk));
        if (file.read(chunk, count) != count) return false;
        crc = checksum(crc, chunk, count);
        // Return only bytes from this integrity pass. A later read could see
        // different media contents even under the filesystem owner's lease.
        const size_t start = std::max(position, offset), end = std::min(position + count, offset + length);
        if (start < end) memcpy(output + start - offset, chunk + start - position, end - start);
        remaining -= count; position += count; yield();
    }
    return ~crc == header.audio.checksum && file.seek(0);
}
// Used only under the existing filesystem lease, after full integrity checking.
// Rewind for each write/readback pass; no whole-clip allocation or retained File.
class FileSource final : public AtomicSource {
public:
    FileSource(File& file, size_t length, uint32_t seed, uint32_t checksum)
        : _file(file), _length(length), _seed(seed), _checksum(checksum) {}
    size_t length() const override { return _length; }
    bool emit(ByteSink& sink) const override {
        if (_file.size() != _length || !_file.seek(0)) return false;
        uint32_t crc = _seed;
        uint8_t chunk[256]; size_t remaining = _length;
        while (remaining) {
            const size_t count = std::min(remaining, sizeof(chunk));
            if (_file.read(chunk, count) != count || sink.write(chunk, count) != count) return false;
            crc = checksum(crc, chunk, count);
            remaining -= count; yield();
        }
        return ~crc == _checksum;
    }
private:
    File& _file;
    size_t _length;
    uint32_t _seed, _checksum;
};
}
