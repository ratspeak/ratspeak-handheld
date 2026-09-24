#pragma once

#include <lvgl.h>
#include "config/BoardConfig.h"

class Keyboard;
class Trackball;
class Scrollwheel;
class TouchInput;
struct KeyEvent;

// LVGL input device drivers — board pointer device selected via BoardConfig
// (HAS_TRACKBALL / HAS_SCROLLWHEEL); touch may be nullptr on boards without it.
namespace LvInput {

#if HAS_TRACKBALL
void init(Keyboard* kb, Trackball* tb, TouchInput* touch);
#elif HAS_SCROLLWHEEL
void init(Keyboard* kb, Scrollwheel* sw, TouchInput* touch);
#else
void init(Keyboard* kb, TouchInput* touch = nullptr);
#endif

// Feed a KeyEvent into the LVGL keypad indev (called from main loop)
bool feedKey(const KeyEvent& evt);
bool canAcceptKey();
// Call for every dispatched key, including keys handled directly by a screen.
void noteKeyActivity();
// Clear stale renderer events and reject a touch held across display-off.
void setEnabled(bool enabled);

// Get the LVGL input group (for focusing widgets)
lv_group_t* group();

// Navbar (tab-cycling) mode: hide the in-content focus ring. Group focus stays
// logical so entering the screen restores the ring on the last-focused widget.
void setFocusSuppressed(bool suppressed);
bool focusSuppressed();

// Focus a widget through the suppression filter — use instead of
// lv_group_focus_obj() for a screen's initial focus placement.
void focusObj(lv_obj_t* obj);

// Re-apply palette colors to the touch cursor after a theme switch
void applyTheme();

}  // namespace LvInput
