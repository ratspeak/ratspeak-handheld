#pragma once

#include "UIManager.h"
#include "Theme.h"

// A notice above every screen until a fresh key or a tap closes it.
class LvNoticeOverlay {
public:
    void show(const char* title, const char* text, void (*dismissed)() = nullptr);
    void hide();
    bool isVisible() const { return _visible; }
    bool handleKey(const KeyEvent& event);

private:
    lv_obj_t* _overlay = nullptr;
    void (*_dismissed)() = nullptr;
    bool _visible = false;
};
