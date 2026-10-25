#pragma once

#include "config/UserConfig.h"

namespace RadioPresets {

struct Preset {
    const char* name;
    uint8_t sf;
    uint32_t bw;
    uint8_t cr;
    int8_t txPower;
    long preamble;
};

inline constexpr Preset values[] = {
    {"Short Turbo",   7,  500000, 5, 14, 18},
    {"Short Fast",    7,  250000, 5, 14, 18},
    {"Short Slow",    8,  250000, 5, 14, 18},
    {"Medium Fast",   9,  250000, 5, 17, 18},
    {"Medium Slow",  10,  250000, 5, 17, 18},
    {"Long Turbo",   11, 500000, 8, 22, 18},
    {"Long Fast",    11, 250000, 5, 22, 18},
    {"Long Moderate",11, 125000, 8, 22, 18},
};
inline constexpr int count = sizeof(values) / sizeof(values[0]);

inline int detect(const UserScalars& settings) {
    for (int i = 0; i < count; ++i) {
        const auto& preset = values[i];
        if (settings.loraSF == preset.sf && settings.loraBW == preset.bw &&
            settings.loraCR == preset.cr && settings.loraTxPower == preset.txPower &&
            settings.loraPreamble == preset.preamble) return i;
    }
    return -1;
}

inline const char* name(const UserScalars& settings) {
    const int index = detect(settings);
    return index < 0 ? "Custom" : values[index].name;
}

inline void apply(UserScalars& settings, int index) {
    if (index < 0 || index >= count) return;
    const auto& preset = values[index];
    settings.loraSF = preset.sf;
    settings.loraBW = preset.bw;
    settings.loraCR = preset.cr;
    settings.loraTxPower = preset.txPower;
    settings.loraPreamble = preset.preamble;
}

} // namespace RadioPresets
