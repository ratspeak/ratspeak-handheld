#pragma once
#include <cstddef>
#include <cstdint>
namespace handheld::storage {
inline bool decodeHex(const char* source, size_t length, uint8_t* destination, size_t bytes) {
    if (!source || length != bytes * 2) return false;
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < bytes; ++i) {
        const int hi = digit(source[2 * i]), lo = digit(source[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        destination[i] = uint8_t(hi * 16 + lo);
    }
    return true;
}

inline void encodeHex(const uint8_t* source, size_t bytes, char* destination) {
    constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < bytes; ++i) {
        destination[2 * i] = hex[source[i] >> 4];
        destination[2 * i + 1] = hex[source[i] & 15];
    }
    destination[2 * bytes] = 0;
}

}
