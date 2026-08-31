#pragma once

#include <Arduino.h>
#include <time.h>

// Monotonic 64-bit millisecond clock for the FFI. millis() wraps at
// ~49.7 days; the accumulator is wrap-safe as long as nowMs() is called more
// than once per wrap period (the backend ticks it every loop pass). All use is
// restricted to the protocol owner task — no atomics needed.
class RustClock {
public:
    uint64_t nowMs() {
        uint32_t m = millis();
        if (m < _lastMs) _high += (1ULL << 32);
        _lastMs = m;
        return _high | (uint64_t)m;
    }

    // Unix seconds when the RTC has a plausible epoch (GPS/NTP/user set); 0
    // before that — callers must treat 0 as "no wall clock".
    static uint64_t epochSecs() {
        time_t t = time(nullptr);
        return (t > 1700000000) ? (uint64_t)t : 0;
    }

private:
    uint32_t _lastMs = 0;
    uint64_t _high = 0;
};

