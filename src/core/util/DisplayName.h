#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace handheld {
inline constexpr char DEVICE_NAME_PREFIX[] = "Ratspeak.org-";
inline constexpr size_t DEVICE_NAME_FALLBACK_SIZE = sizeof(DEVICE_NAME_PREFIX) + 3;
// Derive the same default label for Home and announcements from the active
// destination. Keep configured storage empty so identity switches recalculate it.
inline const char* deviceDisplayName(const char* name, const char* destination,
                                    const char* device,
                                    char (&fallback)[DEVICE_NAME_FALLBACK_SIZE]) {
    if (name && *name) return name;
    if (destination && strlen(destination) == 32) {
        bool hexadecimal = true;
        for (size_t i = 0; i < 32; ++i) {
            const char c = destination[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F'))) hexadecimal = false;
        }
        if (hexadecimal) {
            constexpr size_t prefixSize = sizeof(DEVICE_NAME_PREFIX) - 1;
            memcpy(fallback, DEVICE_NAME_PREFIX, prefixSize);
            memcpy(fallback + prefixSize, destination, 3);
            fallback[prefixSize + 3] = '\0';
            return fallback;
        }
    }
    return device;
}

// Complete UTF-8 scalar values, with no overlong, surrogate or out-of-range
// encodings. Used for new input validation and the existing announce prefix.
inline size_t displayNameCodepoint(const char* text, size_t remaining) {
    if (!remaining) return 0;
    const auto* p = reinterpret_cast<const uint8_t*>(text);
    const uint8_t first = p[0];
    if (first < 0x80) return first >= 0x20 && first != 0x7f ? 1 : 0;
    const size_t n = first >= 0xc2 && first <= 0xdf ? 2 :
        first >= 0xe0 && first <= 0xef ? 3 : first >= 0xf0 && first <= 0xf4 ? 4 : 0;
    if (!n || n > remaining) return 0;
    for (size_t i = 1; i < n; ++i) if ((p[i] & 0xc0) != 0x80) return 0;
    if ((first == 0xe0 && p[1] < 0xa0) || (first == 0xed && p[1] >= 0xa0) ||
        (first == 0xf0 && p[1] < 0x90) || (first == 0xf4 && p[1] >= 0x90)) return 0;
    return n;
}
inline bool validNewDisplayName(const char* text, size_t bytes) {
    size_t offset = 0, characters = 0;
    while (offset < bytes) {
        const size_t n = displayNameCodepoint(text + offset, bytes - offset);
        if (!n || ++characters > 16) return false;
        offset += n;
    }
    return true;
}
inline size_t displayNamePrefix(const char* text, size_t bytes, size_t maximum) {
    size_t offset = 0;
    while (offset < bytes) {
        const size_t n = displayNameCodepoint(text + offset, bytes - offset);
        if (!n || n > maximum - offset) break;
        offset += n;
    }
    return offset;
}
} // namespace handheld
