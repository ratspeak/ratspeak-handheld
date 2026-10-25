#pragma once
#include "protocol/RrcTypes.h"
#include "ratspeak_protocol.h"
#include "util/DisplayName.h"
#include <algorithm>
#include <cstring>
#include <string_view>

namespace handheld::rrc {
// Retain one bounded NOTICE, not a second room/cache tree. Names are normalized
// when projected. The hub's omission marker is never treated as pagination.
class Directory {
public:
    enum class Result : uint8_t { Unrelated, Invalid, Applied };
    static constexpr size_t Capacity = STORAGE_ASYNC_WRITES ? 24 : 48;
    static std::string_view trim(std::string_view value) {
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r' || value.front() == '\n')) value.remove_prefix(1);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' || value.back() == '\n')) value.remove_suffix(1);
        return value;
    }
    Result apply(const uint8_t* bytes, size_t length, uint16_t roomLimit) {
        auto value = trim({reinterpret_cast<const char*>(bytes), length});
        if (value == "No public rooms registered") { *this = {}; return Result::Applied; }
        constexpr std::string_view header = "Registered public rooms:";
        const auto end = value.find('\n');
        if (trim(value.substr(0, end)) != header) return Result::Unrelated;
        if (length > PacketCapacity) return Result::Invalid;
        auto lines = end == value.npos ? std::string_view{} : value.substr(end + 1);
        size_t count = 0; uint32_t omitted = 0; bool marker = false;
        auto remaining = lines;
        while (!remaining.empty()) {
            const auto line = next(remaining); if (line.empty()) continue;
            if (marker) return Result::Invalid;
            if (line.size() > 8 && line.substr(0, 2) == "(+" && line.substr(line.size() - 6) == " more)") {
                const auto number = line.substr(2, line.size() - 8);
                if (number.empty()) return Result::Invalid;
                for (const char c : number) {
                    if (c < '0' || c > '9' || omitted > (UINT32_MAX - uint32_t(c - '0')) / 10) return Result::Invalid;
                    omitted = omitted * 10 + uint32_t(c - '0');
                }
                marker = true; continue;
            }
            DirectoryView row; if (!parse(line, roomLimit, row)) return Result::Invalid;
            // Compare earlier canonical names without retaining a name vector.
            bool duplicate = false; auto earlier = lines;
            while (!earlier.empty()) {
                const auto old = next(earlier); if (old.data() == line.data()) break;
                DirectoryView prior;
                if (!old.empty() && parse(old, roomLimit, prior) && !strcmp(row.name, prior.name)) { duplicate = true; break; }
            }
            if (!duplicate) ++count;
        }
        _length = uint16_t(lines.size()); if (!lines.empty()) std::memcpy(_text, lines.data(), lines.size()); _text[lines.size()] = 0;
        _roomLimit = roomLimit; _count = uint8_t(std::min(count, Capacity));
        _omitted = omitted > UINT32_MAX - (count - _count) ? UINT32_MAX : omitted + uint32_t(count - _count);
        return Result::Applied;
    }
    size_t count() const { return _count; }
    uint32_t omitted() const { return _omitted; }
    bool at(size_t index, DirectoryView& out) const {
        if (index >= _count) return false;
        std::string_view all(_text, _length), remaining = all; size_t current = 0;
        while (!remaining.empty()) {
            const auto line = next(remaining); DirectoryView row;
            if (line.empty() || !parse(line, _roomLimit, row)) continue;
            bool duplicate = false; auto earlier = all;
            while (!earlier.empty()) { const auto previous = next(earlier); if (previous.data() == line.data()) break;
                DirectoryView prior; if (!previous.empty() && parse(previous, _roomLimit, prior) && !strcmp(row.name, prior.name)) { duplicate = true; break; } }
            if (!duplicate && current++ == index) { out = row; return true; }
        }
        return false;
    }
private:
    static std::string_view next(std::string_view& lines) {
        const auto end = lines.find('\n'); const auto line = trim(lines.substr(0, end));
        lines = end == lines.npos ? std::string_view{} : lines.substr(end + 1); return line;
    }
    static bool parse(std::string_view line, uint16_t limit, DirectoryView& row) {
        for (const auto c : line) if (uint8_t(c) < 32 || uint8_t(c) == 127) return false;
        if (line.substr(0, 2) == "(+") return false;
        const auto split = line.find(" - "); const auto name = trim(line.substr(0, split));
        size_t n = 0;
        if (rs_handheld_rrc_normalize(reinterpret_cast<const uint8_t*>(name.data()), name.size(), 0,
            reinterpret_cast<uint8_t*>(row.name), sizeof row.name - 1, &n) != RS_HANDHELD_OK || n > limit) return false;
        row.name[n] = 0;
        if (split != line.npos) {
            const auto topic = trim(line.substr(split + 3));
            const auto size = displayNamePrefix(topic.data(), topic.size(), sizeof row.topic - 1);
            std::memcpy(row.topic, topic.data(), size); row.topic[size] = 0;
        }
        return true;
    }
    char _text[PacketCapacity + 1]{};
    uint32_t _omitted = 0;
    uint16_t _length = 0, _roomLimit = 64;
    uint8_t _count = 0;
};
}
