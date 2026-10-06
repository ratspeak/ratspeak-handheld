#include "hal/Keyboard.h"
#include "hal/Expander.h"
#include "config/BoardConfig.h"

namespace {
constexpr uint32_t PollMs = 20;
constexpr uint32_t HoldMs = 700;
}

bool Keyboard::begin() {
    pinMode(USER_BUTTON_PIN, INPUT_PULLUP);
    return wiol2::expander::ready();
}

Keyboard::Gesture Keyboard::track(Button& button, bool down, uint32_t now) {
    if (down && !button.down) {
        button.down = true; button.held = false; button.since = now;
        _activity = true;
    } else if (down && !button.held && now - button.since >= HoldMs) {
        button.held = true;
        return Gesture::Hold;
    } else if (!down && button.down) {
        button.down = false;
        if (!button.held) return Gesture::Tap;
    }
    return Gesture::None;
}

void Keyboard::emit(bool boot, Gesture gesture) {
    if (gesture == Gesture::None) return;
    KeyEvent event{};
    if (_recovery) {
        if (gesture == Gesture::Tap) event.character = boot ? 'x' : 'r';
        else if (boot) event.character = 'b';
        else event.enter = true;
    } else if (gesture == Gesture::Hold) {
        _hold = true;
        return;
    } else if (boot) {
        event.character = 0x1b;
    } else {
        return;  // A top-button tap only wakes or keeps the screen awake.
    }
    _event = event;
    _hasEvent = true;
}

void Keyboard::update() {
    _hasEvent = _hold = _activity = false;
    const uint32_t now = millis();
    if (now - _lastPoll < PollMs) return;
    _lastPoll = now;
    emit(true, track(_boot, digitalRead(USER_BUTTON_PIN) == LOW, now));
    bool high = true;
    if (wiol2::expander::readInput(EXP_WAKE_BUTTON, high) && !_hasEvent)
        emit(false, track(_top, !high, now));
}
