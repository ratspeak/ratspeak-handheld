#include "LvNoticeOverlay.h"
#include "LvTheme.h"
#include "config/BoardConfig.h"
#include <lvgl.h>
#include "fonts/fonts.h"

void LvNoticeOverlay::show(const char* title, const char* text, void (*dismissed)()) {
    if (_overlay) lv_obj_del(_overlay);
    _dismissed = dismissed;
    _overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(_overlay, Theme::SCREEN_W - 32, LV_SIZE_CONTENT);
    lv_obj_center(_overlay);
    lv_obj_add_style(_overlay, LvTheme::styleModal(), 0);
    lv_obj_set_style_pad_all(_overlay, 10, 0);
    lv_obj_set_style_pad_row(_overlay, 6, 0);
    lv_obj_set_layout(_overlay, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_overlay, [](lv_event_t* e) {
        static_cast<LvNoticeOverlay*>(lv_event_get_user_data(e))->hide();
    }, LV_EVENT_CLICKED, this);
    auto label = [this](const char* value, const lv_font_t* font, uint32_t color) {
        lv_obj_t* item = lv_label_create(_overlay);
        lv_obj_set_width(item, lv_pct(100));
        lv_label_set_long_mode(item, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(item, font, 0);
        lv_obj_set_style_text_color(item, lv_color_hex(color), 0);
        lv_label_set_text(item, value);
        return item;
    };
    label(title, &lv_font_rsdeck_12, Theme::ACCENT);
    label(text, &lv_font_rsdeck_12, Theme::TEXT_PRIMARY);
#if HAS_TOUCH
    lv_obj_t* footer = label("Any key or tap closes", &lv_font_rsdeck_10, Theme::TEXT_MUTED);
#else
    lv_obj_t* footer = label("Any key closes", &lv_font_rsdeck_10, Theme::TEXT_MUTED);
#endif
    lv_obj_set_style_text_align(footer, LV_TEXT_ALIGN_CENTER, 0);
    _visible = true;
}

void LvNoticeOverlay::hide() {
    if (!_visible) return;
    // A tap closes the notice from its own event; deletion waits for LVGL.
    lv_obj_del_async(_overlay); _overlay = nullptr;
    _visible = false;
    if (_dismissed) _dismissed();
}

bool LvNoticeOverlay::handleKey(const KeyEvent& event) {
    if (!_visible) return false;
    if (!event.repeat) hide();
    return true;
}
