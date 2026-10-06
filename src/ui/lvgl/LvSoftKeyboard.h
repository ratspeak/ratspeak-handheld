#pragma once

#include "config/BoardConfig.h"
#include "input/KeyEvent.h"

class LvScreen;

// On-screen keyboard for boards without physical keys (HAS_SOFT_KEYBOARD).
// It produces the same KeyEvents a hardware keyboard would, so screens keep a
// single input path. It appears while the active screen's textInput() asks
// for it and pans that screen's anchor above itself.
namespace LvSoftKeyboard {

void init();
// Call once per UI loop before lv_timer_handler().
void update(LvScreen* screen);
// Re-show after the user hid it; screens call this when their editor is tapped.
void show();
bool take(KeyEvent& event);
bool visible();

}  // namespace LvSoftKeyboard
