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
    // One-shot: true exactly once per dismiss() call, then resets. This
    // class has no reference to whatever screen is currently active (it's
    // generic, driven purely by wantsVirtualKeyboard() each tick), so it
    // can't tell that screen its own focused-textarea flag is now stale --
    // the caller (LvglApplication.cpp's loop) is expected to poll this and
    // forward it via LvScreen::onVirtualKeyboardDismissed(). Without this,
    // a screen whose own "focused" flag only ever clears on the textarea's
    // native LV_EVENT_DEFOCUSED (which dismiss() never triggers, since it
    // only hides this widget -- it never touches LVGL's focus state) gets
    // stuck thinking it still wants keyboard space forever: the resize
    // never reverts, and this keyboard won't re-show either (dismissedByUser
    // latches until that screen's wanted-state drops to false and back).
    bool takeDismissed() { const bool d = _justDismissed; _justDismissed = false; return d; }

private:
    static void onValueChanged(lv_event_t* e);
    void dismiss();

    lv_obj_t* _kb = nullptr;
    bool _shown = false;
    bool _wantedPrev = false;
    bool _dismissedByUser = false;
    bool _justDismissed = false;
};
