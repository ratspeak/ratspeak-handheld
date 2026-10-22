#include "LvQrOverlay.h"
#include "Theme.h"
#include "LvTheme.h"
#include "LvInput.h"
#include "util/ContactCard.h"
#include "fonts/fonts.h"

namespace {
// Square/tall panels (SenseCAP D1L, 480x480) can't use the landscape
// layout below at all: qrSize = SCREEN_H - 64 assumes a short screen, and
// on a 480-tall one that's a 416px QR leaving almost no width for the
// side controls column (confirmed on real hardware: single characters
// wrapping one per line). Controls go in their own row below the QR
// instead of beside it.
constexpr bool kTall = Theme::SCREEN_H >= 400 && Theme::SCREEN_W >= 400;
// Base (unzoomed) QR size for tall mode — recomputed to exactly fit above
// the format/address/identity text and button row below it. Shared between
// create() and renderCode() deliberately: renderCode() used to compute its
// own, unrelated size (SCREEN_H - 64/88, ~400px) independent of what
// create() sized the white "paper" backing to (~320px) — the oversized QR
// silently overflowed and got clipped by its smaller parent, cutting the
// quiet zone and sometimes finder patterns. Confirmed on real hardware as
// the actual cause of "neither the in-app scanner nor the phone's own
// camera QR reader can read this" — the code still *looked* like a valid
// QR in photos, just wasn't one anymore once clipped.
constexpr int kTallQrSize = 320;
// Full-screen zoom target (toggleZoom()) — as big as the screen allows.
constexpr int kTallQrZoomSize = 440;
}  // namespace

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

    _title = lv_label_create(_overlay);
    lv_obj_add_style(_title, LvTheme::styleLabelAccent(), 0);
    lv_obj_set_style_text_font(_title, &lv_font_rsdeck_14, 0);
    lv_label_set_text(_title, "Scan to add me");
    lv_obj_align(_title, LV_ALIGN_TOP_MID, 0, 6);

    // Landscape layout leaves room for real controls on both release screens.
    // Keep at least four white modules around the unbranded QR code.
    constexpr int qrSize = kTall ? kTallQrSize : (Theme::SCREEN_H - 64);
    const int paperSize = qrSize + 24;
    _paper = lv_obj_create(_overlay);
    lv_obj_set_pos(_paper, kTall ? (Theme::SCREEN_W - paperSize) / 2 : 8, 32);
    lv_obj_set_size(_paper, paperSize, paperSize);
    lv_obj_set_style_bg_color(_paper, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(_paper, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_paper, 0, 0);
    lv_obj_set_style_radius(_paper, 0, 0);
    lv_obj_set_style_pad_all(_paper, 0, 0);
    lv_obj_clear_flag(_paper, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_paper, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_paper, [](lv_event_t* e) {
        auto* self = static_cast<LvQrOverlay*>(lv_event_get_user_data(e));
        // Tall: tap zooms the QR to full screen for easier scanning (Close
        // is fully visible/reachable now, so this gesture doesn't need to
        // double as a dismiss). Other boards keep the original tap-to-close
        // — their QR was never cramped/unreachable the way D1L's was.
        if (kTall) self->toggleZoom();
        else self->hide();
    }, LV_EVENT_CLICKED, this);
    _qr = lv_qrcode_create(_paper, qrSize, lv_color_hex(0), lv_color_hex(0xFFFFFF));
    _qrSize = qrSize;
    lv_obj_center(_qr);

    constexpr int controlsX = kTall ? 8 : (qrSize + 40);
    constexpr int controlsW = kTall ? (Theme::SCREEN_W - 16) : (Theme::SCREEN_W - controlsX - 8);
    const int belowQrY = 32 + paperSize + 6;
    _lblFormat = lv_label_create(_overlay);
    lv_obj_add_style(_lblFormat, LvTheme::styleLabelAccent(), 0);
    lv_obj_set_style_text_font(_lblFormat, &lv_font_rsdeck_12, 0);
    lv_obj_set_pos(_lblFormat, controlsX, kTall ? belowQrY : 38);
    _lblAddr = lv_label_create(_overlay);
    lv_obj_add_style(_lblAddr, LvTheme::styleLabelMuted(), 0);
    // Muted gray reads fine at a glance in the narrow non-tall column, but
    // against the tall layout's larger dark area it's too low-contrast.
    // styleLabel() (bound to TEXT_PRIMARY) instead of a hardcoded hex value
    // here deliberately — LvTheme::refresh() mutates that shared style
    // in-place on a light/dark switch, so this stays correct afterward;
    // a direct lv_obj_set_style_text_color() call would freeze whatever
    // scheme was active at this one-time create() and never update again
    // (confirmed on real hardware: unreadable after switching to light).
    if (kTall) lv_obj_add_style(_lblAddr, LvTheme::styleLabel(), 0);
    lv_obj_set_style_text_font(_lblAddr, &lv_font_rsdeck_10, 0);
    lv_obj_set_width(_lblAddr, controlsW);
    lv_obj_set_pos(_lblAddr, controlsX, kTall ? (belowQrY + 16) : 60);

    if (kTall) {
        // Room to spare below the QR — show the full identity hash too,
        // not just the LXMF address (identityHashHex() is already passed
        // into show(), just never displayed before).
        _lblIdentity = lv_label_create(_overlay);
        lv_obj_add_style(_lblIdentity, LvTheme::styleLabel(), 0);  // reactive — see above
        lv_obj_set_style_text_font(_lblIdentity, &lv_font_rsdeck_10, 0);
        lv_obj_set_width(_lblIdentity, controlsW);
        lv_obj_set_pos(_lblIdentity, controlsX, belowQrY + 32);
    }

    auto button = [&](const char* text, int x, int y, int w, lv_obj_t** label) {
        auto* btn = lv_btn_create(_overlay);
        lv_obj_set_pos(btn, x, y);
        lv_obj_set_size(btn, w, 32);
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
    if (kTall) {
        const int btnY = belowQrY + 16 + 16 + 16 + 10;  // format + LXMF line + ID line + gap
        const int btnW = (controlsW - 8) / 2;
        _toggle = button("Legacy", controlsX, btnY, btnW, &_lblToggle);
        _closeBtn = button("Close", controlsX + btnW + 8, btnY, btnW, nullptr);
    } else {
        _toggle = button("Legacy", controlsX, Theme::SCREEN_H - 90, controlsW, &_lblToggle);
        _closeBtn = button("Close", controlsX, Theme::SCREEN_H - 48, controlsW, nullptr);
    }
    lv_obj_add_event_cb(_closeBtn, [](lv_event_t* e) {
        static_cast<LvQrOverlay*>(lv_event_get_user_data(e))->hide();
    }, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(_toggle, [](lv_event_t* e) {
        static_cast<LvQrOverlay*>(lv_event_get_user_data(e))->toggleFormat();
    }, LV_EVENT_CLICKED, this);
    lv_obj_add_flag(_overlay, LV_OBJ_FLAG_HIDDEN);
}

void LvQrOverlay::resizeQr(int size) {
    if (_qrSize == size) return;
    lv_qrcode_delete(_qr);
    _qr = lv_qrcode_create(_paper, size, lv_color_hex(0), lv_color_hex(0xFFFFFF));
    _qrSize = size;
    lv_obj_center(_qr);
}

bool LvQrOverlay::renderCode(bool legacy) {
    const auto& payload = legacy ? _legacyPayload : _payload;
    // Address-only codes have larger modules. Give them a wider white quiet
    // zone without shrinking the denser rich contact card. (Tall mode uses
    // one consistent size for both — the gap was never worth the added
    // branching once it has its own layout anyway.)
    const int size = _zoomed ? kTallQrZoomSize
                    : kTall   ? kTallQrSize
                              : (Theme::SCREEN_H - (legacy ? 88 : 64));
    resizeQr(size);
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
    _zoomed = false;
    _payload = rs::contactCardPayload(name.c_str(), destHashHex.c_str(),
                                      identityHashHex.c_str(), publicKeyHex.c_str());
    if (_payload.empty()) return false;
    // Confirmed against RetiChat's own "Copy Contact URI" output on real
    // hardware: lxma://<dest-hash>:<pubkey-hex> — ours was missing the
    // public key entirely, so a receiving lxma://-aware app had no way to
    // address/encrypt to this contact without first seeing a live announce.
    _legacyPayload = "lxma://" + std::string(destHashHex.c_str()) + ":" + std::string(publicKeyHex.c_str());
    if (!renderCode(false)) return false;
    if (kTall) {
        // Full width available below the QR — the whole 32-hex address
        // fits on one line, no need for the narrow column's 4-line wrap.
        lv_label_set_text_fmt(_lblAddr, "LXMF: %s", destHashHex.c_str());
        lv_label_set_text_fmt(_lblIdentity, "ID: %s", identityHashHex.c_str());
    } else {
        lv_label_set_text_fmt(_lblAddr, "%.8s\n%.8s\n%.8s\n%.8s", destHashHex.c_str(),
                             destHashHex.c_str() + 8, destHashHex.c_str() + 16, destHashHex.c_str() + 24);
    }
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

void LvQrOverlay::toggleZoom() {
    if (!kTall) return;  // only this board's layout supports it (see create())
    _zoomed = !_zoomed;
    const lv_obj_flag_t hideFlag = LV_OBJ_FLAG_HIDDEN;
    if (_zoomed) {
        lv_obj_add_flag(_title, hideFlag);
        lv_obj_add_flag(_lblFormat, hideFlag);
        lv_obj_add_flag(_lblAddr, hideFlag);
        lv_obj_add_flag(_lblIdentity, hideFlag);
        lv_obj_add_flag(_toggle, hideFlag);
        lv_obj_add_flag(_closeBtn, hideFlag);
        const int paperSize = kTallQrZoomSize + 24;
        lv_obj_set_size(_paper, paperSize, paperSize);
        lv_obj_set_pos(_paper, (Theme::SCREEN_W - paperSize) / 2, (Theme::SCREEN_H - paperSize) / 2);
    } else {
        lv_obj_clear_flag(_title, hideFlag);
        lv_obj_clear_flag(_lblFormat, hideFlag);
        lv_obj_clear_flag(_lblAddr, hideFlag);
        lv_obj_clear_flag(_lblIdentity, hideFlag);
        lv_obj_clear_flag(_toggle, hideFlag);
        lv_obj_clear_flag(_closeBtn, hideFlag);
        const int paperSize = kTallQrSize + 24;
        lv_obj_set_size(_paper, paperSize, paperSize);
        lv_obj_set_pos(_paper, (Theme::SCREEN_W - paperSize) / 2, 32);
    }
    resizeQr(_zoomed ? kTallQrZoomSize : kTallQrSize);
    const auto& payload = _legacy ? _legacyPayload : _payload;
    lv_qrcode_update(_qr, payload.data(), payload.size());
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
