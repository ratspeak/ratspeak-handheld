#include "LvNodesScreen.h"
#include "Theme.h"
#include "LvTheme.h"
#include "LvInput.h"
#include <cstring>
#include "UIManager.h"
#include "config/UserConfig.h"
#include <Arduino.h>
#include <algorithm>
#include <climits>
#include "fonts/fonts.h"

namespace {

void setText(lv_obj_t* label, const char* text) {
    if (strcmp(lv_label_get_text(label), text)) lv_label_set_text(label, text);
}
unsigned long nodeAgeMs(const DiscoveredNode& node, unsigned long now) {
    if (node.lastSeen == 0 || now < node.lastSeen) return ULONG_MAX;
    return now - node.lastSeen;
}

std::string displayNameFor(const DiscoveredNode& node) {
    if (!node.name.empty()) return node.name;
    std::string hex = node.hash.toHex();
    return hex.substr(0, 12);
}

std::string identityLineFor(const DiscoveredNode& node) {
    std::string hex = node.hash.toHex();
    return "ID: " + hex;
}

std::string compactAge(unsigned long ageMs) {
    if (ageMs == ULONG_MAX) return "old";
    if (ageMs < 5000) return "now";

    unsigned long sec = ageMs / 1000;
    char buf[12];
    if (sec < 60) {
        snprintf(buf, sizeof(buf), "%lus", sec);
    } else if (sec < 3600) {
        snprintf(buf, sizeof(buf), "%lum", sec / 60);
    } else if (sec < 86400) {
        snprintf(buf, sizeof(buf), "%luh", sec / 3600);
    } else {
        snprintf(buf, sizeof(buf), "%lud", sec / 86400);
    }
    return buf;
}

std::string peerMetaFor(const DiscoveredNode& node, unsigned long ageMs, bool devMode) {
    std::string meta = compactAge(ageMs);
    if (devMode && node.rssi != 0) {
        char buf[14];
        snprintf(buf, sizeof(buf), " %ddB", node.rssi);
        meta += buf;
    }
    return meta;
}

lv_obj_t* createEmptyState(lv_obj_t* parent) {
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_set_size(box, 252, 94);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(box);

    for (int i = 0; i < 4; i++) {
        lv_obj_t* pip = lv_obj_create(box);
        lv_obj_set_size(pip, 6, 6);
        lv_obj_set_pos(pip, 104 + i * 13, 14);
        lv_obj_set_style_radius(pip, 3, 0);
        lv_obj_set_style_bg_color(pip, lv_color_hex(i == 0 ? Theme::TEXT_MUTED : Theme::BORDER), 0);
        lv_obj_set_style_bg_opa(pip, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(pip, 0, 0);
        lv_obj_set_style_pad_all(pip, 0, 0);
        lv_obj_clear_flag(pip, LV_OBJ_FLAG_SCROLLABLE);
    }

    lv_obj_t* title = lv_label_create(box);
    lv_obj_set_style_text_font(title, &lv_font_rsdeck_14, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(Theme::TEXT_SECONDARY), 0);
    lv_label_set_text(title, "No peers heard");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 34);

    lv_obj_t* hint = lv_label_create(box);
    lv_obj_set_style_text_font(hint, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(Theme::TEXT_MUTED), 0);
    lv_label_set_text(hint, "Listening for announces");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 55);

    return box;
}

}  // namespace

