#include "InputManager.h"
#include "voice/AudioCoordinator.h"
#include "config/BoardConfig.h"

void InputManager::begin(Keyboard* kb, Trackball* pointer, TouchInput* touch) {
    _kb = kb;
    _pointer = pointer;
    _touch = touch;
}

void InputManager::update(bool dispatchReady) {
    _hasKey = _activity = _strongActivity = false;
    const bool screenOn = !_powerMgr || _powerMgr->isScreenOn();
    const uint32_t now = millis();
    const bool clickEnabled=!handheld::voice::AudioCoordinator::instance().blocksPointerClick();
    if (_pointer) _pointer->update();
    _pointerInput.update(_pointer ? _pointer->lastDeltaX() : 0,
                         _pointer ? _pointer->lastDeltaY() : 0,
                         _pointer && clickEnabled && digitalRead(TBALL_CLICK) == LOW,
                         screenOn, now, _speed, false, clickEnabled);
    _activity = _pointerInput.activity;
    _strongActivity = _pointerInput.strongActivity;
    // A screen-off long press cancels its concurrent keyboard burst. Leaving
    // that burst pending would immediately wake the screen on the next poll.
    if (_pointerInput.longPress && _kb) _kb->discardPending();

    // Short clicks win the next dispatch; retained motion alternates with
    // keyboard input. Never dequeue a key merely to overwrite it with a click.
    if (screenOn && dispatchReady && !_pointerInput.longPress && _pointerInput.preferPointer()) {
        _hasKey = _pointerInput.take(_keyEvent);
    }
    if (_kb && !_hasKey && !_pointerInput.longPress && (dispatchReady || !screenOn)) {
        _kb->update();
        if (_kb->hasEvent()) {
            _activity = _strongActivity = true;
            if (screenOn) {
                _keyEvent = _kb->getEvent();
                _hasKey = true;
                _pointerInput.keyboardDelivered();
            } else {
                // This entire already-buffered gesture wakes only. Held
                // deletion cannot leak a synthetic repeat into the new screen.
                _kb->discardPending();
            }
        }
    }
    if (!_hasKey && screenOn && dispatchReady && !_pointerInput.longPress) {
        _hasKey = _pointerInput.take(_keyEvent);
    }
    if (_hasKey) _activity = _strongActivity = true;

    // Touch remains weak while the display is off. Keep polling releases so
    // LvInput can reject a touch held across sleep until a fresh press.
    if (_touch && now - _lastTouchPoll >= 20) {
        _lastTouchPoll = now;
        _touch->update();
        if (screenOn && _touch->isTouched()) _activity = _strongActivity = true;
    }
}
