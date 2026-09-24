#include "input/InputManager.h"

void InputManager::update(bool dispatchReady) {
    _hasKey = _activity = _hold = false;
    if (!_kb) return;
    _kb->update();
    _activity = _kb->hadActivity();
    if (_power && !_power->isScreenOn()) {
        _head = _count = 0;
        // Preserve activity so the application wakes, but consume its action.
        if (_activity) _kb->discardPending();
        return;
    }
    if (_kb->hadHold()) {
        _head = _count = 0;
        _hold = true;
        return;
    }
    if (_kb->hasEvent()) {
        if (_count < 2) { _pending[(_head + _count) % 2] = _kb->getEvent(); ++_count; }
        else if (_dropped != UINT32_MAX) ++_dropped;
    }
    if (dispatchReady && _count) {
        _event = _pending[_head]; _head = (_head + 1) % 2; --_count;
        _hasKey = _activity = true;
    }
}
