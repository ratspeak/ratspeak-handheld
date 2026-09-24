#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <atomic>
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
    uint8_t revision() const {
        const uint8_t address = _address.load();
        return address == 0x6c ? 1 : address == 0x6d ? 2 : 0;
    }
    InputMode getMode() const { return _mode; }
    void setMode(InputMode mode) { _mode = mode; }
    bool setBacklightBrightness(uint8_t percent);
    bool backlightOn();
    bool backlightOff();
    bool backlightIsLit() const { return _lit; }
private:
    uint8_t readKey();
    bool probe(bool reportFailure);
    InputMode _mode = InputMode::Navigation;
    KeyEvent _event{};
    // UI owns discovery; the service task reads revision for GNSS power.
    std::atomic<uint8_t> _address{0};
    uint8_t _brightness = 0;
    uint32_t _lastProbe = 0;
    bool _hasEvent = false, _hold = false, _activity = false, _lit = false;
};
