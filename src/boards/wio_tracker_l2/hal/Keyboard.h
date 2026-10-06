#pragma once
#include <Arduino.h>
#include "input/KeyEvent.h"

// The board's two buttons in the keyboard role the shared runtime expects.
// BOOT (GPIO0) taps are Back; holding either button is the long-press action.
// The top button wakes the screen. Text comes from the on-screen keyboard.
class Keyboard {
public:
    bool begin();
    void update();
    void discardPending() { _hasEvent = _hold = _activity = false; _event = {}; }
    bool hasEvent() const { return _hasEvent; }
    const KeyEvent& getEvent() const { return _event; }
    bool hadHold() const { return _hold; }
    bool hadActivity() const { return _activity; }
    bool anyDown() const { return _boot.down || _top.down; }

    // Factory-reset recovery runs before the on-screen keyboard exists and
    // asks for R/X/B/Enter: tap top = R, tap BOOT = X, hold BOOT = B,
    // hold top = Enter.
    void setRecoveryKeys(bool enabled) { _recovery = enabled; }

    // No key backlight on this board.
    bool setBacklightBrightness(uint8_t) { return true; }
    bool backlightOn() { return true; }
    bool backlightOff() { return true; }
    bool backlightIsLit() const { return false; }

private:
    struct Button {
        bool down = false, held = false;
        uint32_t since = 0;
    };
    enum class Gesture : uint8_t { None, Tap, Hold };
    Gesture track(Button& button, bool down, uint32_t now);
    void emit(bool boot, Gesture gesture);

    Button _boot, _top;
    KeyEvent _event{};
    uint32_t _lastPoll = 0;
    bool _hasEvent = false, _hold = false, _activity = false, _recovery = false;
};
