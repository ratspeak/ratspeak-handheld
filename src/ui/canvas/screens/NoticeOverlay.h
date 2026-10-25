#pragma once

#include "Screen.h"

// A notice above the current screen until a fresh key closes it.
class NoticeOverlay : public Screen {
public:
    void show(const char* heading, const char* text, void (*dismissed)() = nullptr) {
        _heading = heading; _text = text; _dismissed = dismissed; _visible = true;
    }
    void render(M5Canvas& canvas) override;
    bool handleKey(const KeyEvent& event) override;
    const char* title() const override { return _heading; }

    bool isVisible() const { return _visible; }

private:
    const char* _heading = "";
    const char* _text = "";
    void (*_dismissed)() = nullptr;
    bool _visible = false;
};
