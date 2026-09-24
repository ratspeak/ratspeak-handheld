#pragma once
#include <Arduino.h>
#include <Wire.h>
#include "input/KeyDecoder.h"

class Keyboard {
public:
    bool begin();
    void update();
    void discardPending();
    bool hasEvent() const { return _hasEvent; }
    const KeyEvent& getEvent() const { return _event; }
    bool hadHold() const { return _hold; }
    bool hadActivity() const { return _activity; }
    uint8_t revision() const { return _address == 0x6c ? 1 : _address == 0x6d ? 2 : 0; }
    InputMode getMode() const { return _mode; }
    void setMode(InputMode mode) { _mode = mode; }
    bool setBacklightBrightness(uint8_t percent);
    bool backlightOn();
    bool backlightOff();
    bool backlightIsLit() const { return _lit; }
private:
    uint8_t readKey();
    InputMode _mode = InputMode::Navigation;
    KeyEvent _event{};
    uint8_t _address = 0, _brightness = 0;
    bool _hasEvent = false, _hold = false, _activity = false, _lit = false;
};
