#pragma once
#include "hal/Keyboard.h"
#include "hal/Power.h"
#include "hal/TouchInput.h"

// No physical keyboard or encoder on this board — keyboard stays a no-op
// stub (never produces a KeyEvent). Touch only drives wake/activity
// tracking here; actual tap dispatch goes through LVGL's native pointer
// indev (see LvInput::init), and long-press-to-delete uses LVGL's native
// LV_EVENT_LONG_PRESSED directly on screen rows, not this KeyEvent path.
// The one physical button (GPIO38, PIN_USER_BTN) is dual-purpose: a tap
// wakes (via hadActivity()/hadStrongActivity(), same as touch); a ~1.2s
// hold reports hadLongPress() — same threshold and "a wake hold must not
// also sleep" guard as PointerInput's trackball/encoder hold — which the
// main loop's dispatchLongPress() turns into forceScreenOff() when no
// screen consumes it, same as every other board's hold-to-sleep gesture.
class InputManager {
public:
    void begin(Keyboard* kb, TouchInput* touch) { _kb = kb; _touch = touch; pinMode(PIN_USER_BTN, INPUT_PULLUP); }
    void setPowerMgr(Power* power) { _power = power; }
    void update(bool dispatchReady = true);
    bool hasKeyEvent() const { return false; }
    const KeyEvent& getKeyEvent() const { return _event; }
    bool hadActivity() const { return _activity; }
    bool hadStrongActivity() const { return _activity; }
    bool hadLongPress() const { return _longPress; }
private:
    Keyboard* _kb = nullptr;
    TouchInput* _touch = nullptr;
    Power* _power = nullptr;
    KeyEvent _event{};
    bool _activity = false;
    bool _longPress = false;
    bool _btnWasDown = false;
    bool _btnFired = false;
    bool _btnFromScreenOn = false;
    uint32_t _lastTouchPoll = 0;
    uint32_t _lastBtnChange = 0;
    uint32_t _btnPressStart = 0;
};
