#include "SettingsScreen.h"
#include "Theme.h"
#include <algorithm>

using namespace handheld::propagation;

void SettingsScreen::buildPropagationMenu() {
    _list.clear();
    if (!_config) return;
    const auto& settings = _candidate.settings().propagation;
    _list.addItem(settings.enabled ? "Propagation: ON" : "Propagation: OFF");
    _list.addItem(settings.selection == Selection::Auto ? "Node mode: AUTO" : "Node mode: MANUAL");
    _list.addItem(settings.delivery == Delivery::Auto ? "Delivery: AUTO" : "Delivery: ALWAYS");
    char address[33]; settings.manualHex(address);
    _list.addItem(settings.hasManual ? std::string("Pin ") + address : "Manual node: Not set");
    _list.addItem("Choose / replace node");
    _list.addItem("Clear manual node");
    _list.addItem("Sync now");
    _propStatus = _backend ? _backend->propagationStatus().status : SyncStatus::Off;
    _list.addItem(std::string("Inbox: ") + syncLabel(_propStatus));
    _list.addItem("< Back");
}

void SettingsScreen::pollPropagationUI() {
    if (_propChoicePending && _config && !_config->settingsPending() && !_radioApplyPending) {
        _propChoicePending = false;
        if (!_candidateDirty && _subMenu == MENU_PROPAGATION &&
            _config->settings().propagation.selection == Selection::Manual) showPropagationChoice();
    }
    if (_subMenu == MENU_PROPAGATION && !_editing && _backend &&
        _backend->propagationStatus().status != _propStatus) {
        const int selected = _list.getSelectedIndex(); buildPropagationMenu(); _list.setSelected(selected);
    }
}

void SettingsScreen::showPropagationChoice() {
    _subMenu = MENU_PROPAGATION_CHOICE; _propChoicePending = false;
    _list.clear(); _list.addItem("Enter address"); _list.addItem("Choose from list"); _list.addItem("< Back");
}

void SettingsScreen::showPropagationNodes() {
    _subMenu = MENU_PROPAGATION_NODES; _list.clear();
    _propCount = _backend ? _backend->propagationNodes(_propNodes, NodeViewCapacity) : 0;
    if (_propCount > NodeViewCapacity) _propCount = 0;
    std::sort(_propNodes, _propNodes + _propCount, [](const NodeView& a, const NodeView& b) {
        if ((a.interface == 0) != (b.interface == 0)) return a.interface != 0;
        if (a.usable != b.usable) return a.usable;
        if (a.cost != b.cost) return a.cost < b.cost;
        return a.hops < b.hops;
    });
    // Each address is a selectable, full-width row; the preceding name is also
    // selectable and maps to the same immutable snapshot, including offline nodes.
    for (size_t i = 0; i < _propCount; ++i) {
        char label[64], address[33]; Settings pin; pin.hasManual = true;
        memcpy(pin.manual, _propNodes[i].address, 16); pin.manualHex(address);
        snprintf(label, sizeof label, "%s %s%s", _propNodes[i].interface ? "WiFi" : "LoRa",
                 _propNodes[i].name[0] ? _propNodes[i].name : "Node", _propNodes[i].usable ? "" : " offline");
        _list.addItem(label); _list.addItem(address);
    }
    if (!_propCount) _list.addItem("No nodes discovered");
    _list.addItem("Refresh nodes"); _list.addItem("< Back");
}

bool SettingsScreen::savePropagationAddress(const uint8_t address[16]) {
    if (!_config || !address) return false;
    auto& settings = _candidate.settings().propagation;
    memcpy(settings.manual, address, 16); settings.hasManual = true; settings.selection = Selection::Manual;
    if (!applyAndSave()) return false;
    _subMenu = MENU_PROPAGATION; buildPropagationMenu(); return true;
}

void SettingsScreen::activatePropagationRow(int row) {
    if (!_config || _config->settingsPending() || _radioApplyPending) return;
    if (_subMenu == MENU_PROPAGATION) {
        auto& settings = _candidate.settings().propagation;
        if (row == 0) { settings.enabled = !settings.enabled; applyAndSave(); }
        else if (row == 1) {
            settings.selection = settings.selection == Selection::Auto ? Selection::Manual : Selection::Auto;
            _propChoicePending = settings.selection == Selection::Manual;
            applyAndSave();
        } else if (row == 2) {
            settings.delivery = settings.delivery == Delivery::Auto ? Delivery::Always : Delivery::Auto;
            applyAndSave();
        } else if (row == 4) { showPropagationChoice(); return; }
        else if (row == 5) { settings.setManual(nullptr, 0); applyAndSave(); }
        else if (row == 6) showToast(_backend && _backend->propagationSync() ? "Sync requested" : "OFF, busy, or synced within 30s", 2500);
        else if (row == 8) { _subMenu = MENU_MAIN; buildMainMenu(); return; }
        buildPropagationMenu(); _list.setSelected(row); pollPropagationUI();
    } else if (_subMenu == MENU_PROPAGATION_CHOICE) {
        if (row == 0) {
            char address[33]; _candidate.settings().propagation.manualHex(address);
            startEditing(0, address); _editLabel = "Node: 32 hexadecimal characters";
            _editInput.setMaxLength(32); _editInput.setNumericOnly(false);
        } else if (row == 1) showPropagationNodes();
        else if (row == 2) { _subMenu = MENU_PROPAGATION; buildPropagationMenu(); }
    } else if (_subMenu == MENU_PROPAGATION_NODES) {
        if (row == _list.itemCount() - 1) showPropagationChoice();
        else if (row == _list.itemCount() - 2) showPropagationNodes();
        else if (row >= 0 && size_t(row / 2) < _propCount) savePropagationAddress(_propNodes[row / 2].address);
    }
}
