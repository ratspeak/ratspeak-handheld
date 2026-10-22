#include "LvVirtualKeyboard.h"
#include "input/KeyEvent.h"
#include "runtime/LvglApplication.h"
#include "Theme.h"
#include <cstring>

void LvVirtualKeyboard::create() {
    _kb = lv_keyboard_create(lv_layer_top());
    lv_obj_align(_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(_kb, onValueChanged, LV_EVENT_VALUE_CHANGED, this);
    applyTheme();
}

void LvVirtualKeyboard::applyTheme() {
    if (!_kb) return;
    // No LVGL theme engine is active anywhere in this app (every screen
    // styles its own objects directly against Theme::*) — lv_keyboard_create
    // is no exception, so without this it renders fully unstyled. Colors are
    // re-applied here (not just once in create()) because Theme::* values
    // change live when the user switches light/dark in Settings — without a
    // re-apply hook the keyboard would keep whichever scheme was active at
    // boot, going low-contrast or invisible after a later theme switch.
    lv_obj_set_style_bg_color(_kb, lv_color_hex(Theme::BG_SURFACE), 0);
    lv_obj_set_style_border_color(_kb, lv_color_hex(Theme::BORDER), 0);
    lv_obj_set_style_bg_color(_kb, lv_color_hex(Theme::BG_ELEVATED), LV_PART_ITEMS);
    lv_obj_set_style_border_color(_kb, lv_color_hex(Theme::BORDER), LV_PART_ITEMS);
    lv_obj_set_style_text_color(_kb, lv_color_hex(Theme::TEXT_PRIMARY), LV_PART_ITEMS);
    // LV_FONT_DEFAULT is this app's own custom font (lv_font_rsdeck_14),
    // which doesn't carry LVGL's symbol glyph range — Enter/Backspace/etc.
    // render as tofu boxes without an explicit font that does.
    lv_obj_set_style_text_font(_kb, &lv_font_montserrat_16, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(_kb, lv_color_hex(Theme::PRIMARY_MUTED), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(_kb, lv_color_hex(Theme::BG_HOVER), LV_PART_ITEMS | LV_STATE_CHECKED);
}

void LvVirtualKeyboard::setActive(bool wanted) {
    if (!_kb) return;
    // A fresh entry into text-entry mode re-arms auto-show even if the user
    // dismissed it earlier during a previous edit.
    if (wanted && !_wantedPrev) _dismissedByUser = false;
    _wantedPrev = wanted;

    const bool shouldShow = wanted && !_dismissedByUser;
    if (shouldShow == _shown) return;
    _shown = shouldShow;
    if (_shown) lv_obj_clear_flag(_kb, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(_kb, LV_OBJ_FLAG_HIDDEN);
}

void LvVirtualKeyboard::dismiss() {
    _dismissedByUser = true;
    _shown = false;
    lv_obj_add_flag(_kb, LV_OBJ_FLAG_HIDDEN);
}

void LvVirtualKeyboard::onValueChanged(lv_event_t* e) {
    auto* self = static_cast<LvVirtualKeyboard*>(lv_event_get_user_data(e));
    lv_obj_t* kb = lv_event_get_target(e);
    uint16_t btn = lv_keyboard_get_selected_btn(kb);
    if (btn == LV_BTNMATRIX_BTN_NONE) return;
    const char* txt = lv_keyboard_get_btn_text(kb, btn);
    if (!txt) return;

    // Mode-switch keys (abc/ABC/1#) are already handled by LVGL's own
    // default handler (auto-registered by lv_keyboard_create); nothing to
    // synthesize for those.
    if (!strcmp(txt, "abc") || !strcmp(txt, "ABC") || !strcmp(txt, "1#")) return;
    if (!strcmp(txt, LV_SYMBOL_KEYBOARD) || !strcmp(txt, LV_SYMBOL_CLOSE)) {
        self->dismiss();
        return;
    }

    KeyEvent evt{};
    if (!strcmp(txt, LV_SYMBOL_NEW_LINE)) evt.enter = true;
    else if (!strcmp(txt, LV_SYMBOL_BACKSPACE)) evt.del = true;
    else if (!strcmp(txt, LV_SYMBOL_LEFT)) evt.left = true;
    else if (!strcmp(txt, LV_SYMBOL_RIGHT)) evt.right = true;
    else if (!strcmp(txt, " ")) { evt.space = true; evt.character = ' '; }
    else if (strlen(txt) == 1) evt.character = txt[0];
    else return;  // Unmapped multi-byte symbol (e.g. the OK checkmark) — ignore.

    handheld::lvgl_application::injectKeyEvent(evt);
}
