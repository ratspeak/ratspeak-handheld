#include "input/InputManager.h"
#include "LvSoftKeyboard.h"

void InputManager::update(bool dispatchReady) {
    _hasKey = _activity = _hold = false;
    const bool screenOn = !_power || _power->isScreenOn();
    const uint32_t now = millis();

    if (_kb) {
        _kb->update();
        _activity = _kb->hadActivity() || _kb->hasEvent() || _kb->hadHold();
        // A press that wakes the screen does nothing else, including the tap
        // or hold that completes after the screen is already back on.
        if (!screenOn && _activity) _wakeGesture = true;
        if (_wakeGesture) {
            if (!_kb->anyDown()) _wakeGesture = false;
        } else if (_kb->hadHold()) {
            _hold = true;
        } else if (dispatchReady && _kb->hasEvent()) {
            _event = _kb->getEvent();
            _hasKey = true;
        }
    }
    if (!_hasKey && !_hold && screenOn && dispatchReady && LvSoftKeyboard::take(_event)) {
        _hasKey = _activity = true;
    }

    // Keep polling while dark so LvInput can reject a touch held across sleep.
    if (_touch && now - _lastTouchPoll >= 20) {
        _lastTouchPoll = now;
        _touch->update();
        if (screenOn && _touch->isTouched()) _activity = true;
    }
}
