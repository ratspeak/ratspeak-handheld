#pragma once

#include <Arduino.h>
#include <Wire.h>
#include "config/BoardConfig.h"

#include "input/KeyEvent.h"

class Keyboard {
public:
    bool begin();
    void update();
    // Consume the wake burst and cancel synthesis until a fresh press.
    void discardPending();

    // Mode control
    InputMode getMode() const { return _mode; }
    void setMode(InputMode mode) { _mode = mode; }

    // State queries
    bool hasEvent() const { return _hasEvent; }
    const KeyEvent& getEvent() const { return _event; }

    // Backlight control. LilyGO's C3 keyboard firmware gained backlight commands
    // on 2024-12-25 and offers no way to probe for them: writes are always
    // acknowledged and reads return only key state. Its own Alt+B toggle is
    // invisible to the host, and it keeps two levels, one for that toggle
    // (limited to PWM 31..255) and the current level; both are driven with the
    // same value here.
    bool setBacklightBrightness(uint8_t percent); // 0 stores off; non-zero doesn't change current brightness
    bool backlightOn();
    bool backlightOff();
    bool backlightIsLit() const { return _backlightLit; }

private:
    uint8_t readKey(uint8_t* modOut);
    static bool setBrightness(uint8_t pwm);

    InputMode _mode = InputMode::Navigation;
    KeyEvent _event = {};
    bool _hasEvent = false;
    uint8_t _backlightBrightness = 255; // PWM 31..255, or 0 when the light is off
    bool _backlightLit = false;         // last host-commanded state; C3 <Alt>+<B> toggles are invisible

    static Keyboard* _instance;
    static int _debugCount;          // Log first N keypresses
};
