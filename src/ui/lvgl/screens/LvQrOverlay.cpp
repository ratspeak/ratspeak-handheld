#include "LvQrOverlay.h"
#include "Theme.h"
#include "LvTheme.h"
#include "LvInput.h"
#include "util/ContactCard.h"
#include "fonts/fonts.h"

void LvQrOverlay::create() {
    if (_overlay) return;
    _group = lv_group_create();
    _overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(_overlay, Theme::SCREEN_W, Theme::SCREEN_H);
    lv_obj_set_pos(_overlay, 0, 0);
    lv_obj_add_style(_overlay, LvTheme::styleScreen(), 0);
    lv_obj_set_style_border_width(_overlay, 0, 0);
    lv_obj_set_style_pad_all(_overlay, 0, 0);
    lv_obj_clear_flag(_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(_overlay);
    lv_obj_add_style(title, LvTheme::styleLabelAccent(), 0);
    lv_obj_set_style_text_font(title, &lv_font_rsdeck_14, 0);
    lv_label_set_text(title, "Scan to add me");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    // Landscape layout leaves room for real controls on both release screens.
    // Keep at least four white modules around the unbranded QR code.
    constexpr int qrSize = Theme::SCREEN_H - 64;
    auto* paper = lv_obj_create(_overlay);
    lv_obj_set_pos(paper, 8, 32);
    lv_obj_set_size(paper, qrSize + 24, qrSize + 24);
    lv_obj_set_style_bg_color(paper, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(paper, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(paper, 0, 0);
    lv_obj_set_style_radius(paper, 0, 0);
    lv_obj_set_style_pad_all(paper, 0, 0);
    lv_obj_clear_flag(paper, LV_OBJ_FLAG_SCROLLABLE);
    _qr = lv_qrcode_create(paper, qrSize, lv_color_hex(0), lv_color_hex(0xFFFFFF));
    _qrSize = qrSize;
    lv_obj_center(_qr);

    constexpr int controlsX = qrSize + 40;
    constexpr int controlsW = Theme::SCREEN_W - controlsX - 8;
    _lblFormat = lv_label_create(_overlay);
    lv_obj_add_style(_lblFormat, LvTheme::styleLabelAccent(), 0);
    lv_obj_set_style_text_font(_lblFormat, &lv_font_rsdeck_12, 0);
    lv_obj_set_pos(_lblFormat, controlsX, 38);
    _lblAddr = lv_label_create(_overlay);
    lv_obj_add_style(_lblAddr, LvTheme::styleLabelMuted(), 0);
    lv_obj_set_style_text_font(_lblAddr, &lv_font_rsdeck_10, 0);
    lv_obj_set_width(_lblAddr, controlsW);
    lv_obj_set_pos(_lblAddr, controlsX, 60);

    auto button = [&](const char* text, int y, lv_obj_t** label) {
        auto* btn = lv_btn_create(_overlay);
        lv_obj_set_pos(btn, controlsX, y);
        lv_obj_set_size(btn, controlsW, 32);
        lv_obj_add_style(btn, LvTheme::styleBtn(), 0);
        lv_obj_add_style(btn, LvTheme::styleBtnFocused(), LV_STATE_FOCUSED);
        lv_obj_add_style(btn, LvTheme::styleBtnPressed(), LV_STATE_PRESSED);
        lv_group_add_obj(_group, btn);
        auto* lbl = lv_label_create(btn);
        lv_obj_set_style_text_font(lbl, &lv_font_rsdeck_12, 0);
        lv_label_set_text(lbl, text);
        lv_obj_center(lbl);
        if (label) *label = lbl;
        return btn;
    };
    _toggle = button("Legacy", Theme::SCREEN_H - 90, &_lblToggle);
    lv_obj_add_event_cb(_toggle, [](lv_event_t* e) {
        static_cast<LvQrOverlay*>(lv_event_get_user_data(e))->toggleFormat();
    }, LV_EVENT_CLICKED, this);
    auto* close = button("Close", Theme::SCREEN_H - 48, nullptr);
    lv_obj_add_event_cb(close, [](lv_event_t* e) {
        static_cast<LvQrOverlay*>(lv_event_get_user_data(e))->hide();
    }, LV_EVENT_CLICKED, this);
    lv_obj_add_flag(_overlay, LV_OBJ_FLAG_HIDDEN);
}

bool LvQrOverlay::renderCode(bool legacy) {
    const auto& payload = legacy ? _legacyPayload : _payload;
    // Address-only codes have larger modules. Give them a wider white quiet
    // zone without shrinking the denser rich contact card.
    const int size = Theme::SCREEN_H - (legacy ? 88 : 64);
    if (_qrSize != size) {
        auto* paper = lv_obj_get_parent(_qr);
        lv_qrcode_delete(_qr);
        _qr = lv_qrcode_create(paper, size, lv_color_hex(0), lv_color_hex(0xFFFFFF));
        _qrSize = size;
        lv_obj_center(_qr);
    }
    if (lv_qrcode_update(_qr, payload.data(), payload.size()) != LV_RES_OK) return false;
    _legacy = legacy;
    lv_label_set_text(_lblFormat, legacy ? "Legacy" : "Ratspeak");
    lv_label_set_text(_lblToggle, legacy ? "Ratspeak" : "Legacy");
    return true;
}

bool LvQrOverlay::show(const String& name, const String& destHashHex,
                       const String& identityHashHex, const String& publicKeyHex) {
    hide();
    if (!_overlay) create();
    _payload = rs::contactCardPayload(name.c_str(), destHashHex.c_str(),
                                      identityHashHex.c_str(), publicKeyHex.c_str());
    if (_payload.empty()) return false;
    _legacyPayload = "lxma://" + std::string(destHashHex.c_str());
    if (!renderCode(false)) return false;
    lv_label_set_text_fmt(_lblAddr, "%.8s\n%.8s\n%.8s\n%.8s", destHashHex.c_str(),
                         destHashHex.c_str() + 8, destHashHex.c_str() + 16, destHashHex.c_str() + 24);
    lv_obj_clear_flag(_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_overlay);
    // Preserve the underlying screen's group and focus while wheel/keypad
    // events belong exclusively to this overlay.
    for (auto* input = lv_indev_get_next(nullptr); input; input = lv_indev_get_next(input)) {
        if (input->group == LvInput::group()) lv_indev_set_group(input, _group);
    }
    lv_group_set_editing(_group, false);
    lv_group_focus_obj(_toggle);
    _visible = true;
    return true;
}

void LvQrOverlay::hide() {
    if (_overlay) lv_obj_add_flag(_overlay, LV_OBJ_FLAG_HIDDEN);
    if (_group) {
        for (auto* input = lv_indev_get_next(nullptr); input; input = lv_indev_get_next(input)) {
            if (input->group == _group) lv_indev_set_group(input, LvInput::group());
        }
    }
    _visible = false;
}

void LvQrOverlay::toggleFormat() {
    if (!renderCode(!_legacy)) hide();
}

bool LvQrOverlay::handleKey(const KeyEvent& event) {
    if (!_visible) return false;
    if (event.repeat) return true;
    if (event.up || event.left) lv_group_focus_prev(_group);
    else if (event.down || event.right) lv_group_focus_next(_group);
    else if (event.enter || event.character == '\n' || event.character == '\r') {
        auto* focused = lv_group_get_focused(_group);
        if (focused) lv_event_send(focused, LV_EVENT_CLICKED, nullptr);
    } else if (event.character == 'l' || event.character == 'L') toggleFormat();
    else hide();
    return true;
}
