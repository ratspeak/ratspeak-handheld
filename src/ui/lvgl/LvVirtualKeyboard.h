#pragma once

#include <lvgl.h>

// On-screen keyboard for touch-only boards (no physical keyboard). Renders
// LVGL's stock lv_keyboard purely as a layout widget — it is never bound to
// a textarea, so its own character-insertion branches are safe no-ops; only
// its mode-switching (abc/ABC/1#) and OK/Close handling run automatically.
// Each button press is instead translated into the same KeyEvent a physical
// keypress would produce and run through LvglApplication's dispatchKey(),
// so every existing screen's handleKey() handles it identically to real
// typing — no screen's internal text storage is touched by this class.
class LvVirtualKeyboard {
public:
    void create();
    // Re-applies styling against the current Theme::* values — call after
    // any light/dark scheme switch (see UIManager::applyTheme()'s callers).
    void applyTheme();
    // Called once per loop tick with the active screen's
    // LvScreen::wantsVirtualKeyboard(). Auto-shows on a false->true edge;
    // stays hidden if the user dismissed it via the keyboard's own close
    // button, until the screen re-enters text-entry mode.
    void setActive(bool wanted);

private:
    static void onValueChanged(lv_event_t* e);
    void dismiss();

    lv_obj_t* _kb = nullptr;
    bool _shown = false;
    bool _wantedPrev = false;
    bool _dismissedByUser = false;
};
