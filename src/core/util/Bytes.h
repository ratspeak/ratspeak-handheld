#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace rs {

// First-party byte buffer covering the surface the app used from the retired
// microReticulum RNS::Bytes (hash/id fields + hex round-trips). Storage-format
// compatible: toHex emits lowercase, matching the persisted JSON schema.
class Bytes {
public:
    Bytes() = default;
    Bytes(const uint8_t* data, size_t len) : _data(data, data + len) {}

    size_t size() const { return _data.size(); }
    bool empty() const { return _data.empty(); }
    const uint8_t* data() const { return _data.data(); }

    std::string toHex() const {
        static const char* kHex = "0123456789abcdef";
        std::string out;
        out.reserve(_data.size() * 2);
        for (uint8_t b : _data) {
            out.push_back(kHex[b >> 4]);
            out.push_back(kHex[b & 0x0F]);
        }
        return out;
    }

    // Parses complete hex pairs; stops at the first non-hex character.
    void assignHex(const char* hex) {
        _data.clear();
        if (!hex) return;
        while (hex[0] && hex[1]) {
            int hi = hexVal(hex[0]);
            int lo = hexVal(hex[1]);
            if (hi < 0 || lo < 0) break;
            _data.push_back(static_cast<uint8_t>((hi << 4) | lo));
            hex += 2;
        }
    }

    void clear() { _data.clear(); }
    void append(const uint8_t* data, size_t len) { _data.insert(_data.end(), data, data + len); }

    bool operator==(const Bytes& other) const { return _data == other._data; }
    bool operator!=(const Bytes& other) const { return !(*this == other); }

private:
    static int hexVal(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    std::vector<uint8_t> _data;
};

}  // namespace rs
