#pragma once
#include "ratspeak_protocol.h"
#include <cstdint>

namespace handheld::voice {
// Rust owns the executable codec shape; C++ uses it for capture, framing and
// route cost. An unsupported profile returns an empty descriptor.
inline rs_handheld_voice_profile_t profileInfo(uint8_t profile) {
    rs_handheld_voice_profile_t info{};
    rs_handheld_voice_profile(profile, &info);
    return info;
}
// Capture-origin TX age includes assembly plus a finite dispatch window. RX
// starts a separate clock at receipt, allowing one packet of playback and the
// same bounded jitter window. Neither deadline survives input cancellation.
inline uint32_t mediaAge(const rs_handheld_voice_profile_t& info) {
    return info.interval_ms + 160;
}
}
