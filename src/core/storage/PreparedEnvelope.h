#pragma once

#include "StorageContract.h"
#include <cstring>

namespace handheld::storage::prepared {
// Private storage format, not a wire format. Only public metadata and the
// recipient-encrypted entry are retained; never persist ephemeral private keys.
// These fixed bytes precede the encrypted entry in the existing storage payload.
constexpr size_t Header = 112;
constexpr size_t EntryMax = Budget::MaxMessageBody - 46; // upload array + PN stamp
constexpr size_t Max = Header + EntryMax;
static_assert(Max <= Budget::LargePayload, "Prepared envelope must use one existing credit");
inline uint32_t read32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline void write32(uint8_t* p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(value >> (8 * i));
}
inline uint32_t checksum(uint32_t crc, const uint8_t* bytes, size_t length) {
    while (length--) {
        crc ^= *bytes++;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return crc;
}
inline bool make(uint8_t (&out)[Header], const RecordKey& key, const uint8_t source[16],
                 const uint8_t messageId[32], const uint8_t transientId[32],
                 const uint8_t* entry, size_t length, uint8_t recipientCost = 0) {
    if (!key.counter || key.incoming || !source || !messageId || !transientId || !entry ||
        length < 112 || length > EntryMax || std::memcmp(entry, key.peer, 16)) return false;
    std::memset(out, 0, Header); std::memcpy(out, "HPW1", 4); write32(out + 4, key.counter);
    std::memcpy(out + 8, source, 16); std::memcpy(out + 24, key.peer, 16);
    std::memcpy(out + 40, messageId, 32); std::memcpy(out + 72, transientId, 32);
    out[104] = uint8_t(length); out[105] = uint8_t(length >> 8);
    out[106] = recipientCost; // Frozen recipient policy satisfied before encryption.
    write32(out + 108, ~checksum(checksum(UINT32_MAX, out, 108), entry, length));
    return true;
}
// CRC detects storage damage. The protocol owner independently recomputes the
// transient hash and validates its message binding before any network operation.
inline bool valid(const uint8_t* bytes, size_t length, const RecordKey& key,
                  const uint8_t source[16], const uint8_t* messageId = nullptr) {
    if (!bytes || !source || !key.counter || key.incoming || length < Header + 112 || length > Max ||
        std::memcmp(bytes, "HPW1", 4) || read32(bytes + 4) != key.counter ||
        std::memcmp(bytes + 8, source, 16) || std::memcmp(bytes + 24, key.peer, 16) ||
        std::memcmp(bytes + Header, key.peer, 16) || bytes[107] ||
        size_t(bytes[104] | uint16_t(bytes[105]) << 8) != length - Header ||
        (messageId && std::memcmp(bytes + 40, messageId, 32))) return false;
    return read32(bytes + 108) == ~checksum(checksum(UINT32_MAX, bytes, 108), bytes + Header, length - Header);
}
}
