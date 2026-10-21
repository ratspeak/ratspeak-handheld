#include "InputManager.h"

void InputManager::update(bool dispatchReady) {
    _activity = false;
    _longPress = false;
    const uint32_t now = millis();
    const bool screenOn = !_power || _power->isScreenOn();

    // Touch is this board's only wake source (no keyboard/trackball), so
    // unlike T-Deck's equivalent poll this must NOT gate on screenOn — the
    // main loop's wake dispatch (LvglApplication.cpp, "Keyboard/touch: wake
    // from any state") relies on hadStrongActivity() firing while the
    // screen is off. A stale touch held across sleep still can't misfire as
    // a tap: LvInput::setEnabled(screenWasOn) keeps the pointer indev
    // disabled until the next tick, and wakeOnlyInput suppresses dispatch
    // on the waking tick itself.
    if (_touch && now - _lastTouchPoll >= 20) {
        _lastTouchPoll = now;
        _touch->update();
        if (_touch->isTouched()) _activity = true;
    }

    // Active-low momentary button; simple debounce, edge-triggered for the
    // tap-to-wake case, held-duration for the hold-to-sleep case.
    const bool btnDown = digitalRead(PIN_USER_BTN) == LOW;
    if (btnDown != _btnWasDown) {
        _btnWasDown = btnDown;
        if (btnDown && now - _lastBtnChange > 50) {
            _activity = true;
            _btnPressStart = now;
            _btnFired = false;
            _btnFromScreenOn = screenOn;
        }
        _lastBtnChange = now;
    } else if (btnDown && !_btnFired && now - _btnPressStart >= 1200) {
        _btnFired = true;
        // A hold that woke the screen must not also immediately sleep it
        // again — same guard as PointerInput's trackball/encoder hold.
        if (_btnFromScreenOn && screenOn) _longPress = true;
    }
}
