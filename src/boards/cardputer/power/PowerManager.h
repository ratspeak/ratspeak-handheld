#pragma once

#include <Arduino.h>
#include <M5Unified.h>

class PowerManager {
public:
    enum State { ACTIVE, DIMMED, SCREEN_OFF };

    void begin();
    void loop();

    // Call on any user activity (keypress, etc.)
    void activity();

    // Configuration (seconds)
    void setDimTimeout(uint16_t seconds) { _dimTimeout = seconds * 1000UL; }
    void setOffTimeout(uint16_t seconds) { _offTimeout = seconds * 1000UL; }
    // Core UserConfig stores brightness as percent 1-100; hardware wants 0-255
    void setBrightness(uint8_t pct) {
        if (pct > 100) pct = 100;
        _fullBrightness = (uint8_t)(((uint16_t)pct * 255) / 100);
    }

    State state() const { return _state; }
    bool isScreenOn() const { return _state != SCREEN_OFF; }

private:
    void setState(State newState);

    State _state = ACTIVE;
    unsigned long _lastActivity = 0;
    unsigned long _dimTimeout = 30000;   // 30s
    unsigned long _offTimeout = 60000;   // 60s
    uint8_t _fullBrightness = 255;
    static constexpr uint8_t DIM_BRIGHTNESS = 64;
};
