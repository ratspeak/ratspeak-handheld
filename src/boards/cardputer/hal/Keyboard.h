#pragma once

#include <Arduino.h>
#include <M5Cardputer.h>
#include <functional>
#include "platform/CardputerAdvKeyboard.h"

// Input modes
enum class InputMode {
    Navigation,  // Arrow-like movement, hotkeys active
    TextInput    // Character entry, Esc exits to Navigation
};

// Simplified key event for consumers
struct KeyEvent {
    char character = 0;  // ASCII value (0 if this is a special key)
    bool ctrl = false;
    bool shift = false;
    bool fn = false;
    bool alt = false;
    bool opt = false;
    bool enter = false;
    bool backspace = false;
    bool forwardDelete = false;
    bool tab = false;
    bool space = false;
    bool escape = false;
    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;

    // The punctuation keys carry the printed arrows on the Cardputer. Bare
    // punctuation remains a convenient navigation alias outside text fields;
    // the Keyboard adapter turns the Fn layer into the semantic flags above.
    bool navUp() const { return up || character == ';'; }
    bool navDown() const { return down || character == '.'; }
    bool navLeft() const { return left || character == ','; }
    bool navRight() const { return right || character == '/'; }
    bool navPrevious() const { return navUp() || navLeft(); }
    bool navNext() const { return navDown() || navRight(); }
};

class Keyboard {
public:
    void begin();
    void update();

    // Mode control
    InputMode getMode() const { return _mode; }
    void setMode(InputMode mode) { _mode = mode; }

    // State queries
    bool hasEvent() const { return _hasEvent; }
    const KeyEvent& getEvent() const { return _event; }

    // For text input mode
    bool capsLocked() const;
    void setCapsLocked(bool locked);

    // Callback for key events (called after hotkey processing)
    using KeyCallback = std::function<void(const KeyEvent&)>;
    void setKeyCallback(KeyCallback cb) { _keyCallback = cb; }

private:
    KeyEvent eventForKey(uint8_t row, uint8_t col) const;
    KeyEvent currentEvent() const;

    InputMode _mode = InputMode::Navigation;
    KeyEvent _event = {};
    bool _hasEvent = false;
    CardputerAdvKeyboard _hardware;
    bool _pressed[4][14] = {};
    bool _capsLocked = false;
    bool _keyHeld = false;
    KeyEvent _heldEvent = {};
    unsigned long _heldSince = 0;
    unsigned long _lastRepeat = 0;
    KeyCallback _keyCallback;
};
