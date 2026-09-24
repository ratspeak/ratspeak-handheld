#pragma once

#include <cstdint>

// The SX1262 driver's established bandwidth values and rounding policy. Keep
// saved values and editors aligned with the value actually sent to the modem.
namespace RadioBandwidth {
struct Value { uint32_t hz; uint8_t code; };
inline constexpr Value values[] = {
#if !defined(RSM9)
    {7800, 0x00}, {10400, 0x08}, {15600, 0x01}, {20800, 0x09},
    {31250, 0x02}, {41700, 0x0a},
#endif
    {62500, 0x03}, {125000, 0x04},
    {250000, 0x05}, {500000, 0x06}
};
inline constexpr int count = sizeof(values) / sizeof(values[0]);

// Preserve the driver's existing upward rounding, including both end clamps.
inline constexpr int index(uint32_t requestedHz) {
    for (int i = 0; i < count - 1; ++i)
        if (requestedHz <= values[i].hz) return i;
    return count - 1;
}
inline constexpr uint32_t normalize(uint32_t requestedHz) {
    return values[index(requestedHz)].hz;
}
inline constexpr uint8_t code(uint32_t requestedHz) {
    return values[index(requestedHz)].code;
}
inline constexpr uint32_t fromCode(uint8_t code) {
    for (const auto& value : values) if (value.code == code) return value.hz;
    return 0;
}
} // namespace RadioBandwidth
