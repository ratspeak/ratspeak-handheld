#include "LvInput.h"
#include "Theme.h"
#include "hal/Keyboard.h"
#if HAS_TRACKBALL
#include "hal/Trackball.h"
#elif HAS_SCROLLWHEEL
#include "hal/Scrollwheel.h"
#endif
#include "hal/TouchInput.h"

namespace LvInput {


static Keyboard* s_kb = nullptr;
#if HAS_TRACKBALL
static Trackball* s_tb = nullptr;
#elif HAS_SCROLLWHEEL
static Scrollwheel* s_sw = nullptr;
#endif
static TouchInput* s_touch = nullptr;
static lv_group_t* s_group = nullptr;
static lv_obj_t* s_cursor = nullptr;

// Six compact semantic keys plus two PointerInput confirmations stay within
// the shared eight-event ceiling. Printable text is consumed by the screen.
static uint8_t s_keys[6] = {};
static uint8_t s_head = 0, s_count = 0;
static uint32_t s_lastKey = 0;
static bool s_releasePending = false;
static bool s_enabled = true;
static bool s_waitTouchRelease = false;
static lv_indev_drv_t* s_keyDriver = nullptr;
static lv_indev_t* s_touchIndev = nullptr;

// Touch cursor auto-hide after 1s of no touch
static unsigned long s_lastTouchMs = 0;
static bool s_cursorVisible = false;

// Suppress touch for 300ms after pointer/keyboard use to prevent accidental taps
static unsigned long s_lastKeyMs = 0;
static constexpr unsigned long TOUCH_SUPPRESS_MS = 300;

// Focus ring suppressed while wheel-navbar (tab-cycling) mode is active
static bool s_focusSuppressed = false;

static void clearFocusVisual(lv_obj_t* obj) {
    if (obj) lv_obj_clear_state(obj, LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY);
}

// Runs after LV_EVENT_FOCUSED on the group add/remove/next/prev paths, so
// auto-refocus during screen builds/rebuilds can't paint a ring while suppressed.
static void group_focus_cb(lv_group_t* g) {
    if (s_focusSuppressed) clearFocusVisual(lv_group_get_focused(g));
}

static void touchpad_read_cb(lv_indev_drv_t *indev_driver, lv_indev_data_t *data) {
    if (!s_enabled || !s_touch || !s_cursor) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    // Ignore touch input briefly after pointer/keyboard use
    if (millis() - s_lastKeyMs < TOUCH_SUPPRESS_MS) {
        if (s_touch->isTouched()) s_waitTouchRelease = true;
        data->state = LV_INDEV_STATE_REL;
        return;
    }
    if (s_waitTouchRelease) {
        if (!s_touch->isTouched()) s_waitTouchRelease = false;
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    if (s_touch->isTouched()) {
        lv_obj_clear_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
        s_cursorVisible = true;
        s_lastTouchMs = millis();
        data->state = LV_INDEV_STATE_PR;
        data->point.x = s_touch->x();
        data->point.y = s_touch->y();
    } else {
        data->state = LV_INDEV_STATE_REL;
        if (s_cursorVisible && millis() - s_lastTouchMs > 1000) {
            lv_obj_add_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
            s_cursorVisible = false;
        }
    }
}

static void keypad_read_cb(lv_indev_drv_t*, lv_indev_data_t* data) {
    data->key = s_lastKey;
    data->state = LV_INDEV_STATE_RELEASED;
    if (!s_enabled) return;
    if (s_releasePending) {
        s_releasePending = false;
        data->continue_reading = s_count != 0;
    } else if (s_count) {
        s_lastKey = s_keys[s_head];
        s_head = (s_head + 1) % sizeof(s_keys);
        --s_count;
        data->key = s_lastKey;
        data->state = LV_INDEV_STATE_PRESSED;
        s_releasePending = true;
        // Always emit release before the next semantic press, including two
        // identical keys queued before LVGL gets its next timer interval.
        data->continue_reading = true;
    }
}

#if HAS_TRACKBALL
void init(Keyboard* kb, Trackball* tb, TouchInput* touch) {
    s_kb = kb;
    s_tb = tb;
#elif HAS_SCROLLWHEEL
void init(Keyboard* kb, Scrollwheel* sw, TouchInput* touch) {
    s_kb = kb;
    s_sw = sw;
#else
void init(Keyboard* kb, TouchInput* touch) {
    s_kb = kb;
#endif
    s_touch = touch;
    s_touchIndev = nullptr;
    s_head = s_count = 0;
    s_releasePending = false;
    s_enabled = true;
    s_waitTouchRelease = false;

    // Create input group
    s_group = lv_group_create();
    lv_group_set_default(s_group);
    lv_group_set_focus_cb(s_group, group_focus_cb);

    // Register keypad indev
    static lv_indev_drv_t keyDrv;
    lv_indev_drv_init(&keyDrv);
    keyDrv.type = LV_INDEV_TYPE_KEYPAD;
    keyDrv.read_cb = keypad_read_cb;
    lv_indev_t* keyIndev = lv_indev_drv_register(&keyDrv);
    s_keyDriver = &keyDrv;
    lv_indev_set_group(keyIndev, s_group);

    if (touch) {
        // Register a touchscreen input device
        static lv_indev_drv_t touchDrv;
        lv_indev_drv_init(&touchDrv);
        touchDrv.type = LV_INDEV_TYPE_POINTER;
        touchDrv.read_cb = touchpad_read_cb;
        lv_indev_t* touchIndev = lv_indev_drv_register(&touchDrv);
        s_touchIndev = touchIndev;
        lv_indev_set_group(touchIndev, s_group);

        // Touch cursor: semi-transparent center with brand green ring
        s_cursor = lv_obj_create(lv_layer_sys());
        lv_obj_set_size(s_cursor, 14, 14);
        lv_obj_set_style_radius(s_cursor, 7, 0);
        lv_obj_set_style_bg_color(s_cursor, lv_color_hex(Theme::BG), 0);
        lv_obj_set_style_bg_opa(s_cursor, LV_OPA_60, 0);
        lv_obj_set_style_outline_color(s_cursor, lv_color_hex(Theme::PRIMARY), 0);
        lv_obj_set_style_outline_width(s_cursor, 1, 0);
        lv_obj_set_style_outline_opa(s_cursor, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_cursor, 0, 0);
        lv_obj_clear_flag(s_cursor, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        // Center the cursor on the touch point (LVGL positions top-left by default)
        lv_obj_set_style_translate_x(s_cursor, -7, 0);
        lv_obj_set_style_translate_y(s_cursor, -7, 0);
        lv_indev_set_cursor(touchIndev, s_cursor);
        lv_obj_add_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);

        Serial.println("[LVGL] Input drivers registered (keypad + touch)");
    } else {
        s_cursor = nullptr;
        Serial.println("[LVGL] Input drivers registered (keypad only)");
    }
}

void applyTheme() {
    if (!s_cursor) return;
    lv_obj_set_style_bg_color(s_cursor, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_outline_color(s_cursor, lv_color_hex(Theme::PRIMARY), 0);
}

bool canAcceptKey() { return s_count < sizeof(s_keys); }

static void cancelTouchPress() {
    if (!s_touchIndev) return;
    // LVGL 8.3 retains its pressed object even while the application skips
    // timers during sleep. A synthetic REL would otherwise click that object.
    // Clear the visual press as well as the indev's retained target, without
    // sending RELEASED/CLICKED events to application action callbacks.
    lv_obj_t* pressed = s_touchIndev->proc.types.pointer.act_obj;
    if (pressed) lv_obj_clear_state(pressed, LV_STATE_PRESSED);
    lv_indev_reset(s_touchIndev, nullptr);
}

void noteKeyActivity() {
    cancelTouchPress();
    s_lastKeyMs = millis();
    if (s_touch && s_touch->isTouched()) s_waitTouchRelease = true;
    if (s_cursor) lv_obj_add_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
}

void setEnabled(bool enabled) {
    if (s_enabled == enabled) return;
    s_enabled = enabled;
    if (!enabled) cancelTouchPress();
    s_head = s_count = 0;
    s_releasePending = false;
    s_waitTouchRelease = true;
    if (s_cursor) lv_obj_add_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
}

bool feedKey(const KeyEvent& evt) {
    // Repeated deletion belongs only to a text editor that consumes it. It
    // cannot become LVGL Back/activate/navigation after that editor exits.
    if (!s_enabled || evt.repeat) return true;
    uint32_t key = 0;

    if (evt.enter) key = LV_KEY_ENTER;
    else if (evt.up) key = LV_KEY_PREV;
    else if (evt.down) key = LV_KEY_NEXT;
    else if (evt.left) key = LV_KEY_LEFT;
    else if (evt.right) key = LV_KEY_RIGHT;
    else if (evt.del) key = LV_KEY_BACKSPACE;
    else if (evt.tab) key = LV_KEY_NEXT;
    else if (evt.character == 0x1B) key = LV_KEY_ESC;
    else return true;  // Screens handle printable characters.

    if (!canAcceptKey()) return false;
    noteKeyActivity();
    s_keys[(s_head + s_count) % sizeof(s_keys)] = key;
    ++s_count;
    // The board loop already runs lv_timer_handler on input activity. Make
    // this read due now so an admitted key precedes the next screen action.
    if (s_keyDriver && s_keyDriver->read_timer) lv_timer_ready(s_keyDriver->read_timer);
    return true;
}

lv_group_t* group() {
    return s_group;
}

void setFocusSuppressed(bool suppressed) {
    s_focusSuppressed = suppressed;
    if (!s_group) return;
    lv_obj_t* focused = lv_group_get_focused(s_group);
    if (!focused) return;
    if (suppressed) {
        clearFocusVisual(focused);
    } else {
        // Re-sends LV_EVENT_FOCUSED: restores ring state and scroll-into-view
        lv_group_focus_obj(focused);
    }
}

bool focusSuppressed() {
    return s_focusSuppressed;
}

void focusObj(lv_obj_t* obj) {
    if (!obj) return;
    // lv_group_focus_obj runs focus_cb before the FOCUSED event, so the ring
    // state must be cleared here, after the event.
    lv_group_focus_obj(obj);
    if (s_focusSuppressed) clearFocusVisual(obj);
}

}  // namespace LvInput
