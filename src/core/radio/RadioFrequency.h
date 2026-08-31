#pragma once

#include <stdint.h>

// Bands with image-calibration settings supported by the SX1262 driver.
// These are not regional permissions or a guarantee of board/antenna coverage.
struct LoRaFrequencyBand {
    uint32_t minHz;
    uint32_t maxHz;
    uint8_t imageCal[2];
};

inline constexpr LoRaFrequencyBand LORA_FREQUENCY_BANDS[] = {
    {430000000UL, 440000000UL, {0x6B, 0x6F}},
    {470000000UL, 510000000UL, {0x75, 0x81}},
    {779000000UL, 787000000UL, {0xC1, 0xC5}},
    {863000000UL, 870000000UL, {0xD7, 0xDB}},
    {902000000UL, 928000000UL, {0xE1, 0xE9}},
};

inline constexpr uint32_t LORA_MIN_FREQUENCY = LORA_FREQUENCY_BANDS[0].minHz;
inline constexpr uint32_t LORA_MAX_FREQUENCY =
    LORA_FREQUENCY_BANDS[sizeof(LORA_FREQUENCY_BANDS) / sizeof(LoRaFrequencyBand) - 1].maxHz;

inline constexpr const LoRaFrequencyBand* loRaFrequencyBand(uint32_t hz) {
    for (const auto& band : LORA_FREQUENCY_BANDS) {
        if (hz >= band.minHz && hz <= band.maxHz) return &band;
    }
    return nullptr;
}
