#pragma once

#include <stdint.h>
#include "KeyEvent.h"

// UI-owner arbitration for a trackball or encoder. Motion is deliberately
// coalesced (Deck +/-20 pulses, Pager +/-2 detents); clicks are discrete.
// Two pending clicks plus LvInput's six keys fit the eight-event input budget.
class PointerInput {
public:
    void update(int dx, int dy, bool down, bool screenOn, uint32_t now,
                uint8_t speed, bool encoder, bool clickEnabled=true) {
        _now = now;
        _encoder = encoder;
        _threshold = encoder ? 1 : 6 - speed;
        _rate = 300 - 40 * speed;
        activity = dx || dy;
        strongActivity = false;
        longPress = false;
        if (screenOn) {
            const int limit = encoder ? 2 : 20;
            _x = clamp(int(_x) + dx, limit);
            _y = clamp(int(_y) + dy, limit);
        } else {
            _x = _y = 0;
            _clicks = 0;
        }
        if (!clickEnabled || _waitClickRelease) {
            _down=false;_fired=false;_clicks=0;
            _waitClickRelease=!clickEnabled || down;
        } else if (down) {
            _lastDown = now;
            if (!_down) {
                _down = true;
                _fired = false;
                _fromScreenOn = screenOn;
                _started = now;
                activity = strongActivity = true;
            } else if (!_fired && now - _started >= 1200) {
                longPress = _fromScreenOn && screenOn;
                _fired = true;
                _x = _y = 0;
                _clicks = 0;
                // A wake hold must not wake the display again after timeout.
                activity = strongActivity = longPress;
            }
        } else if (_down && now - _lastDown >= 80) {
            _down = false;
            if (!_fired && _fromScreenOn && screenOn && _clicks < 2) {
                ++_clicks;
                activity = strongActivity = true;
                // Motion accompanying a click belongs to the clicked position.
                _x = _y = 0;
            }
            _fired = false;
        }
    }

    bool preferPointer() const { return _clicks || (_keyboardLast && motionReady()); }
    void keyboardDelivered() { _keyboardLast = true; }
    bool take(KeyEvent& event) {
        event = {};
        if (_clicks) {
            --_clicks;
            event.enter = true;
            event.character = '\n';
        } else if (motionReady()) {
            const bool vertical = magnitude(_y) >= magnitude(_x);
            int8_t& axis = vertical ? _y : _x;
            if (vertical) { event.up = axis < 0; event.down = axis > 0; }
            else { event.left = axis < 0; event.right = axis > 0; }
            if (_encoder) axis += axis < 0 ? 1 : -1;
            else _x = _y = 0;
        } else return false;
        event.source = _encoder ? InputSource::Wheel : InputSource::Trackball;
        _lastNav = _now;
        _keyboardLast = false;
        return true;
    }

    bool activity = false;
    bool strongActivity = false;
    bool longPress = false;

private:
    static int magnitude(int value) { return value < 0 ? -value : value; }
    static int8_t clamp(int value, int limit) {
        return value > limit ? limit : (value < -limit ? -limit : value);
    }
    bool motionReady() const {
        return _now - _lastNav >= _rate &&
               (magnitude(_x) >= _threshold || magnitude(_y) >= _threshold);
    }
    int8_t _x = 0, _y = 0;
    uint8_t _clicks = 0, _threshold = 3;
    bool _down = false, _fired = false, _fromScreenOn = true;
    bool _keyboardLast = false, _encoder = false, _waitClickRelease = false;
    uint32_t _started = 0, _lastDown = 0, _lastNav = 0, _now = 0, _rate = 180;
};
static_assert(sizeof(PointerInput) <= 40, "Pointer arbitration exceeds its input budget");
