#pragma once

#include "protocol/RrcTypes.h"
#include "ratspeak_protocol.h"
#include "util/DisplayText.h"
#include <algorithm>
#include <cstring>

namespace handheld::ui::rrc_input {
// Pure, allocation-free input helpers. They use the trusted Rust codec without
// accessing a live session, identity secret, filesystem, or network owner.
inline bool address(const char* text, size_t length, uint8_t out[16]) {
    if (!text || !out) return false;
    auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    while (length && space(*text)) { ++text; --length; }
    while (length && space(text[length - 1])) --length;
    if (length != 32) return false;
    auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' :
        c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    uint8_t parsed[16];
    for (size_t n = 0; n < 16; ++n) {
        const int high = digit(text[n * 2]), low = digit(text[n * 2 + 1]);
        if (high < 0 || low < 0) return false;
        parsed[n] = uint8_t(high * 16 + low);
    }
    uint8_t any = 0; for (auto byte : parsed) any |= byte;
    if (!any) return false;
    std::memcpy(out, parsed, 16); return true;
}
inline bool room(const char* text, size_t length, char (&out)[65]) {
    size_t size = 0;
    if (!text || rs_handheld_rrc_normalize(reinterpret_cast<const uint8_t*>(text), length, 0,
        reinterpret_cast<uint8_t*>(out), 64, &size) != RS_HANDHELD_OK) return false;
    out[size] = 0; return true;
}
struct Preview {
    handheld::rrc::Code code = handheld::rrc::Code::Invalid;
    uint16_t maximum = 0, encoded = 0;
};
inline Preview preview(const handheld::rrc::Status& status, const handheld::rrc::Conversation& binding,
                       handheld::rrc::Action action, const uint8_t* text, size_t length) {
    using handheld::rrc::Action; using handheld::rrc::Code;
    Preview result;
    if (length > handheld::rrc::DraftCapacity) { result.code = Code::TooLong; return result; }
    if ((length && !text) || (length && std::memchr(text, 0, length)) || binding.room[64]) return result;
    const bool privateNotice = action == Action::PrivateNotice, emote = action == Action::Emote;
    if (action != Action::Message && !privateNotice && !emote && action != Action::Advanced && action != Action::Join) return result;
    if ((privateNotice && !(status.capabilities & 4)) || (emote && !(status.capabilities & 2))) {
        result.code = Code::Unsupported; return result;
    }
    const size_t roomLength = std::strlen(binding.room);
    if (roomLength > status.roomLimit) { result.code = Code::TooLong; return result; }
    if ((privateNotice && !binding.privateNotice()) ||
        (!privateNotice && action != Action::Advanced && !roomLength)) return result;
    rs_handheld_rrc_meta_t meta{};
    meta.kind = action == Action::Join ? 10 : privateNotice ? 21 : emote ? 22 : 20;
    // Current epoch milliseconds occupy the same CBOR width. Counting the
    // maximum keeps this limit safe before clock synchronization as well.
    meta.timestamp_ms = UINT64_MAX;
    if (privateNotice) { meta.has_destination = 1; std::memcpy(meta.destination, binding.participant, 16); }
    const size_t nickLength = strnlen(status.nickname, sizeof status.nickname);
    auto count = [&](const uint8_t* body, size_t size, size_t& written) {
        return rs_handheld_rrc_encode(&meta,
            privateNotice ? nullptr : reinterpret_cast<const uint8_t*>(binding.room), privateNotice ? 0 : roomLength,
            reinterpret_cast<const uint8_t*>(status.nickname), nickLength <= status.nicknameLimit ? nickLength : 0,
            body, size, action == Action::Join && !size ? 0 : 1, nullptr, 0, &written) == RS_HANDHELD_OK;
    };
    uint8_t sample[handheld::rrc::DraftCapacity]; std::memset(sample, 'x', sizeof sample);
    size_t low = 0, high = action == Action::Join ? handheld::rrc::KeyCapacity :
        std::min(size_t(status.bodyLimit), sizeof sample);
    while (low < high) {
        const size_t candidate = low + (high - low + 1) / 2; size_t size = 0;
        if (count(sample, candidate, size) && size <= handheld::rrc::PacketCapacity) low = candidate;
        else high = candidate - 1;
    }
    result.maximum = low;
    if (length > low) { result.code = Code::TooLong; return result; }
    size_t encoded = 0;
    if (!count(text, length, encoded)) return result;
    result.encoded = uint16_t(encoded);
    if (encoded > handheld::rrc::PacketCapacity) result.code = Code::TooLong;
    else if (length && action == Action::Message && text[0] == '/') result.code = Code::Invalid;
    else if (action == Action::Advanced && (!length || text[0] != '/')) result.code = Code::Invalid;
    else result.code = Code::Ok;
    return result;
}

inline size_t textPrefix(const char* text, size_t length, size_t capacity) {
    size_t at = 0;
    while (at < length) {
        bool escape = false;
        const auto size = display::codepoint(reinterpret_cast<const uint8_t*>(text + at), length - at, true, escape);
        if (!size || escape || at + size > capacity) break;
        at += size;
    }
    return at;
}
}
