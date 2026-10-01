#pragma once
#include "protocol/RrcTypes.h"
#include "storage/RrcRecord.h"
#include "util/DisplayText.h"
#include <cstring>

namespace handheld::rrc {
inline bool validDraftText(const uint8_t* text, size_t length) {
    if ((length && !text) || length > DraftCapacity) return false;
    for (size_t at=0;at<length;) {
        bool escape=false;
        const auto size=display::codepoint(text+at,length-at,true,escape);
        if (!text[at] || !size || (text[at]>=0x80 && escape)) return false;
        at+=size;
    }
    return true;
}
// Private application payloads inside CRC-protected, identity/hub-bound records.
// The root and hub indices hold keys only; saved room bodies are read on demand.
// All layouts are byte-defined, never serialized compiler structs.
constexpr size_t SavedRoomCapacity = RoomCapacity * 2;
struct SavedIndex {
    uint8_t keys[16][16]{};
    uint8_t count = 0;
    int find(const uint8_t key[16]) const {
        for (uint8_t n = 0; n < count; ++n) if (!std::memcmp(keys[n], key, 16)) return n;
        return -1;
    }
    bool add(const uint8_t key[16], size_t maximum) {
        if (find(key) >= 0) return true;
        if (count >= maximum || count >= 16) return false;
        std::memcpy(keys[count++], key, 16); return true;
    }
    void remove(const uint8_t key[16]) {
        const auto at = find(key); if (at < 0) return;
        for (size_t n = size_t(at); n + 1 < count; ++n) std::memcpy(keys[n], keys[n + 1], 16);
        std::memset(keys[--count], 0, 16);
    }
};
inline size_t encodeIndex(uint8_t* out, const SavedIndex& index, const char* name, const char* nickname) {
    std::memset(out, 0, 72); std::memcpy(out, "HRI1", 4); out[4] = index.count;
    if (name) std::memcpy(out + 6, name, strnlen(name, 32));
    if (nickname) std::memcpy(out + 39, nickname, strnlen(nickname, 32));
    std::memcpy(out + 72, index.keys, size_t(index.count) * 16); return 72 + size_t(index.count) * 16;
}
inline bool decodeIndex(const uint8_t* data, size_t length, SavedIndex& index, char* name, char* nickname) {
    if (length < 72 || std::memcmp(data, "HRI1", 4) || data[4] > 26 || data[5] || data[38] || data[71] || length != 72 + size_t(data[4]) * 16) return false;
    SavedIndex candidate; candidate.count = data[4]; std::memcpy(candidate.keys, data + 72, size_t(candidate.count) * 16);
    for (size_t n = 0; n < candidate.count; ++n) for (size_t m = n + 1; m < candidate.count; ++m)
        if (!std::memcmp(candidate.keys[n], candidate.keys[m], 16)) return false;
    index = candidate;
    if (name) std::memcpy(name, data + 6, 33);
    if (nickname) std::memcpy(nickname, data + 39, 33);
    return true;
}
// 72 bytes of metadata + at most 352 encrypted bytes = one 431-byte payload.
// With a 64-byte room and 160-byte key, RSCHKEY v1 seals to exactly 352 bytes.
constexpr size_t RoomPreferenceHeader = 72, SealedKeyCapacity = 352;
inline size_t roomPreference(uint8_t* out, const char* name, uint8_t notifications, const uint8_t* sealed, size_t length) {
    if (notifications > 2 || length > SealedKeyCapacity || (length && !sealed) || !name || !name[0] || strnlen(name, 65) > 64) return 0;
    std::memset(out, 0, RoomPreferenceHeader); std::memcpy(out, "HRP1", 4);
    out[4] = notifications; out[5] = uint8_t(length); out[6] = uint8_t(length >> 8);
    std::memcpy(out + 7, name, std::strlen(name));
    if (length) std::memcpy(out + RoomPreferenceHeader, sealed, length);
    return RoomPreferenceHeader + length;
}
inline bool validRoomPreference(const uint8_t* data, size_t length) {
    if (length < RoomPreferenceHeader || length > RoomPreferenceHeader + SealedKeyCapacity ||
        std::memcmp(data, "HRP1", 4) || data[4] > 2 || !data[7] || data[71]) return false;
    const size_t size = size_t(data[5]) | size_t(data[6]) << 8;
    return length == RoomPreferenceHeader + size;
}
}
