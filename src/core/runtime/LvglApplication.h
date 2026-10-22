#pragma once

struct KeyEvent;

namespace handheld::lvgl_application {
void setup();
void loop();
// Routes a synthesized KeyEvent (e.g. from an on-screen keyboard) through
// the exact same screen/overlay/hotkey/LVGL arbitration a physical or USB
// key already goes through — see dispatchKey() in the .cpp.
void injectKeyEvent(const KeyEvent& evt);
}
