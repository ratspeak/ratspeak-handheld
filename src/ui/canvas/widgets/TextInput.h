#pragma once

#include <M5GFX.h>
#include <string>
#include <cstdint>
#include "hal/Keyboard.h"
#include "Theme.h"

class TextInput {
public:
    void render(M5Canvas& canvas, int x, int y, int w);

    // Handle key event. Returns true if consumed.
    bool handleKey(const KeyEvent& event);

    // Content access
    const std::string& getText() const { return _text; }
    uint64_t revision() const { return _revision; }
    bool setText(const std::string& text);
    bool setText(const char* text, size_t length);
    bool appendText(const char* text, size_t length);
    void clear();
    void clearSensitive() {
        volatile char* bytes = _text.empty() ? nullptr : &_text[0];
        for (size_t n = 0; n < _text.size(); ++n) bytes[n] = 0;
        clear();
    }
    // Reserve before an owner admits work that requires retaining this text.
    // Existing field editors keep their ordinary length policy.
    bool reserveTextCapacity(size_t capacity);
    size_t textCapacity() const { return _text.capacity(); }

    // State
    bool isActive() const { return _active; }
    void setActive(bool active) { _active = active; }
    void setMaxLength(int len) { _maxLength = len; }
    void setNumericOnly(bool numeric) { _numericOnly = numeric; }

    // Callback when Enter is pressed
    using SubmitCallback = std::function<void(const std::string&)>;
    void setSubmitCallback(SubmitCallback cb) { _submitCb = cb; }

private:
    std::string _text;
    size_t _capacityLimit = 0; // Zero leaves ordinary settings editors unchanged.
    uint64_t _revision = 1;
    int _cursorPos = 0;
    bool _active = false;
    bool _cursorVisible = true;
    unsigned long _lastBlink = 0;
    int _maxLength = 200;
    bool _numericOnly = false;
    SubmitCallback _submitCb;
};
