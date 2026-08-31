#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <esp_system.h>

// Platform entropy for the FFI: esp_random() is the hardware TRNG (RF
// subsystem enabled on all three boards). The Rust side generates no
// randomness — every key/iv/tag/seed is caller-supplied through this helper.
// Never use fixed values on-device; fixed entropy is vectors-only.
namespace RustEntropy {

inline void fill(uint8_t* buf, size_t len) {
    size_t i = 0;
    while (i + 4 <= len) {
        uint32_t r = esp_random();
        memcpy(buf + i, &r, 4);
        i += 4;
    }
    if (i < len) {
        uint32_t r = esp_random();
        memcpy(buf + i, &r, len - i);
    }
}

}  // namespace RustEntropy

