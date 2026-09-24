#pragma once
#include "hal/Keyboard.h"
#include "hal/Power.h"

// Two retained keys plus LVGL's six preserve the eight-event input ceiling.
// Always drain the hardware's single event register, even with UI backpressure.
class InputManager {
public:
    void begin(Keyboard* kb) { _kb = kb; }
    void setPowerMgr(Power* power) { _power = power; }
    void update(bool dispatchReady = true);
    bool hasKeyEvent() const { return _hasKey; }
    const KeyEvent& getKeyEvent() const { return _event; }
    bool hadActivity() const { return _activity; }
    bool hadStrongActivity() const { return _activity; }
    bool hadLongPress() const { return _hold; }
    uint32_t droppedKeys() const { return _dropped; }
private:
    Keyboard* _kb = nullptr;
    Power* _power = nullptr;
    KeyEvent _pending[2]{}, _event{};
    uint8_t _head = 0, _count = 0;
    uint32_t _dropped = 0;
    bool _hasKey = false, _activity = false, _hold = false;
};