void LvNodesScreen::createUI(lv_obj_t* parent) {
    _screen = parent;
    lv_obj_set_style_bg_color(parent, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    _emptyState = createEmptyState(parent);

    _list = lv_obj_create(parent);
    lv_obj_set_pos(_list, 0, 19);
    lv_obj_set_size(_list, Theme::CONTENT_W, ViewportHeight);
    lv_obj_add_style(_list, LvTheme::styleList(), 0);
    lv_obj_set_scroll_dir(_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_event_cb(_list, [](lv_event_t* e) {
        auto* self = static_cast<LvNodesScreen*>(lv_event_get_user_data(e));
        if (self->_bindingRows) return;
        const auto code = lv_event_get_code(e);
        if (code == LV_EVENT_SCROLL || code == LV_EVENT_SCROLL_END) {
            const auto oldBegin = self->_peers.visibleBegin();
            const auto oldEnd = self->_peers.visibleEnd();
            self->_peers.setScrollY(lv_obj_get_scroll_y(self->_list));
            if (code == LV_EVENT_SCROLL_END || oldBegin != self->_peers.visibleBegin() || oldEnd != self->_peers.visibleEnd())
                self->bindRows();
        }
    }, LV_EVENT_ALL, this);
    // A single inert end marker gives LVGL the complete logical extent.
    _extent = lv_obj_create(_list);
    lv_obj_remove_style_all(_extent);
    lv_obj_set_size(_extent, 1, 1);
    lv_obj_clear_flag(_extent, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(_list, LV_OBJ_FLAG_HIDDEN);

    _caption = lv_label_create(parent);
    lv_obj_set_pos(_caption, 8, 2);
    lv_obj_set_style_text_font(_caption, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(_caption, lv_color_hex(Theme::TEXT_SECONDARY), 0);
    for (size_t i = 0; i < _rows.size(); ++i) {
        auto& row = _rows[i];
        row.box = lv_obj_create(_list);
        lv_obj_set_size(row.box, Theme::CONTENT_W, RowHeight);
        lv_obj_add_style(row.box, LvTheme::styleListBtn(), 0);
        lv_obj_add_style(row.box, LvTheme::styleListBtnFocused(), LV_STATE_FOCUSED);
        lv_obj_set_style_border_side(row.box, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row.box, 1, 0);
        lv_obj_set_style_border_color(row.box, lv_color_hex(Theme::BORDER), 0);
        lv_obj_set_style_pad_all(row.box, 0, 0);
        lv_obj_clear_flag(row.box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS | LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_add_flag(row.box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_user_data(row.box, reinterpret_cast<void*>(i));
        lv_obj_add_event_cb(row.box, [](lv_event_t* e) {
            auto* self = static_cast<LvNodesScreen*>(lv_event_get_user_data(e));
            auto* box = lv_event_get_target(e);
            const size_t slot = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(box));
            auto& row = self->_rows[slot];
            if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
                self->_pressedRow = box;
                self->_pressedHex = row.hex;
            } else if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
                const auto hex = self->_pressedRow == box ? self->_pressedHex : row.hex;
                if (hex.empty()) return;
                for (size_t i = 0; i < self->_peers.total(); ++i)
                    if (self->_peers.hash(i) == hex) { self->_peers.select(i); break; }
                self->focusSelection();
                self->showActionMenu(hex);
            }
            // RELEASED precedes CLICKED. The pinned slot is retired only by a
            // subsequent refresh, after LVGL finishes delivering this input.
        }, LV_EVENT_ALL, this);
        row.name = lv_label_create(row.box);
        lv_obj_set_style_text_font(row.name, &lv_font_rsdeck_12, 0);
        lv_obj_set_pos(row.name, 8, 3);
        lv_obj_set_size(row.name, Theme::CONTENT_W - 132, lv_font_rsdeck_12.line_height);
        lv_label_set_long_mode(row.name, LV_LABEL_LONG_DOT);
        row.meta = lv_label_create(row.box);
        lv_obj_set_style_text_font(row.meta, &lv_font_rsdeck_10, 0);
        lv_obj_set_style_text_color(row.meta, lv_color_hex(Theme::TEXT_SECONDARY), 0);
        lv_obj_set_style_text_align(row.meta, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(row.meta, Theme::CONTENT_W - 124, 5);
        lv_obj_set_size(row.meta, 116, lv_font_rsdeck_10.line_height);
        lv_label_set_long_mode(row.meta, LV_LABEL_LONG_CLIP);
        row.id = lv_label_create(row.box);
        lv_obj_set_style_text_font(row.id, &lv_font_rsdeck_10, 0);
        lv_obj_set_style_text_color(row.id, lv_color_hex(Theme::TEXT_MUTED), 0);
        lv_obj_set_pos(row.id, 8, 20);
        lv_obj_set_size(row.id, Theme::CONTENT_W - 16, lv_font_rsdeck_10.line_height);
        lv_label_set_long_mode(row.id, LV_LABEL_LONG_CLIP);
    }
    // --- Action modal overlay (on top layer, centered) ---
    _overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(_overlay, 260, 158);
    lv_obj_set_pos(_overlay, (Theme::SCREEN_W - 260) / 2, Theme::STATUS_BAR_H + (Theme::CONTENT_H - 158) / 2);
    lv_obj_add_style(_overlay, LvTheme::styleModal(), 0);
    lv_obj_set_style_pad_all(_overlay, 0, 0);
    lv_obj_clear_flag(_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_overlay, LV_OBJ_FLAG_HIDDEN);

    _overlayTitle = lv_label_create(_overlay);
    lv_obj_set_style_text_font(_overlayTitle, &lv_font_rsdeck_14, 0);
    lv_obj_set_style_text_color(_overlayTitle, lv_color_hex(Theme::ACCENT), 0);
    lv_label_set_long_mode(_overlayTitle, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(_overlayTitle, 236);
    lv_obj_set_pos(_overlayTitle, 12, 9);

    _overlayMeta = lv_label_create(_overlay);
    lv_obj_set_style_text_font(_overlayMeta, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(_overlayMeta, lv_color_hex(Theme::TEXT_MUTED), 0);
    lv_label_set_long_mode(_overlayMeta, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(_overlayMeta, 236);
    lv_obj_set_pos(_overlayMeta, 12, 29);

    _overlayReach = lv_label_create(_overlay);
    lv_obj_set_style_text_font(_overlayReach, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(_overlayReach, lv_color_hex(Theme::TEXT_SECONDARY), 0);
    lv_label_set_long_mode(_overlayReach, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(_overlayReach, 236);
    lv_obj_set_pos(_overlayReach, 12, 43);

    const char* menuText[] = {"Save Contact", "Message", "Close"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t* btn = lv_obj_create(_overlay);
        lv_obj_set_size(btn, 236, 24);
        lv_obj_set_pos(btn, 12, 63 + i * 27);
        lv_obj_set_style_bg_color(btn, lv_color_hex(Theme::BG_SURFACE), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(Theme::BORDER), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(btn, (void*)(intptr_t)i);
        lv_obj_add_event_cb(btn, [](lv_event_t* e) {
            auto* self = (LvNodesScreen*)lv_event_get_user_data(e);
            int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
            self->_menuIdx = idx;
            KeyEvent tap = {};
            tap.enter = true;
            self->handleKey(tap);
        }, LV_EVENT_CLICKED, this);

        _menuLabels[i] = lv_label_create(btn);
        lv_obj_set_style_text_font(_menuLabels[i], &lv_font_rsdeck_14, 0);
        lv_obj_set_style_text_color(_menuLabels[i], lv_color_hex(Theme::PRIMARY), 0);
        lv_label_set_text(_menuLabels[i], menuText[i]);
        lv_obj_center(_menuLabels[i]);

        _menuBtns[i] = btn;
    }

    // Nickname input widgets
    _nicknameBox = lv_obj_create(_overlay);
    lv_obj_set_size(_nicknameBox, 236, 86);
    lv_obj_set_pos(_nicknameBox, 12, 63);
    lv_obj_set_style_bg_opa(_nicknameBox, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_nicknameBox, 0, 0);
    lv_obj_set_style_pad_all(_nicknameBox, 0, 0);
    lv_obj_clear_flag(_nicknameBox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_nicknameBox, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* nickTitle = lv_label_create(_nicknameBox);
    lv_obj_set_style_text_font(nickTitle, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(nickTitle, lv_color_hex(Theme::TEXT_SECONDARY), 0);
    lv_label_set_text(nickTitle, "Contact name");
    lv_obj_set_pos(nickTitle, 0, 0);

    _nicknameLbl = lv_label_create(_nicknameBox);
    lv_obj_set_style_text_font(_nicknameLbl, &lv_font_rsdeck_14, 0);
    lv_obj_set_style_text_color(_nicknameLbl, lv_color_hex(Theme::PRIMARY), 0);
    lv_label_set_long_mode(_nicknameLbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(_nicknameLbl, 232);
    lv_label_set_text(_nicknameLbl, "_");
    lv_obj_set_pos(_nicknameLbl, 0, 22);

    _nicknameHint = lv_label_create(_nicknameBox);
    lv_obj_set_style_text_font(_nicknameHint, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(_nicknameHint, lv_color_hex(Theme::TEXT_MUTED), 0);
#if HAS_TRACKBALL
    lv_label_set_text(_nicknameHint, "Enter saves / Hold click cancels");
#else
    lv_label_set_text(_nicknameHint, "Enter saves / Alt+Back cancels");
#endif
    lv_obj_set_pos(_nicknameHint, 0, 46);

    lv_obj_t* cancel = lv_btn_create(_nicknameBox);
    // Nickname keys are handled by the screen; this button is a touch action.
    // Leaving a hidden default-group button behind during a row rebuild can
    // strand the group's focus and prevent keypad/encoder input reaching peers.
    lv_group_remove_obj(cancel);
    lv_obj_set_pos(cancel, 154, 61);
    lv_obj_set_size(cancel, 80, 22);
    lv_obj_add_style(cancel, LvTheme::styleBtn(), 0);
    lv_obj_add_style(cancel, LvTheme::styleBtnPressed(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(cancel, [](lv_event_t* e) {
        auto* self = static_cast<LvNodesScreen*>(lv_event_get_user_data(e));
        self->hideOverlay();
    }, LV_EVENT_CLICKED, this);
    lv_obj_t* cancelLabel = lv_label_create(cancel);
    lv_obj_set_style_text_font(cancelLabel, &lv_font_rsdeck_10, 0);
    lv_label_set_text(cancelLabel, "Cancel");
    lv_obj_center(cancelLabel);
}

void LvNodesScreen::destroyUI() {
    if (_overlay) { lv_obj_del(_overlay); _overlay = nullptr; }
    for (int i = 0; i < 3; i++) { _menuBtns[i] = nullptr; _menuLabels[i] = nullptr; }
    _overlayTitle = nullptr; _overlayMeta = nullptr; _overlayReach = nullptr;
    _nicknameBox = nullptr; _nicknameLbl = nullptr; _nicknameHint = nullptr;
    _list = nullptr; _emptyState = nullptr; _caption = nullptr;
    _extent = nullptr; _pressedRow = nullptr; _pressedHex.clear();
    _rows = {}; _peers.reset();
    LvScreen::destroyUI();
}

void LvNodesScreen::onEnter() {
    _peers.reset();
    _peers.configure(RowHeight, ViewportHeight);
    _pressedRow = nullptr; _pressedHex.clear();
#if HAS_TOUCH
    _focusActive = false;
#endif
    _confirmDelete = false;
    hideOverlay();
    rebuildList();
}

bool LvNodesScreen::interactionBusy() const {
    // Read LVGL's current ownership instead of latching BEGIN/END: input reset
    // (sleep or a keyboard takeover) can retire a drag without SCROLL_END.
    if (_list) {
        if (lv_obj_is_scrolling(_list)) return true; // Drag and momentum owner.
        lv_point_t end;
        lv_obj_get_scroll_end(_list, &end);
        // Elastic return can outlive the pointer's scroll owner.
        if (end.x != lv_obj_get_scroll_x(_list) || end.y != lv_obj_get_scroll_y(_list)) return true;
    }
    for (const auto& row : _rows) if (lv_obj_has_state(row.box, LV_STATE_PRESSED)) return true;
    // The pointer can remain held after leaving its originally pressed row.
    for (auto* input = lv_indev_get_next(nullptr); input; input = lv_indev_get_next(input))
        if (lv_indev_get_type(input) == LV_INDEV_TYPE_POINTER && input->proc.state == LV_INDEV_STATE_PRESSED)
            return true;
    return false;
}

void LvNodesScreen::refreshUI() {
    if (!_am || !_list || _actionState != NodeAction::BROWSE || _confirmDelete || interactionBusy()) return;
    // Clearing the pinned slot happens outside the press/release callback stack.
    _pressedRow = nullptr; _pressedHex.clear();
    if (millis() - _lastRebuild >= 1000) rebuildList();
}

void LvNodesScreen::rebuildList() {
    if (!_am || !_list || interactionBusy()) return;
    _lastRebuild = millis();
    _peers.sync(_am->nodes());
    _bindingRows = true;
    // Hide old positions before reducing the extent, or old pooled rows can
    // temporarily keep LVGL's scroll range larger than the directory.
    for (auto& row : _rows) lv_obj_add_flag(row.box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(_extent, 0, std::max(0, int(_peers.total()) * RowHeight - 1));
    lv_obj_update_layout(_list);
    lv_obj_scroll_to_y(_list, _peers.scrollY(), LV_ANIM_OFF);
    _bindingRows = false;
    char caption[32];
    snprintf(caption, sizeof(caption), "%u peers", unsigned(_peers.total()));
    setText(_caption, caption);
    if (_peers.total()) {
        lv_obj_add_flag(_emptyState, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(_list, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(_emptyState, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_list, LV_OBJ_FLAG_HIDDEN);
    }
    bindRows();
}

void LvNodesScreen::bindRows() {
    if (_bindingRows || !_list || !_am) return;
    _bindingRows = true;
    const size_t begin = _peers.visibleBegin(1), end = _peers.visibleEnd(1);
    // Keep matching slots in place, including the one whose press is pending.
    for (auto& row : _rows) {
        const bool keep = row.index >= begin && row.index < end && row.hex == _peers.hash(row.index);
        if (!keep && row.box != _pressedRow) {
            lv_group_remove_obj(row.box);
            lv_obj_add_flag(row.box, LV_OBJ_FLAG_HIDDEN);
            row.hex.clear();
        }
    }
    for (size_t index = begin; index < end; ++index) {
        Row* bound = nullptr;
        for (auto& row : _rows) if (row.index == index && row.hex == _peers.hash(index)) { bound = &row; break; }
        if (!bound) for (auto& row : _rows) if (row.hex.empty() && row.box != _pressedRow) { bound = &row; break; }
        if (!bound) continue; // Pool includes a spare for a pinned pointer row.
        auto& row = *bound;
        row.index = index; row.hex = _peers.hash(index);
        lv_obj_set_pos(row.box, 0, int(index) * RowHeight);
        lv_obj_clear_flag(row.box, LV_OBJ_FLAG_HIDDEN);
        const auto* node = _am->findNodeByHex(row.hex);
        // NodeView can publish between sync and a scroll event. Never use an
        // earlier vector index here, even while membership is deferred.
        const auto name = node ? displayNameFor(*node) : row.hex.substr(0, 12);
        if (row.nameText != name) { row.nameText = name; lv_label_set_text(row.name, name.c_str()); }
        lv_obj_set_style_text_color(row.name, lv_color_hex(node && node->saved ? Theme::ACCENT : Theme::PRIMARY), 0);
        setText(row.meta, node ? peerMetaFor(*node, nodeAgeMs(*node, millis()), _cfg && _cfg->settings().devMode).c_str() : "unavailable");
        setText(row.id, ("ID: " + row.hex).c_str());
    }
    focusSelection();
    _bindingRows = false;
}

void LvNodesScreen::focusSelection() {
    lv_obj_t* selected = nullptr;
    for (auto& row : _rows) {
        if (!row.hex.empty() && row.hex == _peers.selectedHash() &&
            row.index >= _peers.visibleBegin() && row.index < _peers.visibleEnd()) selected = row.box;
        else { lv_group_remove_obj(row.box); lv_obj_clear_state(row.box, LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY); }
    }
    if (selected) {
        lv_group_add_obj(LvInput::group(), selected);
        LvInput::focusObj(selected);
#if HAS_TOUCH
        if (!_focusActive) lv_obj_clear_state(selected, LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY);
#endif
    }
}

void LvNodesScreen::scrollToSelection() {
    _peers.ensureSelectedVisible();
    _bindingRows = true;
    lv_obj_scroll_to_y(_list, _peers.scrollY(), LV_ANIM_OFF);
    _bindingRows = false;
    bindRows();
}

std::string LvNodesScreen::getFocusedNodeHex() const {
    return _peers.selectedHash();
}

// --- Action modal helpers ---

void LvNodesScreen::updateOverlayDetails(const char* title) {
    if (!_overlayTitle || !_overlayMeta || !_overlayReach) return;

    const DiscoveredNode* nodePtr = _am ? _am->findNodeByHex(_actionNodeHex) : nullptr;
    if (!nodePtr) {
        lv_label_set_text(_overlayTitle, title ? title : "Peer");
        lv_label_set_text(_overlayMeta, "ID: unavailable");
        lv_label_set_text(_overlayReach, "No route data");
        return;
    }

    const auto& node = *nodePtr;
    unsigned long age = nodeAgeMs(node, millis());
    bool devMode = _cfg && _cfg->settings().devMode;

    std::string heading = title ? title : displayNameFor(node);
    std::string identity = identityLineFor(node);
    std::string details = node.saved ? "Saved contact / " : "Peer / ";
    details += peerMetaFor(node, age, devMode);

    lv_obj_set_style_text_color(_overlayTitle, lv_color_hex(node.saved ? Theme::ACCENT : Theme::PRIMARY), 0);
    lv_obj_set_style_text_color(_overlayReach, lv_color_hex(Theme::TEXT_SECONDARY), 0);
    lv_label_set_text(_overlayTitle, heading.c_str());
    lv_label_set_text(_overlayMeta, identity.c_str());
    lv_label_set_text(_overlayReach, details.c_str());
}

void LvNodesScreen::showActionMenu(const std::string& nodeHex) {
    _actionNodeHex = nodeHex;
    _menuIdx = 0;
    _actionState = NodeAction::ACTION_MENU;
    _nicknameText = "";
    if (_overlay) {
        const DiscoveredNode* node = _am ? _am->findNodeByHex(nodeHex) : nullptr;
        if (node) {
            lv_label_set_text(_menuLabels[0], node->saved ? "Edit Name" : "Save Contact");
            lv_label_set_text(_menuLabels[1], "Message");
            lv_label_set_text(_menuLabels[2], "Close");
        }
        updateOverlayDetails(nullptr);
        for (int i = 0; i < 3; i++) lv_obj_clear_flag(_menuBtns[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_nicknameBox, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(_overlay, LV_OBJ_FLAG_HIDDEN);
        updateMenuSelection();
    }
}

void LvNodesScreen::hideOverlay() {
    _actionState = NodeAction::BROWSE;
    _actionNodeHex.clear();
    _nicknameText = "";
    if (_overlay) lv_obj_add_flag(_overlay, LV_OBJ_FLAG_HIDDEN);
}

void LvNodesScreen::showNicknameInput() {
    _actionState = NodeAction::NICKNAME_INPUT;
    const DiscoveredNode* node = _am ? _am->findNodeByHex(_actionNodeHex) : nullptr;
    if (node) {
        _nicknameText = String(node->name.c_str());
    }
    updateOverlayDetails("Set contact name");
    for (int i = 0; i < 3; i++) lv_obj_add_flag(_menuBtns[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_nicknameBox, LV_OBJ_FLAG_HIDDEN);
    updateNicknameDisplay();
}

void LvNodesScreen::updateMenuSelection() {
    for (int i = 0; i < 3; i++) {
        bool sel = (i == _menuIdx);
        lv_obj_set_style_text_color(_menuLabels[i], lv_color_hex(
            sel ? Theme::ACCENT : Theme::TEXT_SECONDARY), 0);
        lv_obj_set_style_bg_color(_menuBtns[i], lv_color_hex(
            sel ? Theme::PRIMARY_SUBTLE : Theme::BG_SURFACE), 0);
        lv_obj_set_style_bg_opa(_menuBtns[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(_menuBtns[i], lv_color_hex(sel ? Theme::BORDER_ACTIVE : Theme::BORDER), 0);
    }
}

void LvNodesScreen::updateNicknameDisplay() {
    if (_nicknameLbl) {
        String display = _nicknameText + "_";
        lv_label_set_text(_nicknameLbl, display.c_str());
    }
}

bool LvNodesScreen::handleLongPress() {
#if HAS_TRACKBALL
    if (_actionState == NodeAction::NICKNAME_INPUT) {
        hideOverlay();
        return true;
    }
#endif
    if (!_am) return false;
#if HAS_TOUCH
    // On trackball/touch boards, an unfocused hold belongs to the power shell.
    if (!_focusActive) return false;
#endif
    std::string nodeHex = getFocusedNodeHex();
    const DiscoveredNode* node = nodeHex.empty() ? nullptr : _am->findNodeByHex(nodeHex);
    if (!node) return false;
    if (node->saved) {
        _confirmDelete = true;
        _actionNodeHex = nodeHex;
        if (_ui) _ui->lvStatusBar().showToast("Remove? Enter=Remove Backspace=Keep", 5000);
    } else {
        showActionMenu(nodeHex);
    }
    return true;
}

bool LvNodesScreen::handleKey(const KeyEvent& event) {
    if (!_am) return false;

#if HAS_TOUCH
    // --- Focus activation guard (trackball/touch browse mode only) ---
    if (_actionState == NodeAction::BROWSE && !_confirmDelete &&
        !_focusActive && (event.up || event.down || event.enter)) {
        _focusActive = true;
        scrollToSelection();
        return true;
    }
#endif

    // --- Nickname input mode ---
    if (_actionState == NodeAction::NICKNAME_INPUT) {
        if (event.enter || event.character == '\n' || event.character == '\r') {
            const DiscoveredNode* node = _am->findNodeByHex(_actionNodeHex);
            if (node) {
                String finalName = _nicknameText;
                finalName.trim();
                if (finalName.isEmpty()) {
                    if (!node->name.empty()) finalName = String(node->name.c_str());
                    else finalName = String(_actionNodeHex.substr(0, 12).c_str());
                }
                if (_am->setContactName(_actionNodeHex, finalName.c_str())) {
                    if (_ui) _ui->lvStatusBar().showToast("Saving contact...", 1200);
                } else if (_ui) {
                    _ui->lvStatusBar().showToast("Contact action unavailable", 1200);
                }
                hideOverlay();
                rebuildList();
            } else {
                if (_ui) _ui->lvStatusBar().showToast("Contact action unavailable", 1200);
                hideOverlay();
            }
            return true;
        }
        if (event.character == 0x1B) { hideOverlay(); return true; }
        if (event.character == '\b' || event.character == 0x7F) {
            if (_nicknameText.length() > 0) _nicknameText.remove(_nicknameText.length() - 1);
            updateNicknameDisplay();
            return true;
        }
        if (event.character >= 0x20 && event.character <= 0x7E && _nicknameText.length() < 16) {
            _nicknameText += (char)event.character;
            updateNicknameDisplay();
            return true;
        }
        return true;
    }

    // --- Action menu mode ---
    if (_actionState == NodeAction::ACTION_MENU) {
        if (event.up) {
            if (_menuIdx > 0) { _menuIdx--; updateMenuSelection(); }
            return true;
        }
        if (event.down) {
            if (_menuIdx < 2) { _menuIdx++; updateMenuSelection(); }
            return true;
        }
        if (event.enter || event.character == '\n' || event.character == '\r') {
            switch (_menuIdx) {
                case 0:
                    showNicknameInput();
                    break;
                case 1:
                    if (!_actionNodeHex.empty() && _onSelect) {
                        std::string hex = _actionNodeHex;
                        hideOverlay();
                        _onSelect(hex);
                    } else {
                        hideOverlay();
                    }
                    break;
                case 2:
                    hideOverlay();
                    break;
            }
            return true;
        }
        if (event.character == 0x1B ||
            ((event.del || event.character == '\b') && !event.repeat)) {
            hideOverlay();
            return true;
        }
        return true;
    }

    // --- Confirm delete mode ---
    if (_confirmDelete) {
        if (event.enter || event.character == '\n' || event.character == '\r') {
            if (!_actionNodeHex.empty()) {
                bool removed = _am->deleteContactByHex(_actionNodeHex);
                if (_ui) _ui->lvStatusBar().showToast(
                    removed ? "Removing contact..." : "Contact action unavailable", 1200);
                _actionNodeHex.clear();
                rebuildList();
            }
            _confirmDelete = false;
            return true;
        }
        _confirmDelete = false;
        if (_ui) _ui->lvStatusBar().showToast("Kept contact", 800);
        return true;
    }

    // 's' or 'S' to save/unsave contact (persists both directions)
    if (event.character == 's' || event.character == 'S') {
        std::string nodeHex = getFocusedNodeHex();
        const DiscoveredNode* node = nodeHex.empty() ? nullptr : _am->findNodeByHex(nodeHex);
        if (node) {
            if (node->saved) _am->unsaveNode(nodeHex);
            else _am->saveNode(nodeHex);
            rebuildList();
        }
        return true;
    }

    // Logical navigation cannot wrap around the reusable row pool. Left/right
    // remains available to the existing board-level tab dispatcher.
    if (event.up || event.down || event.tab) {
        _peers.move(event.up ? -1 : 1);
        scrollToSelection();
        return true;
    }
    if (event.enter) {
        if (!_peers.selectedHash().empty()) showActionMenu(_peers.selectedHash());
        return true;
    }
    return false;
}
