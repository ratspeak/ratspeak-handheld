#include "LvSettingsScreen.h"
#include "LvInput.h"
#include "LvTheme.h"
#include "Theme.h"
#include "fonts/fonts.h"
#include <algorithm>

using namespace handheld::propagation;

void LvSettingsScreen::onExit() {
    ++_propGeneration; _propChoicePending = false;
}

void LvSettingsScreen::buildPropagationItems(int& idx) {
    auto& settings = _cfg->settings().propagation;
    const int start = idx; _propCategory = int(_categories.size());
    _items.push_back({"Propagation", SettingType::TOGGLE,
        [&settings] { return settings.enabled ? 1 : 0; }, [&settings](int v) { settings.enabled = v != 0; }}); ++idx;
    _items.push_back({"Node mode", SettingType::ENUM_CHOICE,
        [&settings] { return int(settings.selection); }, [this, &settings](int v) {
            settings.selection = Selection(v); _propChoicePending = v == int(Selection::Manual);
        }, nullptr, 0, 1, 1, {"AUTO", "MANUAL"}}); ++idx;
    _items.push_back({"Delivery", SettingType::ENUM_CHOICE,
        [&settings] { return int(settings.delivery); }, [&settings](int v) { settings.delivery = Delivery(v); },
        nullptr, 0, 1, 1, {"AUTO", "ALWAYS"}}); ++idx;
    _items.push_back({"Manual node", SettingType::READONLY, nullptr, nullptr, [&settings](int) {
        char hash[33]; settings.manualHex(hash); return settings.hasManual ? String(hash) : String("Not set");
    }}); ++idx;
    auto action = [&](const char* label, std::function<void()> run) {
        SettingItem item; item.label = label; item.type = SettingType::ACTION;
        item.formatter = [](int) { return String("[Enter]"); }; item.action = std::move(run);
        _items.push_back(std::move(item)); ++idx;
    };
    action("Choose / replace node", [this] { showPropagationDialog(SettingsView::PROPAGATION_CHOICE); });
    action("Clear manual node", [this, &settings] { settings.setManual(nullptr, 0); applyAndSave(); });
    _items.push_back({"Inbox", SettingType::READONLY, nullptr, nullptr, [this](int) {
        return String(syncLabel(_service ? _service->status().propagation.status : SyncStatus::Off));
    }}); ++idx;
    action("Sync now", [this] { if (_service) _service->action(handheld::Operation::PropagationSync); });
    _categories.push_back({"Propagation", start, idx - start, [&settings] {
        return settings.enabled ? String(settings.delivery == Delivery::Always ? "ON / ALWAYS" : "ON / AUTO") : String("OFF");
    }});
}

void LvSettingsScreen::pollPropagationUI() {
    if (_propChoicePending && (!_service || !_service->settingsPending())) {
        _propChoicePending = false;
        if (_cfg && _view == SettingsView::ITEM_LIST && _categoryIdx == _propCategory &&
            _cfg->settings().propagation.selection == Selection::Manual)
            showPropagationDialog(SettingsView::PROPAGATION_CHOICE);
    }
    const auto status = _service ? _service->status().propagation.status : SyncStatus::Off;
    if (status != _propStatus) {
        _propStatus = status;
        if (_view == SettingsView::ITEM_LIST && _categoryIdx == _propCategory && !_editing && !_textEditing && !_freqEditing)
            rebuildItemList();
    }
}

