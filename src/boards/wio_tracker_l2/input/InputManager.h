#pragma once
#include "hal/Keyboard.h"
#include "hal/TouchInput.h"
#include "hal/Power.h"

// Buttons, on-screen keyboard keys and touch. Touch never wakes the screen:
// the board is often face-up and unlocked, so only the buttons do.
class InputManager {
public:
    void begin(Keyboard* kb, TouchInput* touch) { _kb = kb; _touch = touch; }
    void setPowerMgr(Power* power) { _power = power; }
    void update(bool dispatchReady = true);
    bool hasKeyEvent() const { return _hasKey; }
    const KeyEvent& getKeyEvent() const { return _event; }
    bool hadActivity() const { return _activity; }
    bool hadStrongActivity() const { return _activity; }
    bool hadLongPress() const { return _hold; }
private:
    Keyboard* _kb = nullptr;
    TouchInput* _touch = nullptr;
    Power* _power = nullptr;
    KeyEvent _event{};
    uint32_t _lastTouchPoll = 0;
    bool _hasKey = false, _activity = false, _hold = false, _wakeGesture = false;
};
