#include "SettingsScreen.h"
#include "Theme.h"
#include "ui/PropagationNodeText.h"
#include <algorithm>

using namespace handheld::propagation;

void SettingsScreen::buildPropagationMenu() {
    clearRows();
    if (!_config) return;
    const auto& settings = settingsView().propagation;
    addRow(settings.enabled ? "Propagation: ON" : "Propagation: OFF", RowAction::Propagation, 0);
    addRow(settings.selection == Selection::Auto ? "Node mode: AUTO" : "Node mode: MANUAL", RowAction::Propagation, 1);
    addRow(settings.delivery == Delivery::Auto ? "Delivery AUTO: direct first" : "Delivery ALWAYS: relay only", RowAction::Propagation, 2);
    char address[33]; settings.manualHex(address);
    addRow(settings.hasManual ? std::string("Pin ") + address : "Manual node: Not set");
    addRow("Choose / replace node", RowAction::Propagation, 4);
    addRow("Clear manual node", RowAction::Propagation, 5);
    addRow("Sync now", RowAction::Propagation, 6);
    _propStatus = _backend ? _backend->propagationStatus().status : SyncStatus::Off;
    addRow(std::string("Inbox: ") + syncLabel(_propStatus));
    addRow("< Back", RowAction::Back);
}

void SettingsScreen::pollPropagationUI() {
    if (!_active) return;
    if (_propChoicePending && _config && !_config->settingsPending() && !_radioApplyPending) {
        _propChoicePending = false;
        if (!_candidate && _subMenu == MENU_PROPAGATION &&
            _config->settings().propagation.selection == Selection::Manual) showPropagationChoice();
    }
    if (_subMenu == MENU_PROPAGATION && !_editing && _backend &&
        _backend->propagationStatus().status != _propStatus) {
        const int selected = _list.getSelectedIndex(); buildPropagationMenu(); _list.setSelected(selected);
    }
}

void SettingsScreen::showPropagationChoice() {
    if (_subMenu != MENU_PROPAGATION_CHOICE && _subMenu != MENU_PROPAGATION_NODES)
        _focus[_subMenu] = selectedRow();
    _subMenu = MENU_PROPAGATION_CHOICE; _propChoicePending = false;
    clearRows(); addRow("Enter address", RowAction::Propagation, 0);
    addRow("Choose from list", RowAction::Propagation, 1); addRow("< Back", RowAction::Back);
}

void SettingsScreen::showPropagationNodes() {
    if (_subMenu != MENU_PROPAGATION_NODES) _focus[_subMenu] = selectedRow();
    _subMenu = MENU_PROPAGATION_NODES; clearRows();
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
        const auto& settings = settingsView().propagation;
        handheld::ui::propagationNodeTitle(_propNodes[i], settings.enabled && settings.selection == Selection::Auto,
            label, sizeof label);
        addRow(label, RowAction::Propagation, uint8_t(i * 2));
        addRow(address, RowAction::Propagation, uint8_t(i * 2 + 1));
    }
    if (!_propCount) addRow("No nodes discovered");
    addRow("Refresh nodes", RowAction::Propagation, uint8_t(_list.itemCount()));
    addRow("< Back", RowAction::Back);
}

bool SettingsScreen::savePropagationAddress(const uint8_t address[16]) {
    if (!_config || !address) return false;
    UserScalars next = _config->settings();
    auto& settings = next.propagation;
    memcpy(settings.manual, address, 16); settings.hasManual = true; settings.selection = Selection::Manual;
    if (!applyScalars(next)) return false;
    _subMenu = MENU_PROPAGATION; buildPropagationMenu();
    restoreRow(_focus[MENU_PROPAGATION]); return true;
}

void SettingsScreen::activatePropagationRow(int row) {
    if (!_config || _config->settingsPending() || _radioApplyPending) return;
    if (_subMenu == MENU_PROPAGATION) {
        if (row == 4) { showPropagationChoice(); return; }
        if (row == 6) {
            const auto* feedback=!settingsView().propagation.enabled?"Enable propagation to sync":
                !_backend?"Inbox unavailable; try again":_backend->propagationStatus().busy?"Inbox sync already running":
                _backend->propagationSync()?"Sync requested":"Wait 30s, then sync again";
            showToast(feedback,2500); return;
        }
        if (row != 0 && row != 1 && row != 2 && row != 5) return;
        UserScalars next = _config->settings();
        auto& settings = next.propagation;
        if (row == 0) { settings.enabled = !settings.enabled; applyScalars(next); }
        else if (row == 1) {
            settings.selection = settings.selection == Selection::Auto ? Selection::Manual : Selection::Auto;
            _propChoicePending = settings.selection == Selection::Manual;
            applyScalars(next);
        } else if (row == 2) {
            settings.delivery = settings.delivery == Delivery::Auto ? Delivery::Always : Delivery::Auto;
            applyScalars(next);
        } else if (row == 5) { settings.setManual(nullptr, 0); applyScalars(next); }
        buildPropagationMenu(); _list.setSelected(row); pollPropagationUI();
    } else if (_subMenu == MENU_PROPAGATION_CHOICE) {
        if (row == 0) {
            char address[33]; settingsView().propagation.manualHex(address);
            startEditing(0, address); _editLabel = "Node: 32 hexadecimal characters";
            _editInput.setMaxLength(32); _editInput.setNumericOnly(false);
        } else if (row == 1) showPropagationNodes();
    } else if (_subMenu == MENU_PROPAGATION_NODES) {
        if (row == _list.itemCount() - 2) showPropagationNodes();
        else if (row >= 0 && size_t(row / 2) < _propCount) savePropagationAddress(_propNodes[row / 2].address);
    }
}