void LvSettingsScreen::showPropagationDialog(SettingsView view) {
    ++_propGeneration; _propSelected = 0; _propChoicePending = false;
    _editing = _textEditing = _freqEditing = _numericTyping = false; _editValueLbl = nullptr;
    _view = view;
    if (view == SettingsView::PROPAGATION_ENTRY) {
        if (_cfg) _cfg->settings().propagation.manualHex(_propInput);
        else memset(_propInput, 0, sizeof _propInput);
    }
    if (view == SettingsView::PROPAGATION_NODES) {
        _propCount = 0; _propLoading = true; _propFailed = false;
        const auto generation = _propGeneration;
        if (!_service || !_service->requestPropagationNodes([this, generation](const handheld::Result& result,
            const NodeView* rows, size_t count) {
            if (_propGeneration != generation || _view != SettingsView::PROPAGATION_NODES) return;
            _propLoading = false; _propFailed = result.outcome != handheld::Outcome::Ok;
            if (!_propFailed && count <= NodeViewCapacity) {
                _propCount = count; memcpy(_propNodes, rows, count * sizeof(*rows));
                std::sort(_propNodes, _propNodes + count, [](const NodeView& a, const NodeView& b) {
                    if ((a.interface == 0) != (b.interface == 0)) return a.interface != 0;
                    if (a.usable != b.usable) return a.usable;
                    if (a.cost != b.cost) return a.cost < b.cost;
                    return a.hops < b.hops;
                });
            }
            rebuildPropagationDialog();
        })) { _propLoading = false; _propFailed = true; }
    }
    rebuildPropagationDialog();
}

void LvSettingsScreen::savePropagationAddress(const uint8_t address[16]) {
    if (!_cfg || !address) return;
    auto& settings = _cfg->settings().propagation;
    memcpy(settings.manual, address, 16); settings.hasManual = true; settings.selection = Selection::Manual;
    ++_propGeneration; _view = SettingsView::ITEM_LIST;
    applyAndSave(); rebuildItemList();
}

void LvSettingsScreen::activatePropagationRow(int row) {
    if (_service && _service->settingsPending()) return;
    if (_view == SettingsView::PROPAGATION_CHOICE) {
        if (row == 0) showPropagationDialog(SettingsView::PROPAGATION_ENTRY);
        else if (row == 1) showPropagationDialog(SettingsView::PROPAGATION_NODES);
        else if (row == 2) { ++_propGeneration; _view = SettingsView::ITEM_LIST; rebuildItemList(); }
    } else if (_view == SettingsView::PROPAGATION_NODES) {
        if (row == 0) showPropagationDialog(SettingsView::PROPAGATION_CHOICE);
        else if (row == 1) showPropagationDialog(SettingsView::PROPAGATION_NODES);
        else if (row >= 2 && size_t(row - 2) < _propCount) savePropagationAddress(_propNodes[row - 2].address);
    } else if (_view == SettingsView::PROPAGATION_ENTRY) {
        if (row == 0) { showPropagationDialog(SettingsView::PROPAGATION_CHOICE); return; }
        Settings parsed;
        if (strlen(_propInput) != 32 || !parsed.setManual(_propInput, 32)) {
            if (_ui) _ui->lvStatusBar().showToast("Enter 32 hexadecimal characters", 2000);
            return;
        }
        savePropagationAddress(parsed.manual);
    }
}

bool LvSettingsScreen::handlePropagationKey(const KeyEvent& event) {
    if (event.character == 0x1b) {
        activatePropagationRow(_view == SettingsView::PROPAGATION_CHOICE ? 2 : 0); return true;
    }
    if (_view == SettingsView::PROPAGATION_ENTRY) {
        const size_t length = strlen(_propInput);
        if (event.enter || event.character == '\n' || event.character == '\r') { activatePropagationRow(1); return true; }
        if (event.del || event.character == 8) {
            if (length) _propInput[length - 1] = 0;
            else if (!event.repeat) { activatePropagationRow(0); return true; }
        } else {
            const char c = event.character;
            if (length < 32 && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                _propInput[length] = c; _propInput[length + 1] = 0;
            }
        }
        if (_editValueLbl) lv_label_set_text(_editValueLbl, _propInput[0] ? _propInput : "_");
        return true;
    }
    if (event.del || event.character == 8) {
        if (!event.repeat) activatePropagationRow(_view == SettingsView::PROPAGATION_CHOICE ? 2 : 0);
        return true;
    }
    if (event.enter || event.character == '\n' || event.character == '\r') {
        if (auto* focused = lv_group_get_focused(LvInput::group())) _propSelected = int(intptr_t(lv_obj_get_user_data(focused)));
        activatePropagationRow(_propSelected); return true;
    }
    return false; // Ordinary focus navigation owns arrows and wheel/trackball.
}

