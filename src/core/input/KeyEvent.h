#pragma once

// Shared input contract for the Deck/Pager LVGL frontend. Hardware adapters
// translate their own keys and pointing devices into these semantic events.
enum class InputMode { Navigation, TextInput };
enum class InputSource : unsigned char { Keyboard, Wheel, Trackball };

struct KeyEvent {
    char character = 0;
    bool ctrl = false;
    bool shift = false;
    bool fn = false;
    bool alt = false;
    bool opt = false;
    bool enter = false;
    bool del = false;
    bool tab = false;
    bool space = false;
    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;
    InputSource source = InputSource::Keyboard;
    bool repeat = false;  // Text deletion may repeat; back/dismiss must not.
};

static_assert(sizeof(KeyEvent) == 16, "Deck/Pager semantic input budget");
