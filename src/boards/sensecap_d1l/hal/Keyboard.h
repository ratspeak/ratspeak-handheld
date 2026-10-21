#pragma once

#include <Arduino.h>
#include "input/KeyEvent.h"

// This board has no physical keyboard — HAS_KEYBOARD is false and nothing
// should ever instantiate this class. It exists only because UIManager.h
// unconditionally includes "hal/Keyboard.h" (every board provides one),
// even though UIManager.h itself never references the type.
class Keyboard {
public:
    bool begin() { return false; }
    void update() {}
    void discardPending() {}
    InputMode getMode() const { return _mode; }
    void setMode(InputMode mode) { _mode = mode; }
    bool hasEvent() const { return false; }
    const KeyEvent& getEvent() const { return _event; }
    // No physical backlight to drive — these exist only to satisfy Power's
    // call sites (same reasoning as the rest of this stub).
    bool setBacklightBrightness(uint8_t) { return false; }
    bool backlightOn() { return false; }
    bool backlightOff() { return false; }
    bool backlightIsLit() const { return false; }

private:
    InputMode _mode = InputMode::Navigation;
    KeyEvent _event;
};