void LvSettingsScreen::rebuildPropagationDialog() {
    if (!_scrollContainer) return;
    _rowObjs.clear(); _editValueLbl = nullptr; lv_obj_clean(_scrollContainer);
    auto row = [&](const char* title, const char* detail, int index) {
        auto* object = lv_obj_create(_scrollContainer);
        lv_obj_set_size(object, Theme::CONTENT_W, detail ? 48 : 32);
        lv_obj_add_style(object, LvTheme::styleListBtn(), 0);
        lv_obj_set_style_pad_all(object, 0, 0); lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
        if (index >= 0) {
            lv_obj_add_style(object, LvTheme::styleListBtnFocused(), LV_STATE_FOCUSED);
            lv_obj_set_user_data(object, reinterpret_cast<void*>(intptr_t(index)));
            lv_obj_add_event_cb(object, [](lv_event_t* event) {
                auto* self = static_cast<LvSettingsScreen*>(lv_event_get_user_data(event));
                self->activatePropagationRow(int(intptr_t(lv_obj_get_user_data(lv_event_get_target(event)))));
            }, LV_EVENT_CLICKED, this);
            lv_obj_add_event_cb(object, [](lv_event_t* event) {
                lv_obj_scroll_to_view(lv_event_get_target(event), LV_ANIM_ON);
            }, LV_EVENT_FOCUSED, nullptr);
            lv_group_add_obj(LvInput::group(), object); _rowObjs.push_back(object);
        } else lv_obj_clear_flag(object, LV_OBJ_FLAG_CLICKABLE);
        auto label = [&](const char* text, int y, const lv_font_t* font) {
            auto* value = lv_label_create(object); lv_obj_set_width(value, Theme::CONTENT_W - 16);
            lv_label_set_long_mode(value, LV_LABEL_LONG_DOT); lv_obj_set_style_text_font(value, font, 0);
            lv_obj_set_style_text_color(value, lv_color_hex(Theme::TEXT_PRIMARY), 0);
            lv_label_set_text(value, text); lv_obj_align(value, LV_ALIGN_TOP_LEFT, 8, y); return value;
        };
        auto* main = label(title, 6, &lv_font_rsdeck_12);
        if (detail) label(detail, 28, &lv_font_rsdeck_10);
        return main;
    };
    if (_view == SettingsView::PROPAGATION_CHOICE) {
        row("Manual propagation node", "The address stays pinned while unreachable", -1);
        row("Enter address", nullptr, 0); row("Choose from list", nullptr, 1); row("< Back", nullptr, 2);
    } else if (_view == SettingsView::PROPAGATION_ENTRY) {
        row("< Back", nullptr, 0);
        row("Node address", "32 hexadecimal characters", -1);
        _editValueLbl = row(_propInput[0] ? _propInput : "_", nullptr, -1);
        lv_obj_set_style_text_font(_editValueLbl, &lv_font_rsdeck_10, 0);
        row("Save address", "Verification can finish when the node is reachable", 1);
    } else {
        row("< Back", nullptr, 0); row("Refresh nodes", nullptr, 1);
        if (_propLoading) row("Loading nodes...", nullptr, -1);
        else if (_propFailed) row("List unavailable; refresh to retry", nullptr, -1);
        else if (!_propCount) row("No propagation nodes discovered", nullptr, -1);
        for (size_t i = 0; i < _propCount; ++i) {
            const auto& node = _propNodes[i]; Settings hash; hash.hasManual = true; memcpy(hash.manual, node.address, 16);
            char address[33], title[80]; hash.manualHex(address);
            snprintf(title, sizeof title, "%s %.24s  %s", node.interface ? "WiFi" : "LoRa",
                     node.name[0] ? node.name : "Node", node.usable ? "" : "(unavailable)");
            row(title, address, int(i + 2));
        }
    }
    if (!_rowObjs.empty()) LvInput::focusObj(_rowObjs[0]);
}
