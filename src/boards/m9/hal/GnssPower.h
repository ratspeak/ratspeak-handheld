#pragma once
#include <Arduino.h>
#include "config/BoardConfig.h"

namespace m9 {
inline bool setGnssPower(uint8_t revision, bool enabled) {
    if (revision != 1 && revision != 2) return false;
    digitalWrite(GPS_ENABLE_PIN, enabled == (revision == 2) ? HIGH : LOW);
    return true;
}

inline void initGnssPower(uint8_t revision) {
    if (revision != 1 && revision != 2) return;
    // M9 reset is active HIGH, unlike the radio reset. Release it before
    // enabling the receiver. Keep RTC_INT at its board-reference idle level.
    digitalWrite(GPS_RESET_PIN, LOW);
    pinMode(GPS_RESET_PIN, OUTPUT);
    digitalWrite(GPS_RTC_INT_PIN, LOW);
    pinMode(GPS_RTC_INT_PIN, OUTPUT);
    setGnssPower(revision, false);
    pinMode(GPS_ENABLE_PIN, OUTPUT);
}
} // namespace m9
