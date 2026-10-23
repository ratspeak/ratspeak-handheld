#pragma once
#include <Arduino.h>
#include "input/KeyEvent.h"
// Compatibility adapter for common lifecycle code. Text comes from TouchKeyboard.
class Keyboard {
public:
 bool begin() { return true; }
 void update() {}
 void discardPending() {}
 bool hasEvent() const { return false; }
 const KeyEvent& getEvent() const { static const KeyEvent empty{}; return empty; }
 void setMode(InputMode mode) { _mode=mode; }
 InputMode getMode() const { return _mode; }
 bool setBacklightBrightness(unsigned char) { return false; }
 bool backlightOn() { return false; }
 bool backlightOff() { return false; }
 bool backlightIsLit() const { return false; }
private:
 InputMode _mode=InputMode::Navigation;
};
