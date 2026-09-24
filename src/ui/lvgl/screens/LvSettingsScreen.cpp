#include "LvSettingsScreen.h"
#include "LvPrivacy.h"
#include <new>
#include "Theme.h"
#include "LvTheme.h"
#include "LvInput.h"
#include "config/Config.h"
#include "config/AnnounceInterval.h"
#include "config/UserConfig.h"
#include "radio/RadioFrequency.h"
#include "radio/RadioBandwidth.h"
#include "radio/RadioPresets.h"
#include "screens/LvTimezoneScreen.h"  // For TIMEZONE_TABLE
#include "audio/AudioNotify.h"
#include "hal/Power.h"
#include "transport/WiFiInterface.h"
#include <Arduino.h>
#include <esp_system.h>
#include <WiFi.h>
#include "runtime/FirmwareReleaseCheck.h"
#include "fonts/fonts.h"

namespace {

bool labelEq(const char* a, const char* b) {
    return a && b && strcmp(a, b) == 0;
}

const char* onOff(bool enabled) {
    return enabled ? "ON" : "OFF";
}

const char* wifiModeLabel(RatWiFiMode mode) {
    switch (mode) {
        case RAT_WIFI_AP: return "Hotspot";
        case RAT_WIFI_STA: return "Client";
        case RAT_WIFI_OFF:
        default: return "Off";
    }
}

String formatRadioFrequency(uint32_t hz) {
    char buf[20];
    unsigned long mhz = hz / 1000000;
    unsigned long rem = hz % 1000000;
    if (rem == 0) {
        snprintf(buf, sizeof(buf), "%lu MHz", mhz);
    } else {
        char frac[8];
        snprintf(frac, sizeof(frac), "%06lu", rem);
        int len = 6;
        while (len > 0 && frac[len - 1] == '0') len--;
        frac[len] = '\0';
        snprintf(buf, sizeof(buf), "%lu.%s MHz", mhz, frac);
    }
    return String(buf);
}

String formatEditedValue(const SettingItem& item, int value) {
    String text;
    if (item.allowOff && value == 0) {
        text = "OFF";
    } else if (item.type == SettingType::ENUM_CHOICE && !item.enumLabels.empty()) {
        text = item.enumLabels[constrain(value, 0, (int)item.enumLabels.size() - 1)];
    } else {
        text = item.formatter ? item.formatter(value) : String(value);
    }
#if HAS_SCROLLWHEEL
    return String("[ ") + text + " ]";
#else
    return String("< ") + text + " >";
#endif
}

String maskedValue(const String& value) {
    if (value.isEmpty()) return String("");
    int len = constrain((int)value.length(), 4, 12);
    String masked;
    masked.reserve(len);
    for (int i = 0; i < len; i++) masked += '*';
    return masked;
}

bool isWiFiSSIDLabel(const char* label) {
    return labelEq(label, "WiFi SSID");
}

bool isWiFiPasswordLabel(const char* label) {
    return labelEq(label, "WiFi Password");
}

size_t selectedWiFiSlot(const UserSettings& s) {
    return s.wifiSTASelected < WIFI_STA_MAX_NETWORKS ? s.wifiSTASelected : 0;
}

void ensureWiFiSlot(UserSettings& s, size_t slot) {
    while (s.wifiSTANetworks.size() <= slot && s.wifiSTANetworks.size() < WIFI_STA_MAX_NETWORKS) {
        s.wifiSTANetworks.push_back({});
    }
}

String wifiProfileValue(const UserSettings& s, size_t slot) {
    String label = String(slot + 1);
    label += " ";
    if (slot < s.wifiSTANetworks.size() && !s.wifiSTANetworks[slot].ssid.isEmpty()) {
        label += s.wifiSTANetworks[slot].ssid;
    } else {
        label += "empty";
    }
    return label;
}

String selectedWiFiSSID(const UserSettings& s) {
    size_t slot = selectedWiFiSlot(s);
    if (slot >= s.wifiSTANetworks.size()) return String("");
    return s.wifiSTANetworks[slot].ssid;
}

void clipLabel(lv_obj_t* lbl, int width) {
    lv_obj_set_width(lbl, width);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
}

}  // namespace

int LvSettingsScreen::detectPreset() const {
    return _cfg ? RadioPresets::detect(_cfg->settings()) : -1;
}

void LvSettingsScreen::applyPreset(int presetIdx) {
    if (_cfg) RadioPresets::apply(_cfg->settings(), presetIdx);
}

bool LvSettingsScreen::isEditable(int idx) const {
    if (idx < 0 || idx >= (int)_items.size()) return false;
    const auto& item = _items[idx];
    auto t = item.type;
    if (t == SettingType::ENUM_CHOICE && item.minVal >= item.maxVal) return false;
    return t == SettingType::INTEGER || t == SettingType::TOGGLE
        || t == SettingType::ENUM_CHOICE || t == SettingType::ACTION
        || t == SettingType::TEXT_INPUT;
}

void LvSettingsScreen::skipToNextEditable(int dir) {
    int n = _catRangeEnd;
    int start = _selectedIdx;
    for (int i = 0; i < (n - _catRangeStart); i++) {
        _selectedIdx += dir;
        if (_selectedIdx < _catRangeStart) _selectedIdx = _catRangeStart;
        if (_selectedIdx >= n) _selectedIdx = n - 1;
        if (isEditable(_selectedIdx)) return;
        if (_selectedIdx == _catRangeStart && dir < 0) return;
        if (_selectedIdx == n - 1 && dir > 0) return;
    }
    _selectedIdx = start;
}

bool LvSettingsScreen::settingNeedsReboot(const SettingItem& item) const {
    if (!_cfg) return false;
    const auto& s = _cfg->settings();
    if (labelEq(item.label, "WiFi Mode")) return s.wifiMode != _rebootSnap.wifiMode;
    if (labelEq(item.label, "LoRa Radio")) return loraSettingsChanged();
    if (labelEq(item.label, "WiFi Profile")) return s.wifiSTASelected != _rebootSnap.wifiSTASelected;
    if (isWiFiSSIDLabel(item.label) || isWiFiPasswordLabel(item.label)) return interfaceSettingsChanged();
    if (labelEq(item.label, "Scan Networks") || labelEq(item.label, "Forget Network")) return interfaceSettingsChanged();
    if (labelEq(item.label, "TCP Server") || labelEq(item.label, "Host") ||
        labelEq(item.label, "Port")) return tcpSettingsChanged();
    if (labelEq(item.label, "LAN Discovery")) return s.autoIfaceEnabled != _rebootSnap.autoIfaceEnabled;
    if (labelEq(item.label, "SD Message Store")) return storageSettingsChanged();
    return false;
}

bool LvSettingsScreen::categoryNeedsReboot(int catIdx) const {
    if (catIdx < 0 || catIdx >= (int)_categories.size()) return false;
    if (labelEq(_categories[catIdx].name, "LoRa")) {
        return loraSettingsChanged();
    }
    if (labelEq(_categories[catIdx].name, "Network")) {
        return interfaceSettingsChanged() || tcpSettingsChanged();
    }
    if (labelEq(_categories[catIdx].name, "Storage & Maintenance")) {
        return storageSettingsChanged();
    }
    return false;
}

bool LvSettingsScreen::confirmableAction(const SettingItem& item) const {
    return labelEq(item.label, "Developer Radio Controls")
        || labelEq(item.label, "Format SD Card")
        || labelEq(item.label, "Erase " DEVICE_NAME " SD Data")
        || labelEq(item.label, "Erase Device");
}

bool LvSettingsScreen::armedAction(const SettingItem& item) const {
    return (_confirmingInitSD && labelEq(item.label, "Format SD Card")) ||
        (_confirmingWipeSD && labelEq(item.label, "Erase " DEVICE_NAME " SD Data")) ||
        (_confirmingReset && labelEq(item.label, "Erase Device")) ||
        (_confirmingDevMode && labelEq(item.label, "Developer Radio Controls"));
}

bool LvSettingsScreen::destructiveAction(const SettingItem& item) const {
    return labelEq(item.label, "Format SD Card")
        || labelEq(item.label, "Erase " DEVICE_NAME " SD Data")
        || labelEq(item.label, "Erase Device");
}

const char* LvSettingsScreen::confirmationTitle() const {
    if (_confirmingInitSD) return "ARMED: FORMAT SD CARD";
    if (_confirmingWipeSD) return "ARMED: ERASE SD DATA";
    if (_confirmingReset) return "ARMED: ERASE DEVICE";
    if (_confirmingDevMode) return "ARMED: UNLOCK RF CONTROLS";
    return nullptr;
}

const char* LvSettingsScreen::confirmationDetail() const {
    if (_confirmingInitSD) return "Hold " BOARD_CONFIRM_INPUT_NAME " to format. Backspace cancels.";
    if (_confirmingWipeSD) return "Hold " BOARD_CONFIRM_INPUT_NAME " to erase SD data. Backspace cancels.";
    if (_confirmingReset) return "Hold " BOARD_CONFIRM_INPUT_NAME " to erase device. Backspace cancels.";
    if (_confirmingDevMode) return "Hold " BOARD_CONFIRM_INPUT_NAME " to unlock. Backspace cancels.";
    return nullptr;
}

bool LvSettingsScreen::hasPendingConfirmation() const {
    return _confirmingInitSD || _confirmingWipeSD || _confirmingReset || _confirmingDevMode;
}

void LvSettingsScreen::clearConfirmations() {
    _confirmingInitSD = false;
    _confirmingWipeSD = false;
    _confirmingReset = false;
    _confirmingDevMode = false;
}

void LvSettingsScreen::runFormatSD() {
    clearConfirmations();
    if (_service) _service->lifecycle(handheld::Operation::FormatSD);
}

void LvSettingsScreen::runWipeSD() {
    clearConfirmations();
    if (_service) _service->lifecycle(handheld::Operation::WipeSD);
}

void LvSettingsScreen::runFactoryReset() {
    clearConfirmations();
    if (_service) _service->lifecycle(handheld::Operation::FactoryReset);
}

void LvSettingsScreen::runEnableDevMode() {
    _confirmingDevMode = false;
    if (!_cfg) {
        rebuildItemList();
        return;
    }
    _cfg->settings().devMode = true;
    if (!_service) { _cfg->settings().devMode = false; return; }
    _service->applySettings([this](const handheld::Result& result) {
        buildItems();
        if (_scrollContainer && _view == SettingsView::ITEM_LIST) enterCategory(_categoryIdx);
        if (result.outcome == handheld::Outcome::Ok && _ui)
            _ui->lvStatusBar().showToast("Developer radio controls unlocked", 1500);
    });
}

void LvSettingsScreen::startFirmwareCheck() {
    pollFirmwareCheck();
    if (!_service || !_service->available()) {
        if (_ui) _ui->lvStatusBar().showToast("Device is shutting down", 2000);
        return;
    }
    if (!_service || !_service->status().wifi) {
        if (_ui) _ui->lvStatusBar().showToast("Connect WiFi to check firmware", 2500);
        return;
    }
    if (firmwareCheckRunning()) {
        if (_ui) _ui->lvStatusBar().showToast("Firmware check already running", 1200);
        return;
    }

    _fwCheckVersion[0] = '\0';
    _fwRequestGeneration = _fwViewGeneration;
    _fwCheckState = FirmwareCheckState::RUNNING;
    _fwCheckActive.store(true, std::memory_order_release);
    if (xTaskCreatePinnedToCore(
            firmwareCheckTask,
            "fw-check",
            6144,
            this,
            1,
            &_fwCheckTask,
            xPortGetCoreID()) != pdPASS) {
        _fwCheckTask = nullptr;
        _fwResultGeneration = _fwRequestGeneration;
        _fwCheckState = FirmwareCheckState::FAILED;
        _fwCheckActive.store(false, std::memory_order_release);
    } else if (_ui) {
        _ui->lvStatusBar().showToast("Checking firmware release...", 1200);
    }
}

void LvSettingsScreen::pollFirmwareCheck() {
    if (!_fwCheckTask || _fwCheckState.load(std::memory_order_acquire) == FirmwareCheckState::RUNNING)
        return;
    // ESP-IDF 4.4.7 deletes a suspended task pinned to the caller's core
    // synchronously, including TLS and stack storage. Keep admission closed
    // until this happens; publication alone does not release a task owner.
    if (eTaskGetState(_fwCheckTask) != eSuspended ||
        xTaskGetAffinity(_fwCheckTask) != xPortGetCoreID()) return;
    vTaskDelete(_fwCheckTask);
    _fwCheckTask = nullptr;
    _fwCheckActive.store(false, std::memory_order_release);
}

void LvSettingsScreen::firmwareCheckTask(void* arg) {
    auto* self = static_cast<LvSettingsScreen*>(arg);
    if (!self) {
        vTaskDelete(nullptr);
        return;
    }

    FirmwareCheckState result = FirmwareCheckState::FAILED;
    char version[sizeof(self->_fwCheckVersion)] = {};

    // The helper returns before this FreeRTOS task is retired, so its
    // network, JSON and String owners always run their destructors.
    const auto checked = handheld::checkFirmwareRelease(
        BOARD_RELEASE_REPO, RSDECK_VERSION_STRING, version, sizeof(version));
    if (checked == handheld::ReleaseCheckResult::Available) result = FirmwareCheckState::AVAILABLE;
    else if (checked == handheld::ReleaseCheckResult::Current) result = FirmwareCheckState::CURRENT;

    if (version[0]) strlcpy(self->_fwCheckVersion, version, sizeof(self->_fwCheckVersion));
    self->_fwResultGeneration = self->_fwRequestGeneration;
    self->_fwCheckState.store(result, std::memory_order_release);
    for (;;) vTaskSuspend(nullptr);
}

void LvSettingsScreen::buildItems() {
    _items.clear();
    _categories.clear();
    if (!_cfg) return;
    auto& s = _cfg->settings();
    int idx = 0;

    // Identity & Device
    int devStart = idx;
    _items.push_back({"Firmware", SettingType::READONLY, nullptr, nullptr,
        [](int) { return String(RSDECK_VERSION_STRING)
#ifdef BOARD_BETA_LABEL
            + " / " BOARD_BETA_LABEL
#endif
            ; }});
    idx++;
#if defined(HAS_RNODE_MODE) && !HAS_RNODE_MODE
    _items.push_back({"RNode", SettingType::READONLY, nullptr, nullptr,
        [](int) { return String("Coming soon"); }});
    idx++;
#endif
    _items.push_back({"LXMF Address", SettingType::READONLY, nullptr, nullptr,
        [this](int) { return _destinationHash.length() > 0 ? _destinationHash : String("unknown"); }});
    idx++;
    _items.push_back({"Identity Hash", SettingType::READONLY, nullptr, nullptr,
        [this](int) { return _identityHash; }});
    idx++;
    {
        SettingItem nameItem;
        nameItem.label = "Display Name";
        nameItem.type = SettingType::TEXT_INPUT;
        nameItem.textGetter = [&s]() -> const String& { return s.displayName; };
        nameItem.textSetter = [&s](const String& v) {
            return UserConfig::trySetString(s.displayName, v.c_str(), v.length());
        };
        nameItem.maxTextLen = 16;
        _items.push_back(nameItem);
        idx++;
    }
    if (_service && !_service->identities().empty()) {
        SettingItem idSwitch;
        idSwitch.label = "Identity Slot";
        idSwitch.type = SettingType::ENUM_CHOICE;
        idSwitch.getter = [this]() { return _service->activeIdentity(); };
        idSwitch.setter = [this](int v) {
            if (!_service || v < 0 || size_t(v) >= _identityHashes.size()) return;
            _service->lifecycle(handheld::Operation::SwitchIdentity, _identityHashes[v]);
        };
        idSwitch.minVal = 0;
        idSwitch.maxVal = int(_service->identities().size()) - 1;
        idSwitch.step = 1;
        _identityHashes.clear();
        for (int i = 0; i < int(_service->identities().size()); i++) {
            const auto& slot = _service->identities()[i];
            _identityHashes.push_back(slot.hash);
            static char labelBufs[8][32];
            if (!slot.displayName.isEmpty()) {
                snprintf(labelBufs[i], sizeof(labelBufs[i]), "%s [%.8s]",
                         slot.displayName.c_str(), slot.hash.c_str());
            } else {
                snprintf(labelBufs[i], sizeof(labelBufs[i]), "%.12s", slot.hash.c_str());
            }
            idSwitch.enumLabels.push_back(labelBufs[i]);
        }
        _items.push_back(idSwitch);
        idx++;
    }
    {
        SettingItem newId;
        newId.label = "Create Identity";
        newId.type = SettingType::ACTION;
        newId.formatter = [](int) { return String("[Enter]"); };
        newId.action = [this]() {
            if (_service) _service->action(handheld::Operation::CreateIdentity);
        };
        _items.push_back(newId);
        idx++;
    }
    if (_service && _service->status().sd) {
        SettingItem importId;
        importId.label = "Import Identity";
        importId.type = SettingType::ACTION;
        importId.formatter = [](int) { return String("[Enter]"); };
        importId.action = [this]() {
            if (_service) _service->action(handheld::Operation::ImportIdentity);
        };
        _items.push_back(importId);
        idx++;
    }
    _items.push_back({"Auto Announce", SettingType::INTEGER,
        [&s]() { return s.announceInterval; }, [&s](int v) { s.announceInterval = v; },
        [](int v) { return v == 0 ? String("OFF") : String(v) + "m"; },
        handheld::announce::MinimumMinutes, handheld::announce::MaximumMinutes,
        handheld::announce::StepMinutes});
    _items.back().allowOff = true;
    idx++;
    _categories.push_back({"Identity & Device", devStart, idx - devStart,
        [&s]() -> String {
            String name = s.displayName.isEmpty() ? String("Unnamed device") : s.displayName;
            if (s.devMode) name += " / Dev Enabled";
            return name;
        }});

    // Screen & Input
    int dispStart = idx;
    _items.push_back({"Screen Brightness", SettingType::INTEGER,
        [&s]() { return s.brightness; }, [&s](int v) { s.brightness = v; },
        [](int v) { return String(v) + "%"; }, 1, 100, 5});
    idx++;
    {
        SettingItem themeItem;
        themeItem.label = "Theme";
        themeItem.type = SettingType::ENUM_CHOICE;
        themeItem.getter = [&s]() { return s.themeLight ? 1 : 0; };
        themeItem.setter = [&s](int v) { s.themeLight = (v != 0); };
        themeItem.minVal = 0; themeItem.maxVal = 1; themeItem.step = 1;
        themeItem.enumLabels = {"Dark", "Light"};
        _items.push_back(themeItem);
        idx++;
    }
    _items.push_back({"Dim After", SettingType::INTEGER,
        [&s]() { return s.screenDimTimeout; }, [&s](int v) { s.screenDimTimeout = v; },
        [](int v) { return String(v) + "s"; }, 5, 3600, 5});
    idx++;
    _items.push_back({"Display Off After", SettingType::INTEGER,
        [&s]() { return s.screenOffTimeout; }, [&s](int v) { s.screenOffTimeout = v; },
        [](int v) { return String(v) + "s"; }, 10, 7200, 10});
    idx++;
    _items.push_back({"Key Backlight", SettingType::ENUM_CHOICE,
        [&s]() { return keyboardBacklightChoice(s.keyboardBrightness); },
        [&s](int v) { s.keyboardBrightness = keyboardBacklightPercent(v); },
        nullptr, 0, 2, 1, {"OFF", "LOW", "HIGH"}, {}, {}, {}});
    idx++;
    _items.push_back({"Keys Wake Light", SettingType::TOGGLE,
        [&s]() { return s.keyboardAutoOn ? 1 : 0; },
        [&s](int v) { s.keyboardAutoOn = (v != 0); },
        [](int v) { return String(onOff(v != 0)); }});
    idx++;
    _items.push_back({"Keys Auto-Off", SettingType::TOGGLE,
        [&s]() { return s.keyboardAutoOff ? 1 : 0; },
        [&s](int v) { s.keyboardAutoOff = (v != 0); },
        [](int v) { return String(onOff(v != 0)); }});
    idx++;
#if HAS_TRACKBALL || HAS_SCROLLWHEEL
    // Same persisted field/JSON key on all boards; label follows the pointer device
#if HAS_SCROLLWHEEL
    _items.push_back({"Encoder Speed", SettingType::INTEGER,
#else
    _items.push_back({"Trackball Speed", SettingType::INTEGER,
#endif
        [&s]() { return s.trackballSpeed; }, [&s](int v) { s.trackballSpeed = v; },
        [](int v) { return String(v); }, 1, 5, 1});
    idx++;
#endif
    // Input help is reachable with the physical pointer and Enter on boards
    // whose stock translated keyboard does not report Ctrl shortcuts.
    {
        SettingItem helpItem;
        helpItem.label = "Input Help";
        helpItem.type = SettingType::ACTION;
        helpItem.formatter = [](int) { return String("[Enter]"); };
        helpItem.action = [this]() { if (_showHelpCb) _showHelpCb(); };
        _items.push_back(helpItem);
        idx++;
    }
    _categories.push_back({"Screen & Input", dispStart, idx - dispStart,
        [&s]() -> String {
            String summary = String("Screen ") + String(s.brightness);
            summary += "% / Keys ";
            summary += keyboardBacklightLabel(s.keyboardBrightness);
            return summary;
        }});

    // Both boards choose status-bar presentation. Only ADC-model boards expose
    // discharge calibration; a fuel gauge already supplies its own estimate.
    int battStart = idx;
    _items.push_back({"Battery Display", SettingType::ENUM_CHOICE,
        [&s]() { return (int)s.batteryDisplay; },
        [&s](int v) { s.batteryDisplay = (uint8_t)v; },
        nullptr, BATTERY_DISPLAY_PERCENT, BATTERY_DISPLAY_BAR, 1, {"Percent", "Bar"}});
    idx++;

    _items.push_back({"Estimated Charge", SettingType::READONLY, nullptr, nullptr,
        [this](int) -> String {
            if (!_power) return String("--");
            return String(_power->batteryPercent()) + "%";
        }});
    idx++;

#if HAS_BATTERY_MODEL
    if (s.devMode) {
        _items.push_back({"Discharge Curve", SettingType::ENUM_CHOICE,
            [&s]() { return (int)s.batteryModel; },
            [&s](int v) { s.batteryModel = (uint8_t)v; },
            nullptr, BATTERY_MODEL_LIPO, BATTERY_MODEL_LINEAR, 1, {"LiPo / Li-Ion", "Linear"}});
        idx++;

        _items.push_back({"Charge Above", SettingType::INTEGER,
            [&s]() { return (int)roundf(s.chargeThresholdV * 100); },
            [&s](int v) { s.chargeThresholdV = v / 100.0f; },
            [](int v) -> String {
                char buf[8]; snprintf(buf, sizeof(buf), "%.2fV", v / 100.0f); return String(buf);
            }, 380, 430, 1});
        idx++;
        _items.push_back({"Full Voltage", SettingType::INTEGER,
            [&s]() { return (int)roundf(s.fullBatteryV * 100); },
            [&s](int v) { s.fullBatteryV = v / 100.0f; },
            [](int v) -> String {
                char buf[8]; snprintf(buf, sizeof(buf), "%.2fV", v / 100.0f); return String(buf);
            }, 350, 420, 1});
        idx++;

        _items.push_back({"Voltage", SettingType::READONLY, nullptr, nullptr,
            [this](int) -> String {
                if (!_power) return String("--");
                char buf[12];
                snprintf(buf, sizeof(buf), "%.2fV", _power->batteryVoltage());
                return String(buf);
            }});
        idx++;
    }

#endif
    _categories.push_back({"Battery", battStart, idx - battStart,
        [this, &s]() -> String {
            String summary = s.batteryDisplay == BATTERY_DISPLAY_PERCENT ? String("Percent") : String("Bar");
            if (_power) {
                summary += " / ";
                char buf[8];
                snprintf(buf, sizeof(buf), "%d%%", _power->batteryPercent());
                summary += buf;
            }
            return summary;
        }});

    // LoRa link
    int radioStart = idx;
    _items.push_back({"LoRa Radio", SettingType::TOGGLE,
        [&s]() { return s.loraEnabled ? 1 : 0; },
        [&s](int v) { s.loraEnabled = (v != 0); },
        [](int v) { return String(onOff(v != 0)); }});
    idx++;
    {
        // Explicitly choosing a region restores its default frequency.
        SettingItem regionItem;
        regionItem.label = "Region Preset";
        regionItem.type = SettingType::ENUM_CHOICE;
        regionItem.getter = [&s]() {
            int region = constrain(s.radioRegion, 0, REGION_COUNT - 1);
            return s.loraFrequency == REGION_FREQ[region] ? region : (int)REGION_COUNT;
        };
        regionItem.setter = [&s](int v) {
            if (v < 0 || v >= REGION_COUNT) return; // Keep the displayed Custom value.
            s.radioRegion = v;
            s.loraFrequency = REGION_FREQ[v];
        };
        regionItem.minVal = 0; regionItem.maxVal = REGION_COUNT - 1; regionItem.step = 1;
        regionItem.enumLabels = {REGION_LABELS[0], REGION_LABELS[1], REGION_LABELS[2], REGION_LABELS[3], "Custom"};
        _items.push_back(regionItem);
        idx++;

        SettingItem presetItem;
        presetItem.label = "Link Preset";
        presetItem.type = SettingType::ENUM_CHOICE;
        presetItem.getter = [this]() { int p = detectPreset(); return (p >= 0) ? p : RadioPresets::count; };
        presetItem.setter = [this](int v) { if (v >= 0 && v < RadioPresets::count) applyPreset(v); };
        presetItem.minVal = 0; presetItem.maxVal = RadioPresets::count - 1; presetItem.step = 1;
        presetItem.enumLabels = {};
        for (int i = 0; i < RadioPresets::count; i++)
            presetItem.enumLabels.push_back(RadioPresets::values[i].name);
        presetItem.enumLabels.push_back("Custom");
        _items.push_back(presetItem);
        idx++;
    }
    {
        SettingItem devModeItem;
        devModeItem.label = "Developer Radio Controls";
        devModeItem.type = SettingType::ACTION;
        devModeItem.formatter = [this, &s](int) {
            if (_confirmingDevMode) return String("Hold");
            return s.devMode ? String("Unlocked") : String("Locked");
        };
        devModeItem.action = [this, &s]() {
            if (s.devMode) {
                s.devMode = false;
                _confirmingDevMode = false;
                clearConfirmations();
                applyAndSave();
                buildItems();
                enterCategory(_categoryIdx);
                return;
            }
            if (!_confirmingDevMode) {
                clearConfirmations();
                _confirmingDevMode = true;
                if (_ui) _ui->lvStatusBar().showToast("RF controls armed. Hold to unlock", 5000);
                rebuildItemList();
                return;
            }
            if (_ui) _ui->lvStatusBar().showToast("Hold " BOARD_CONFIRM_INPUT_NAME " to unlock RF controls", 2500);
        };
        _items.push_back(devModeItem);
        idx++;
    }
    // Custom radio parameters - only visible in Developer Mode
    if (s.devMode) {
        _items.push_back({"Frequency", SettingType::INTEGER,
            [&s]() { return (int)(s.loraFrequency); },
            [&s](int v) { s.loraFrequency = (uint32_t)v; },
            [](int v) { return formatRadioFrequency(v); },
            LORA_MIN_FREQUENCY, LORA_MAX_FREQUENCY, 125000});
        idx++;
        _items.push_back({"TX Power", SettingType::INTEGER,
            [&s]() { return s.loraTxPower; }, [&s](int v) { s.loraTxPower = v; },
            [](int v) { return String(v) + " dBm"; }, -9, 22, 1});
        idx++;
        _items.push_back({"Spread Factor", SettingType::INTEGER,
            [&s]() { return s.loraSF; }, [&s](int v) { s.loraSF = v; },
            [](int v) { return String("SF") + String(v); }, 5, 12, 1});
        idx++;
        _items.push_back({"Bandwidth", SettingType::ENUM_CHOICE,
            [&s]() { return RadioBandwidth::index(s.loraBW); },
            [&s](int v) { s.loraBW = RadioBandwidth::values[constrain(v, 0, RadioBandwidth::count - 1)].hz; },
            nullptr, 0, RadioBandwidth::count - 1, 1,
            {"7.8k", "10.4k", "15.6k", "20.8k", "31.25k", "41.7k", "62.5k", "125k", "250k", "500k"}});
        idx++;
        _items.push_back({"Coding Rate", SettingType::INTEGER,
            [&s]() { return s.loraCR; }, [&s](int v) { s.loraCR = v; },
            [](int v) { return String("4/") + String(v); }, 5, 8, 1});
        idx++;
        _items.push_back({"Preamble", SettingType::INTEGER,
            [&s]() { return (int)s.loraPreamble; }, [&s](int v) { s.loraPreamble = v; },
            [](int v) { return String(v); }, 6, 65, 1});
        idx++;
    }
    _categories.push_back({"LoRa", radioStart, idx - radioStart,
        [this]() {
            if (loraSettingsChanged()) {
                return String("Saved - reboot to apply");
            }
            int p = detectPreset();
            auto& s = _cfg->settings();
            if (!s.loraEnabled) return String("Off");
            String label = (p >= 0) ? String(RadioPresets::values[p].name) : String("Custom");
            label += " ";
            label += formatRadioFrequency(s.loraFrequency);
            if (s.devMode) label += " / Dev";
            return label;
        }});

    // Network
    int netStart = idx;
    _items.push_back({"WiFi Mode", SettingType::ENUM_CHOICE,
        [&s]() { return (int)s.wifiMode; },
        [&s](int v) {
            s.wifiMode = (RatWiFiMode)v;
            if (s.wifiMode != RAT_WIFI_OFF) s.wifiRestoreMode = s.wifiMode;
        },
        nullptr, 0, 2, 1, {"Off", "Hotspot", "Client"}});
    idx++;
    {
        SettingItem scanItem;
        scanItem.label = "Scan Networks";
        scanItem.type = SettingType::ACTION;
        scanItem.formatter = [](int) { return String("[Enter]"); };
        scanItem.action = [this, &s]() {
            _wifiTargetSlot = selectedWiFiSlot(s);
            showWifiPicker();
        };
        _items.push_back(scanItem);
        idx++;
    }
    _items.push_back({"WiFi Profile", SettingType::INTEGER,
        [&s]() { return (int)selectedWiFiSlot(s) + 1; },
        [&s](int v) { s.wifiSTASelected = (uint8_t)constrain(v - 1, 0, (int)WIFI_STA_MAX_NETWORKS - 1); },
        [&s](int v) { return wifiProfileValue(s, constrain(v - 1, 0, (int)WIFI_STA_MAX_NETWORKS - 1)); },
        1, (int)WIFI_STA_MAX_NETWORKS, 1});
    idx++;
    {
        SettingItem ssidItem;
        ssidItem.label = "WiFi SSID";
        ssidItem.type = SettingType::TEXT_INPUT;
        ssidItem.textGetter = [&s]() -> const String& {
            static const String empty;
            size_t slot = selectedWiFiSlot(s);
            return slot < s.wifiSTANetworks.size() ? s.wifiSTANetworks[slot].ssid : empty;
        };
        ssidItem.textSetter = [&s](const String& v) {
            size_t slot = selectedWiFiSlot(s);
            ensureWiFiSlot(s, slot);
            if (slot >= s.wifiSTANetworks.size()) return false;
            const bool changed = s.wifiSTANetworks[slot].ssid != v;
            if (!UserConfig::trySetString(s.wifiSTANetworks[slot].ssid, v.c_str(), v.length())) return false;
            if (changed || v.isEmpty()) s.wifiSTANetworks[slot].password = "";
            return true;
        };
        ssidItem.maxTextLen = 32;
        _items.push_back(ssidItem);
        idx++;
    }
    {
        SettingItem passItem;
        passItem.label = "WiFi Password";
        passItem.type = SettingType::TEXT_INPUT;
        passItem.textGetter = [&s]() -> const String& {
            static const String empty;
            size_t slot = selectedWiFiSlot(s);
            return slot < s.wifiSTANetworks.size() ? s.wifiSTANetworks[slot].password : empty;
        };
        passItem.textSetter = [&s](const String& v) {
            size_t slot = selectedWiFiSlot(s);
            ensureWiFiSlot(s, slot);
            if (slot >= s.wifiSTANetworks.size()) return false;
            return UserConfig::trySetString(s.wifiSTANetworks[slot].password, v.c_str(), v.length());
        };
        passItem.maxTextLen = 64;
        _items.push_back(passItem);
        idx++;
    }
    {
        SettingItem forgetItem;
        forgetItem.label = "Forget Network";
        forgetItem.type = SettingType::ACTION;
        forgetItem.formatter = [&s](int) {
            String ssid = selectedWiFiSSID(s);
            return ssid.isEmpty() ? String("Empty") : String("[Enter]");
        };
        forgetItem.action = [this, &s]() {
            size_t slot = selectedWiFiSlot(s);
            if (slot >= s.wifiSTANetworks.size() || s.wifiSTANetworks[slot].ssid.isEmpty()) {
                if (_ui) _ui->lvStatusBar().showToast("WiFi profile already empty", 1200);
                return;
            }
            s.wifiSTANetworks[slot].ssid = "";
            s.wifiSTANetworks[slot].password = "";
            applyAndSave();
        };
        _items.push_back(forgetItem);
        idx++;
    }
    {
        SettingItem tcpPreset;
        tcpPreset.label = "TCP Server";
        tcpPreset.type = SettingType::ENUM_CHOICE;
        tcpPreset.getter = [&s]() {
            for (auto& ep : s.tcpConnections) {
                if (!ep.autoConnect || ep.host.isEmpty()) continue;
                if (ep.host == RATSPEAK_HUB_HOST) return 1;
                return 2;
            }
            return 0;
        };
        tcpPreset.setter = [&s](int v) {
            if (v == 0) {
                for (auto& ep : s.tcpConnections) ep.autoConnect = false;
            }
            else if (v == 1) {
                TCPEndpoint ep;
                if (!UserConfig::trySetString(ep.host, RATSPEAK_HUB_HOST, sizeof(RATSPEAK_HUB_HOST) - 1)) throw std::bad_alloc();
                ep.port = TCP_DEFAULT_PORT; ep.autoConnect = true;
                std::vector<TCPEndpoint> prepared;
                prepared.push_back(std::move(ep));
                s.tcpConnections.swap(prepared);
            } else if (v == 2 && s.tcpConnections.empty()) {
                TCPEndpoint ep; ep.port = TCP_DEFAULT_PORT; ep.autoConnect = false;
                s.tcpConnections.push_back(std::move(ep));
            }
        };
        tcpPreset.minVal = 0; tcpPreset.maxVal = 2; tcpPreset.step = 1;
        tcpPreset.enumLabels = {"None", "Ratspeak Hub", "Custom"};
        _items.push_back(tcpPreset);
        idx++;
    }
    {
        SettingItem tcpHost;
        tcpHost.label = "Host";
        tcpHost.type = SettingType::TEXT_INPUT;
        tcpHost.textGetter = [&s]() -> const String& {
            static const String empty;
            return s.tcpConnections.empty() ? empty : s.tcpConnections[0].host;
        };
        tcpHost.textSetter = [&s](const String& v) {
            if (s.tcpConnections.empty()) {
                TCPEndpoint ep;
                if (!UserConfig::trySetString(ep.host, v.c_str(), v.length())) return false;
                ep.port = TCP_DEFAULT_PORT; ep.autoConnect = true;
                s.tcpConnections.push_back(std::move(ep));
                return true;
            }
            return UserConfig::trySetString(s.tcpConnections[0].host, v.c_str(), v.length());
        };
        tcpHost.maxTextLen = 40;
        _items.push_back(tcpHost);
        idx++;
    }
    _items.push_back({"Port", SettingType::INTEGER,
        [&s]() { return s.tcpConnections.empty() ? TCP_DEFAULT_PORT : (int)s.tcpConnections[0].port; },
        [&s](int v) {
            if (s.tcpConnections.empty()) {
                TCPEndpoint ep; ep.port = v; ep.autoConnect = true;
                s.tcpConnections.push_back(std::move(ep));
            } else { s.tcpConnections[0].port = v; }
        },
        [](int v) { return String(v); }, 1, 65535, 1});
    idx++;
    _items.push_back({"LAN Discovery", SettingType::TOGGLE,
        [&s]() { return s.autoIfaceEnabled ? 1 : 0; },
        [&s](int v) { s.autoIfaceEnabled = (v != 0); },
        [](int v) { return String(onOff(v != 0)); }});
    idx++;
    _categories.push_back({"Network", netStart, idx - netStart,
        [this, &s]() {
            if (interfaceSettingsChanged() || tcpSettingsChanged()) return String("Saved - reboot to apply");
            String summary = wifiModeLabel(s.wifiMode);
            if (s.wifiMode == RAT_WIFI_STA) {
                String ssid = selectedWiFiSSID(s);
                summary += ": ";
                summary += ssid.isEmpty() ? String("No profile") : ssid;
            }
            if (!s.tcpConnections.empty()) summary += " + TCP";
            if (s.autoIfaceEnabled) summary += " + LAN";
            return summary;
        }});

    // Time & Location
    int gpsStart = idx;
#if HAS_GPS
    _items.push_back({"GPS Time Sync", SettingType::TOGGLE,
        [&s]() { return s.gpsTimeEnabled ? 1 : 0; },
        [&s](int v) { s.gpsTimeEnabled = (v != 0); },
        [](int v) { return String(onOff(v != 0)); }});
    idx++;
    _items.push_back({"GPS Location", SettingType::TOGGLE,
        [&s]() { return s.gpsLocationEnabled ? 1 : 0; },
        [&s](int v) { s.gpsLocationEnabled = (v != 0); },
        [](int v) { return String(onOff(v != 0)); }});
    idx++;
#endif
    _items.push_back({"Timezone", SettingType::INTEGER,
        [&s]() { return (int)s.timezoneIdx; },
        [&s](int v) { s.timezoneIdx = (uint8_t)v; s.timezoneSet = true; },
        [](int v) {
            if (v >= 0 && v < TIMEZONE_COUNT) return String(TIMEZONE_TABLE[v].label);
            return String("Unknown");
        },
        0, TIMEZONE_COUNT - 1, 1});
    idx++;
    _items.push_back({"24-Hour Clock", SettingType::TOGGLE,
        [&s]() { return s.use24HourTime ? 1 : 0; },
        [&s](int v) { s.use24HourTime = (v != 0); },
        [](int v) { return String(onOff(v != 0)); }});
    idx++;
    _categories.push_back({"Time & Location", gpsStart, idx - gpsStart,
        [&s]() {
            if (s.timezoneIdx < TIMEZONE_COUNT)
                return String(TIMEZONE_TABLE[s.timezoneIdx].label);
            return String("Not set");
        }});

    // Alerts & Audio
    int audioStart = idx;
    _items.push_back({"Audio Alerts", SettingType::TOGGLE,
        [&s]() { return s.audioEnabled ? 1 : 0; },
        [&s](int v) { s.audioEnabled = (v != 0); },
        [](int v) { return String(onOff(v != 0)); }});
    idx++;
    _items.push_back({"Volume", SettingType::INTEGER,
        [&s]() { return s.audioVolume; }, [&s](int v) { s.audioVolume = v; },
        [](int v) { return String(v) + "%"; }, 0, 100, 10});
    idx++;
    _categories.push_back({"Alerts & Audio", audioStart, idx - audioStart,
        [&s]() -> String {
            if (!s.audioEnabled) return String("Muted");
            String summary = String("Volume ") + String(s.audioVolume);
            summary += "%";
            return summary;
        }});

    // Diagnostics
    int infoStart = idx;
    _items.push_back({"RNS Transport", SettingType::READONLY, nullptr, nullptr,
        [this](int) { return _backend && _backend->isTransportActive() ? String("ACTIVE") : String("OFFLINE"); }});
    idx++;
    _items.push_back({"Known Paths", SettingType::READONLY, nullptr, nullptr,
        [this](int) { return _backend ? String((int)_backend->pathCount()) : String("0"); }});
    idx++;
    _items.push_back({"Live Links", SettingType::READONLY, nullptr, nullptr,
        [this](int) { return _backend ? String((int)_backend->linkCount()) : String("0"); }});
    idx++;
    _items.push_back({"LoRa Driver", SettingType::READONLY, nullptr, nullptr,
        [this](int) {
            if (_service && _service->status().radio) {
                char buf[32];
                snprintf(buf, sizeof(buf), "SF%d BW%luk %ddBm",
                    _service->status().sf,
                    (unsigned long)(_service->status().bandwidth / 1000),
                    _service->status().txPower);
                return String(buf);
            }
            return String("OFFLINE");
        }});
    idx++;
    _items.push_back({"Heap", SettingType::READONLY, nullptr, nullptr,
        [](int) { return String((unsigned long)(ESP.getFreeHeap() / 1024)) + " KB"; }});
    idx++;
    _items.push_back({"PSRAM", SettingType::READONLY, nullptr, nullptr,
        [](int) { return String((unsigned long)(ESP.getFreePsram() / 1024)) + " KB"; }});
    idx++;
    _items.push_back({"Uptime", SettingType::READONLY, nullptr, nullptr,
        [](int) -> String {
            unsigned long m = millis() / 60000;
            if (m >= 60) {
                char buf[16];
                snprintf(buf, sizeof(buf), "%luh %lum", m / 60, m % 60);
                return String(buf);
            }
            return String(m) + "m";
        }});
    idx++;
    _categories.push_back({"Diagnostics", infoStart, idx - infoStart,
        [this]() -> String {
            if (!_backend || !_backend->isTransportActive()) return String("Transport offline");
            String summary = String("Paths ") + String((int)_backend->pathCount());
            summary += " / Links ";
            summary += String((int)_backend->linkCount());
            return summary;
        }});

    // Storage & Maintenance
    int sysStart = idx;
    _items.push_back({"Free Heap", SettingType::READONLY, nullptr, nullptr,
        [](int) { return String((unsigned long)(ESP.getFreeHeap() / 1024)) + " KB"; }});
    idx++;
    _items.push_back({"Free PSRAM", SettingType::READONLY, nullptr, nullptr,
        [](int) { return String((unsigned long)(ESP.getFreePsram() / 1024)) + " KB"; }});
    idx++;
    _items.push_back({"Flash", SettingType::READONLY, nullptr, nullptr,
        [this](int) {
            if (!_service || !_service->status().flash) return String("Error");
            char buf[24];
            snprintf(buf, sizeof(buf), "%lu/%lu KB",
                     (unsigned long)(_service->status().flashUsed / 1024),
                     (unsigned long)(_service->status().flashTotal / 1024));
            return String(buf);
        }});
    idx++;
    _items.push_back({"SD Card", SettingType::READONLY, nullptr, nullptr,
        [this](int) { return _service && _service->status().sd ? String("Ready") : String("Not Found"); }});
    idx++;
    _items.push_back({"SD Message Store", SettingType::TOGGLE,
        [&s]() { return s.sdStorageEnabled ? 1 : 0; },
        [&s](int v) { s.sdStorageEnabled = (v != 0); },
        [](int v) { return String(onOff(v != 0)); }});
    idx++;
    {
        SettingItem announceItem;
        announceItem.label = "Send Announce";
        announceItem.type = SettingType::ACTION;
        announceItem.formatter = [](int) { return String("[Enter]"); };
        announceItem.action = [this]() {
            if (_service) _service->action(handheld::Operation::Announce);
        };
        _items.push_back(announceItem);
        idx++;
    }
    {
        SettingItem showQrItem;
        showQrItem.label = "Show QR Code";
        showQrItem.type = SettingType::ACTION;
        showQrItem.formatter = [](int) { return String("[Enter]"); };
        showQrItem.action = [this]() {
            if (_showQrCb) _showQrCb();
            else if (_ui) _ui->lvStatusBar().showToast("QR not available");
        };
        _items.push_back(showQrItem);
        idx++;
    }
    {
        SettingItem initSD;
        initSD.label = "Format SD Card";
        initSD.type = SettingType::ACTION;
        initSD.formatter = [this](int) {
            if (!_service || !_service->status().sd) return String("No Card");
            return _confirmingInitSD ? String("Hold") : String("Arm");
        };
        initSD.action = [this]() {
            if (!_service || !_service->status().sd) { if (_ui) _ui->lvStatusBar().showToast("No SD card!", 1200); return; }
            if (!_confirmingInitSD) {
                clearConfirmations();
                _confirmingInitSD = true;
                if (_ui) _ui->lvStatusBar().showToast("Format SD armed. Hold to confirm", 5000);
                rebuildItemList();
                return;
            }
            if (_ui) _ui->lvStatusBar().showToast("Hold " BOARD_CONFIRM_INPUT_NAME " to format SD", 2500);
        };
        _items.push_back(initSD);
        idx++;
    }
    {
        SettingItem wipeSD;
        wipeSD.label = "Erase " DEVICE_NAME " SD Data";
        wipeSD.type = SettingType::ACTION;
        wipeSD.formatter = [this](int) {
            if (!_service || !_service->status().sd) return String("No Card");
            return _confirmingWipeSD ? String("Hold") : String("Arm");
        };
        wipeSD.action = [this]() {
            if (!_service || !_service->status().sd) { if (_ui) _ui->lvStatusBar().showToast("No SD card!", 1200); return; }
            if (!_confirmingWipeSD) {
                clearConfirmations();
                _confirmingWipeSD = true;
                if (_ui) _ui->lvStatusBar().showToast("SD erase armed. Hold to confirm", 5000);
                rebuildItemList();
                return;
            }
            if (_ui) _ui->lvStatusBar().showToast("Hold " BOARD_CONFIRM_INPUT_NAME " to erase SD data", 2500);
        };
        _items.push_back(wipeSD);
        idx++;
    }
    {
        SettingItem factoryReset;
        factoryReset.label = "Erase Device";
        factoryReset.type = SettingType::ACTION;
        factoryReset.formatter = [this](int) { return _confirmingReset ? String("Hold") : String("Arm"); };
        factoryReset.action = [this]() {
            if (!_confirmingReset) {
                clearConfirmations();
                _confirmingReset = true;
                if (_ui) _ui->lvStatusBar().showToast("Device erase armed. Hold to confirm", 5000);
                rebuildItemList();
                return;
            }
            if (_ui) _ui->lvStatusBar().showToast("Hold " BOARD_CONFIRM_INPUT_NAME " to erase device", 2500);
        };
        _items.push_back(factoryReset);
        idx++;
    }
    {
        SettingItem rebootItem;
        rebootItem.label = "Reboot Now";
        rebootItem.type = SettingType::ACTION;
        rebootItem.formatter = [](int) { return String("[Enter]"); };
        rebootItem.action = [this]() {
            if (_service) _service->lifecycle(handheld::Operation::Restart);
        };
        _items.push_back(rebootItem);
        idx++;
    }
    {
        SettingItem updateCheck;
        updateCheck.label = "Check Firmware";
        updateCheck.type = SettingType::ACTION;
        updateCheck.formatter = [this](int) {
            return _fwCheckState == FirmwareCheckState::RUNNING ? String("Checking") : String("[Enter]");
        };
        updateCheck.action = [this]() {
            startFirmwareCheck();
            rebuildItemList();
        };
        _items.push_back(updateCheck);
        idx++;
    }
    _categories.push_back({"Storage & Maintenance", sysStart, idx - sysStart,
        [this](){
            if (_rebootNeeded) return String("Reboot pending");
            return (_service && _service->status().sd) ? String("SD ready") : String("Flash only");
        }});
}

void LvSettingsScreen::createUI(lv_obj_t* parent) {
    _screen = parent;
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(parent, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_pad_all(parent, 0, 0);

    _scrollContainer = lv_obj_create(parent);
    lv_obj_set_size(_scrollContainer, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(_scrollContainer, 0, 0);
    lv_obj_set_style_bg_color(_scrollContainer, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_bg_opa(_scrollContainer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_scrollContainer, 0, 0);
    lv_obj_set_style_pad_all(_scrollContainer, 0, 0);
    lv_obj_set_style_pad_row(_scrollContainer, 0, 0);
    lv_obj_set_style_radius(_scrollContainer, 0, 0);
    lv_obj_set_layout(_scrollContainer, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(_scrollContainer, LV_FLEX_FLOW_COLUMN);
}

void LvSettingsScreen::onEnter() {
    ++_fwViewGeneration;
    buildItems();
    _rebootNeeded = rebootSettingsChanged();
    _view = SettingsView::CATEGORY_LIST;
    _categoryIdx = 0;
    _selectedIdx = 0;
    _editing = false;
    _textEditing = false;
    _wifiScanActive = false;
    _freqEditing = false;
    _numericTyping = false;
    _editValueLbl = nullptr;
    _wifiTargetSlot = 0;
    clearConfirmations();
    _kbBrightness = _cfg ? _cfg->settings().keyboardBrightness : 0;
    rebuildCategoryList();
}

void LvSettingsScreen::refreshUI() {
    FirmwareCheckState fwState = _fwCheckState.load(std::memory_order_acquire);
    if (!firmwareCheckRunning() && fwState != FirmwareCheckState::IDLE && fwState != FirmwareCheckState::RUNNING) {
        if (_ui && _fwResultGeneration == _fwViewGeneration) {
            if (fwState == FirmwareCheckState::AVAILABLE) {
                char msg[64];
                snprintf(msg, sizeof(msg), "v%s available at ratspeak.org", _fwCheckVersion);
                _ui->lvStatusBar().showToast(msg, 5000);
            } else if (fwState == FirmwareCheckState::CURRENT) {
                _ui->lvStatusBar().showToast("Firmware is current", 2000);
            } else {
                _ui->lvStatusBar().showToast("Couldn't fetch firmware", 2000);
            }
        }
        _fwCheckState = FirmwareCheckState::IDLE;
        if (_view == SettingsView::ITEM_LIST) rebuildItemList();
    }

    if (_service && !_editing && !_textEditing && !_freqEditing &&
        _lastIdentityRevision != _service->identityRevision()) {
        _lastIdentityRevision = _service->identityRevision();
        buildItems();
        if (_view == SettingsView::CATEGORY_LIST) rebuildCategoryList();
        else if (_view == SettingsView::ITEM_LIST) rebuildItemList();
    }
}

void LvSettingsScreen::showWifiPicker() {
    _wifiResults.clear();
    _wifiPickerIdx = 0;
    _wifiScanActive = true;
    _wifiScanOutcome = handheld::Outcome::NotReady;
    if (!_service || !_service->scan([this](const handheld::Result& result) {
        _wifiScanActive = false;
        _wifiScanOutcome = result.outcome;
        _wifiResults.clear();
        if (result.outcome == handheld::Outcome::Ok) {
            JsonDocument doc;
            if (deserializeJson(doc, _service->scanJson()) ||
                !doc.is<JsonArray>() || doc.size() > 15) {
                _wifiScanOutcome = handheld::Outcome::Failed;
            } else {
                for (JsonObject row : doc.as<JsonArray>()) {
                    if (!row["ssid"].is<const char*>() || !row["rssi"].is<int>() ||
                        !row["encrypted"].is<bool>()) {
                        _wifiResults.clear();
                        _wifiScanOutcome = handheld::Outcome::Failed;
                        break;
                    }
                    WiFiInterface::ScanResult network;
                    const char* ssid = row["ssid"].as<const char*>();
                    if (!UserConfig::trySetString(network.ssid, ssid, strlen(ssid))) {
                        _wifiResults.clear(); _wifiScanOutcome = handheld::Outcome::Failed; break;
                    }
                    network.rssi = row["rssi"].as<int>(); network.encrypted = row["encrypted"].as<bool>();
                    try { _wifiResults.push_back(std::move(network)); }
                    catch (const std::bad_alloc&) {
                        _wifiResults.clear(); _wifiScanOutcome = handheld::Outcome::Failed; break;
                    }
                }
            }
        }
        if (_screen && _view == SettingsView::WIFI_PICKER) rebuildWifiList();
    })) {
        _wifiScanActive = false;
        _wifiScanOutcome = handheld::Outcome::Failed;
    }
    _view = SettingsView::WIFI_PICKER;
    rebuildWifiList();
}

void LvSettingsScreen::rebuildCategoryList() {
    if (!_scrollContainer) return;
    _rowObjs.clear();
    lv_obj_clean(_scrollContainer);

    const lv_font_t* font = &lv_font_rsdeck_12;

    // Title
    lv_obj_t* titleRow = lv_obj_create(_scrollContainer);
    lv_obj_set_size(titleRow, Theme::CONTENT_W, 26);
    lv_obj_set_style_bg_color(titleRow, lv_color_hex(Theme::BG_ELEVATED), 0);
    lv_obj_set_style_bg_opa(titleRow, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(titleRow, lv_color_hex(Theme::BORDER), 0);
    lv_obj_set_style_border_width(titleRow, 1, 0);
    lv_obj_set_style_border_side(titleRow, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_all(titleRow, 0, 0);
    lv_obj_set_style_radius(titleRow, 0, 0);
    lv_obj_clear_flag(titleRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* titleLbl = lv_label_create(titleRow);
    lv_obj_set_style_text_font(titleLbl, font, 0);
    lv_obj_set_style_text_color(titleLbl, lv_color_hex(Theme::ACCENT), 0);
    lv_label_set_text(titleLbl, "SETTINGS");
    lv_obj_align(titleLbl, LV_ALIGN_LEFT_MID, 8, 0);

    if (_rebootNeeded) {
        lv_obj_t* pendingLbl = lv_label_create(titleRow);
        lv_obj_set_style_text_font(pendingLbl, &lv_font_rsdeck_10, 0);
        lv_obj_set_style_text_color(pendingLbl, lv_color_hex(Theme::WARNING_CLR), 0);
        lv_label_set_text(pendingLbl, "REBOOT PENDING");
        lv_obj_align(pendingLbl, LV_ALIGN_RIGHT_MID, -8, 0);
    }

    for (int i = 0; i < (int)_categories.size(); i++) {
        auto& cat = _categories[i];
        bool selected = (i == _categoryIdx);
        bool pending = categoryNeedsReboot(i);

        lv_obj_t* row = lv_obj_create(_scrollContainer);
        lv_obj_set_size(row, Theme::CONTENT_W, 40);
        lv_obj_add_style(row, LvTheme::styleListBtn(), 0);
        lv_obj_add_style(row, LvTheme::styleListBtnFocused(), LV_STATE_FOCUSED);
        lv_obj_set_style_bg_color(row, lv_color_hex(selected ? Theme::BG_HOVER : Theme::BG), 0);
        lv_obj_set_style_border_color(row, lv_color_hex(pending ? Theme::WARNING_CLR : Theme::BORDER), 0);
        lv_obj_set_style_border_width(row, pending ? 2 : 1, 0);
        lv_obj_set_style_border_side(row, pending ? (lv_border_side_t)(LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_BOTTOM) : LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, (void*)(intptr_t)i);
        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            auto* self = (LvSettingsScreen*)lv_event_get_user_data(e);
            int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
            self->_categoryIdx = idx;
            self->enterCategory(idx);
        }, LV_EVENT_CLICKED, this);
        lv_group_add_obj(LvInput::group(), row);
        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            lv_obj_scroll_to_view(lv_event_get_target(e), LV_ANIM_ON);
        }, LV_EVENT_FOCUSED, nullptr);

        // Category name
        lv_obj_t* nameLbl = lv_label_create(row);
        lv_obj_set_style_text_font(nameLbl, font, 0);
        lv_obj_set_style_text_color(nameLbl, lv_color_hex(pending ? Theme::WARNING_CLR : Theme::TEXT_PRIMARY), 0);
        clipLabel(nameLbl, Theme::CONTENT_W - 44);
        lv_label_set_text(nameLbl, cat.name);
        lv_obj_align(nameLbl, LV_ALIGN_TOP_LEFT, 12, 4);

        // Summary
        if (cat.summary) {
            lv_obj_t* sumLbl = lv_label_create(row);
            lv_obj_set_style_text_font(sumLbl, &lv_font_rsdeck_10, 0);
            lv_obj_set_style_text_color(sumLbl, lv_color_hex(pending ? Theme::WARNING_CLR : Theme::TEXT_MUTED), 0);
            clipLabel(sumLbl, Theme::CONTENT_W - 44);
            lv_label_set_text(sumLbl, cat.summary().c_str());
            lv_obj_align(sumLbl, LV_ALIGN_BOTTOM_LEFT, 20, -4);
        }

        // Arrow
        lv_obj_t* arrow = lv_label_create(row);
        lv_obj_set_style_text_font(arrow, font, 0);
        lv_obj_set_style_text_color(arrow, lv_color_hex(pending ? Theme::WARNING_CLR : (selected ? Theme::ACCENT : Theme::TEXT_MUTED)), 0);
        lv_label_set_text(arrow, pending ? "!" : ">");
        lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -8, 0);

        _rowObjs.push_back(row);
    }

    // Restore focus to current category
    if (_categoryIdx >= 0 && _categoryIdx < (int)_rowObjs.size()) {
        LvInput::focusObj(_rowObjs[_categoryIdx]);
    }
}

void LvSettingsScreen::rebuildItemList() {
    if (!_scrollContainer) return;
    _rowObjs.clear();
    _editValueLbl = nullptr;  // Invalidate cached label before destroying widgets
    lv_obj_clean(_scrollContainer);

    const lv_font_t* font = &lv_font_rsdeck_12;

    // Category header
    lv_obj_t* headerRow = lv_obj_create(_scrollContainer);
    lv_obj_set_size(headerRow, Theme::CONTENT_W, 24);
    lv_obj_set_style_bg_color(headerRow, lv_color_hex(Theme::BG_ELEVATED), 0);
    lv_obj_set_style_bg_opa(headerRow, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(headerRow, lv_color_hex(Theme::BORDER), 0);
    lv_obj_set_style_border_width(headerRow, 1, 0);
    lv_obj_set_style_border_side(headerRow, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_all(headerRow, 0, 0);
    lv_obj_set_style_radius(headerRow, 0, 0);
    lv_obj_clear_flag(headerRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(headerRow, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(headerRow, [](lv_event_t* e) {
        auto* self = (LvSettingsScreen*)lv_event_get_user_data(e);
        if (!self->cancelEditing()) self->exitToCategories();
    }, LV_EVENT_CLICKED, this);

    char headerBuf[48];
    if (_editing || _textEditing || _freqEditing) {
        snprintf(headerBuf, sizeof(headerBuf), "< Cancel edit");
    } else {
        snprintf(headerBuf, sizeof(headerBuf), "< %s", _categories[_categoryIdx].name);
    }
    lv_obj_t* headerLbl = lv_label_create(headerRow);
    lv_obj_set_style_text_font(headerLbl, font, 0);
    lv_obj_set_style_text_color(headerLbl, lv_color_hex(Theme::ACCENT), 0);
    clipLabel(headerLbl, Theme::CONTENT_W - 128);
    lv_label_set_text(headerLbl, headerBuf);
    lv_obj_align(headerLbl, LV_ALIGN_LEFT_MID, 8, 0);

    if (_rebootNeeded) {
        lv_obj_t* pendingLbl = lv_label_create(headerRow);
        lv_obj_set_style_text_font(pendingLbl, &lv_font_rsdeck_10, 0);
        lv_obj_set_style_text_color(pendingLbl, lv_color_hex(Theme::WARNING_CLR), 0);
        lv_label_set_text(pendingLbl, "REBOOT NEEDED");
        lv_obj_align(pendingLbl, LV_ALIGN_RIGHT_MID, -8, 0);
    }

    if (_rebootNeeded) {
        lv_obj_t* notice = lv_obj_create(_scrollContainer);
        lv_obj_set_size(notice, Theme::CONTENT_W, 20);
        lv_obj_set_style_bg_color(notice, lv_color_hex(Theme::PRIMARY_SUBTLE), 0);
        lv_obj_set_style_bg_opa(notice, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(notice, lv_color_hex(Theme::WARNING_CLR), 0);
        lv_obj_set_style_border_width(notice, 1, 0);
        lv_obj_set_style_border_side(notice, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_pad_all(notice, 0, 0);
        lv_obj_set_style_radius(notice, 0, 0);
        lv_obj_clear_flag(notice, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* noticeLbl = lv_label_create(notice);
        lv_obj_set_style_text_font(noticeLbl, &lv_font_rsdeck_10, 0);
        lv_obj_set_style_text_color(noticeLbl, lv_color_hex(Theme::WARNING_CLR), 0);
        clipLabel(noticeLbl, Theme::CONTENT_W - 16);
        lv_label_set_text(noticeLbl, "Saved interface config is pending reboot");
        lv_obj_align(noticeLbl, LV_ALIGN_LEFT_MID, 8, 0);
    }

    const char* confirmTitle = confirmationTitle();
    const char* confirmDetail = confirmationDetail();
    if (confirmTitle && confirmDetail) {
        uint32_t confirmColor = _confirmingDevMode ? Theme::WARNING_CLR : Theme::ERROR_CLR;
        lv_obj_t* confirm = lv_obj_create(_scrollContainer);
        lv_obj_set_size(confirm, Theme::CONTENT_W, 34);
        lv_obj_set_style_bg_color(confirm, lv_color_hex(Theme::BG_SURFACE), 0);
        lv_obj_set_style_bg_opa(confirm, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(confirm, lv_color_hex(confirmColor), 0);
        lv_obj_set_style_border_width(confirm, 2, 0);
        lv_obj_set_style_border_side(confirm, (lv_border_side_t)(LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_BOTTOM), 0);
        lv_obj_set_style_pad_all(confirm, 0, 0);
        lv_obj_set_style_radius(confirm, 0, 0);
        lv_obj_clear_flag(confirm, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* confirmTitleLbl = lv_label_create(confirm);
        lv_obj_set_style_text_font(confirmTitleLbl, &lv_font_rsdeck_12, 0);
        lv_obj_set_style_text_color(confirmTitleLbl, lv_color_hex(confirmColor), 0);
        clipLabel(confirmTitleLbl, Theme::CONTENT_W - 16);
        lv_label_set_text(confirmTitleLbl, confirmTitle);
        lv_obj_align(confirmTitleLbl, LV_ALIGN_TOP_LEFT, 8, 3);

        lv_obj_t* confirmDetailLbl = lv_label_create(confirm);
        lv_obj_set_style_text_font(confirmDetailLbl, &lv_font_rsdeck_10, 0);
        lv_obj_set_style_text_color(confirmDetailLbl, lv_color_hex(Theme::TEXT_PRIMARY), 0);
        clipLabel(confirmDetailLbl, Theme::CONTENT_W - 16);
        lv_label_set_text(confirmDetailLbl, confirmDetail);
        lv_obj_align(confirmDetailLbl, LV_ALIGN_BOTTOM_LEFT, 8, -3);
    }

    for (int i = _catRangeStart; i < _catRangeEnd; i++) {
        const auto& item = _items[i];
        bool selected = (i == _selectedIdx);
        bool editable = isEditable(i);
        bool rebootPending = settingNeedsReboot(item);
        bool armed = armedAction(item);
        bool destructive = destructiveAction(item);
        uint32_t armedColor = destructive ? Theme::ERROR_CLR : Theme::WARNING_CLR;

        lv_obj_t* row = lv_obj_create(_scrollContainer);
        lv_obj_set_size(row, Theme::CONTENT_W, _freqEditing && selected ? 46 : 28);
        lv_obj_add_style(row, LvTheme::styleListBtn(), 0);
        if (editable) {
            lv_obj_add_style(row, LvTheme::styleListBtnFocused(), LV_STATE_FOCUSED);
        }
        lv_obj_set_style_bg_color(row, lv_color_hex(armed ? Theme::BG_SURFACE : (selected && editable ? Theme::BG_HOVER : Theme::BG)), 0);
        lv_obj_set_style_border_color(row, lv_color_hex(armed ? armedColor : (rebootPending ? Theme::WARNING_CLR : Theme::BORDER)), 0);
        lv_obj_set_style_border_width(row, rebootPending || armed ? 2 : 1, 0);
        lv_obj_set_style_border_side(row, rebootPending || armed ? (lv_border_side_t)(LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_BOTTOM) : LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        if (editable) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_user_data(row, (void*)(intptr_t)i);
            lv_obj_add_event_cb(row, [](lv_event_t* e) {
                auto* self = (LvSettingsScreen*)lv_event_get_user_data(e);
                int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
                if (self->_editing || self->_textEditing || self->_freqEditing) {
                    self->_editing = false;
                    self->_textEditing = false;
                    self->_freqEditing = false;
                    self->_numericTyping = false;
                }
                self->_selectedIdx = idx;
                KeyEvent tap = {};
                tap.enter = true;
                self->handleKey(tap);
            }, LV_EVENT_CLICKED, this);
            lv_group_add_obj(LvInput::group(), row);
            lv_obj_add_event_cb(row, [](lv_event_t* e) {
                lv_obj_scroll_to_view(lv_event_get_target(e), LV_ANIM_ON);
            }, LV_EVENT_FOCUSED, nullptr);
        }

        // Label
        lv_obj_t* nameLbl = lv_label_create(row);
        lv_obj_set_style_text_font(nameLbl, font, 0);
        uint32_t nameColor =
            armed ? armedColor :
            rebootPending ? Theme::WARNING_CLR :
            destructive ? Theme::ERROR_CLR :
            item.type == SettingType::ACTION ? Theme::TEXT_PRIMARY :
            !editable ? Theme::TEXT_MUTED : Theme::TEXT_SECONDARY;
        lv_obj_set_style_text_color(nameLbl, lv_color_hex(nameColor), 0);
        clipLabel(nameLbl, Theme::CONTENT_W - 136);
        lv_label_set_text(nameLbl, item.label);
        lv_obj_align(nameLbl, LV_ALIGN_LEFT_MID, 8, _freqEditing && selected ? -9 : 0);

        // Value
        String valStr;
        uint32_t valColor = editable ? Theme::PRIMARY : Theme::TEXT_MUTED;

        if (_freqEditing && selected) {
            valStr = String("< ") + freqFormatWithCursor() + " >";
            valColor = Theme::WARNING_CLR;
        } else if (_editing && selected) {
            valStr = formatEditedValue(item, _editValue);
            valColor = Theme::WARNING_CLR;
        } else if (_textEditing && selected) {
            valStr = _editText + "_";
            valColor = Theme::WARNING_CLR;
        } else {
            switch (item.type) {
                case SettingType::READONLY:
                    valStr = item.formatter ? item.formatter(0) : "";
                    valColor = Theme::TEXT_MUTED;
                    break;
                case SettingType::TEXT_INPUT: {
                    String v = item.textGetter ? item.textGetter() : "";
                    valStr = v.isEmpty() ? "(not set)" : (isWiFiPasswordLabel(item.label) ? maskedValue(v) : v);
                    valColor = v.isEmpty() ? Theme::TEXT_MUTED : Theme::PRIMARY;
                    break;
                }
                case SettingType::ENUM_CHOICE:
                    if (!item.enumLabels.empty()) {
                        int vi = item.getter ? constrain(item.getter(), 0, (int)item.enumLabels.size() - 1) : 0;
                        valStr = item.enumLabels[vi];
                    }
                    break;
                case SettingType::ACTION:
                    valStr = item.formatter ? item.formatter(0) : "";
                    valColor = destructive ? Theme::ERROR_CLR : Theme::TEXT_MUTED;
                    break;
                default: {
                    int v = item.getter ? item.getter() : 0;
                    valStr = item.formatter ? item.formatter(v) : String(v);
                    break;
                }
            }
        }

        if (armed) {
            valColor = armedColor;
        } else if (rebootPending) {
            valColor = Theme::WARNING_CLR;
        }

        if (!valStr.isEmpty()) {
            lv_obj_t* valLbl = lv_label_create(row);
            if (isWiFiPasswordLabel(item.label)) LvPrivacy::markSensitive(valLbl);
            lv_obj_set_style_text_font(valLbl, font, 0);
            lv_obj_set_style_text_color(valLbl, lv_color_hex(valColor), 0);
            lv_obj_set_style_text_align(valLbl, LV_TEXT_ALIGN_RIGHT, 0);
            clipLabel(valLbl, _freqEditing && selected ? 180 : 124);
            lv_label_set_text(valLbl, valStr.c_str());
            lv_obj_align(valLbl, LV_ALIGN_RIGHT_MID, -8, _freqEditing && selected ? -9 : 0);
            // Cache value label for the actively edited item (in-place updates)
            if (i == _selectedIdx && (_textEditing || _freqEditing || _editing)) {
                _editValueLbl = valLbl;
            }
        }

        if (_freqEditing && selected) {
            lv_obj_t* hint = lv_label_create(row);
            lv_obj_set_style_text_font(hint, &lv_font_rsdeck_10, 0);
            lv_obj_set_style_text_color(hint, lv_color_hex(Theme::TEXT_MUTED), 0);
#if HAS_SCROLLWHEEL
            lv_label_set_text(hint, "A/D: digit   Wheel: tune   Enter: save   Alt+Back: cancel");
#elif HAS_DPAD
            lv_label_set_text(hint, "Left/Right: digit  Up/Down: tune  OK: save");
#else
            lv_label_set_text(hint, "A/D digit  Ball tune  Enter save  Hold click cancels");
#endif
            lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 8, -2);
        }

        _rowObjs.push_back(row);
    }

    // Restore focus to the currently selected item after rebuild
    int focusOffset = _selectedIdx - _catRangeStart;
    if (focusOffset >= 0 && focusOffset < (int)_rowObjs.size()) {
        LvInput::focusObj(_rowObjs[focusOffset]);
    }
}

void LvSettingsScreen::selectWifiResult(int resultIdx) {
    if (!_cfg || resultIdx < 0 || resultIdx >= (int)_wifiResults.size()) return;

    try {
    auto& net = _wifiResults[resultIdx];
    auto& nets = _cfg->settings().wifiSTANetworks;
    while (nets.size() <= _wifiTargetSlot && nets.size() < WIFI_STA_MAX_NETWORKS) {
        nets.push_back({});
    }
    if (_wifiTargetSlot >= WIFI_STA_MAX_NETWORKS || _wifiTargetSlot >= nets.size()) {
        if (_ui) _ui->lvStatusBar().showToast("Network slots full", 1500);
        return;
    }

    for (size_t i = 0; i < nets.size(); i++) {
        if (i != _wifiTargetSlot && !nets[i].ssid.isEmpty() && nets[i].ssid == net.ssid) {
            if (_ui) _ui->lvStatusBar().showToast("Already saved", 1200);
            return;
        }
    }

    bool sameSSID = nets[_wifiTargetSlot].ssid == net.ssid;
    if (!UserConfig::trySetString(nets[_wifiTargetSlot].ssid, net.ssid.c_str(), net.ssid.length())) {
        if (_ui) _ui->lvStatusBar().showToast("Settings memory unavailable; retry", 2000);
        return;
    }
    if (!sameSSID) nets[_wifiTargetSlot].password = "";
    _cfg->settings().wifiSTASelected = (uint8_t)_wifiTargetSlot;
    applyAndSave();
    } catch (const std::bad_alloc&) {
        if (_ui) _ui->lvStatusBar().showToast("Settings memory unavailable; retry", 2000);
    }
}

void LvSettingsScreen::rebuildWifiList() {
    if (!_scrollContainer) return;
    _rowObjs.clear();
    lv_obj_clean(_scrollContainer);

    const lv_font_t* font = &lv_font_rsdeck_12;

    // Header
    lv_obj_t* headerRow = lv_obj_create(_scrollContainer);
    lv_obj_set_size(headerRow, Theme::CONTENT_W, 22);
    lv_obj_set_style_bg_opa(headerRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(headerRow, lv_color_hex(Theme::BORDER), 0);
    lv_obj_set_style_border_width(headerRow, 1, 0);
    lv_obj_set_style_border_side(headerRow, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_all(headerRow, 0, 0);
    lv_obj_set_style_radius(headerRow, 0, 0);
    lv_obj_clear_flag(headerRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* headerLbl = lv_label_create(headerRow);
    lv_obj_set_style_text_font(headerLbl, font, 0);
    lv_obj_set_style_text_color(headerLbl, lv_color_hex(Theme::ACCENT), 0);
    char header[48];
    snprintf(header, sizeof(header), "< Scan for profile %u", (unsigned)(_wifiTargetSlot + 1));
    lv_label_set_text(headerLbl, header);
    lv_obj_align(headerLbl, LV_ALIGN_LEFT_MID, 4, 0);

    // Make header tappable to go back
    lv_obj_add_flag(headerRow, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(headerRow, [](lv_event_t* e) {
        auto* self = (LvSettingsScreen*)lv_event_get_user_data(e);
        self->_wifiScanActive = false;
        self->_view = SettingsView::ITEM_LIST;
        self->rebuildItemList();
    }, LV_EVENT_CLICKED, this);

    if (_wifiResults.empty()) {
        lv_obj_t* emptyLbl = lv_label_create(_scrollContainer);
        lv_obj_set_style_text_font(emptyLbl, font, 0);
        lv_obj_set_style_text_color(emptyLbl, lv_color_hex(Theme::TEXT_MUTED), 0);
        lv_label_set_text(emptyLbl, _wifiScanActive ? "Scanning..." :
            _wifiScanOutcome == handheld::Outcome::Ok ? "No networks found" :
            _wifiScanOutcome == handheld::Outcome::Cancelled ? "Scan cancelled" : "Scan failed");
        return;
    }

    for (int i = 0; i < (int)_wifiResults.size(); i++) {
        auto& net = _wifiResults[i];

        lv_obj_t* row = lv_obj_create(_scrollContainer);
        lv_obj_set_size(row, Theme::CONTENT_W, 22);
        lv_obj_add_style(row, LvTheme::styleListBtn(), 0);
        lv_obj_add_style(row, LvTheme::styleListBtnFocused(), LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, (void*)(intptr_t)i);
        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            auto* self = (LvSettingsScreen*)lv_event_get_user_data(e);
            int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
            self->selectWifiResult(idx);
            self->_wifiScanActive = false;
            self->_view = SettingsView::ITEM_LIST;
            self->rebuildItemList();
        }, LV_EVENT_CLICKED, this);
        lv_group_add_obj(LvInput::group(), row);
        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            lv_obj_scroll_to_view(lv_event_get_target(e), LV_ANIM_ON);
        }, LV_EVENT_FOCUSED, nullptr);

        // Lock + SSID
        char buf[48];
        snprintf(buf, sizeof(buf), "%s %s", net.encrypted ? "*" : " ", net.ssid.c_str());
        lv_obj_t* lbl = lv_label_create(row);
        lv_obj_set_style_text_font(lbl, font, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(Theme::PRIMARY), 0);
        lv_label_set_text(lbl, buf);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);

        // Signal
        char sigBuf[12];
        snprintf(sigBuf, sizeof(sigBuf), "%ddBm", net.rssi);
        lv_obj_t* sigLbl = lv_label_create(row);
        lv_obj_set_style_text_font(sigLbl, &lv_font_rsdeck_10, 0);
        lv_obj_set_style_text_color(sigLbl, lv_color_hex(Theme::TEXT_MUTED), 0);
        lv_label_set_text(sigLbl, sigBuf);
        lv_obj_align(sigLbl, LV_ALIGN_RIGHT_MID, -4, 0);

        _rowObjs.push_back(row);
    }
}

void LvSettingsScreen::updateCategorySelection(int oldIdx, int newIdx) {
    if (oldIdx >= 0 && oldIdx < (int)_rowObjs.size()) {
        bool pending = categoryNeedsReboot(oldIdx);
        lv_obj_set_style_bg_color(_rowObjs[oldIdx], lv_color_hex(Theme::BG), 0);
        lv_obj_t* nameLbl = lv_obj_get_child(_rowObjs[oldIdx], 0);
        if (nameLbl) lv_obj_set_style_text_color(nameLbl, lv_color_hex(pending ? Theme::WARNING_CLR : Theme::TEXT_PRIMARY), 0);
        lv_obj_t* arrow = lv_obj_get_child(_rowObjs[oldIdx], -1);
        if (arrow) lv_obj_set_style_text_color(arrow, lv_color_hex(pending ? Theme::WARNING_CLR : Theme::TEXT_MUTED), 0);
    }
    if (newIdx >= 0 && newIdx < (int)_rowObjs.size()) {
        bool pending = categoryNeedsReboot(newIdx);
        lv_obj_set_style_bg_color(_rowObjs[newIdx], lv_color_hex(Theme::BG_HOVER), 0);
        lv_obj_t* nameLbl = lv_obj_get_child(_rowObjs[newIdx], 0);
        if (nameLbl) lv_obj_set_style_text_color(nameLbl, lv_color_hex(pending ? Theme::WARNING_CLR : Theme::TEXT_PRIMARY), 0);
        lv_obj_t* arrow = lv_obj_get_child(_rowObjs[newIdx], -1);
        if (arrow) lv_obj_set_style_text_color(arrow, lv_color_hex(pending ? Theme::WARNING_CLR : Theme::PRIMARY), 0);
        lv_obj_scroll_to_view(_rowObjs[newIdx], LV_ANIM_OFF);
    }
}

void LvSettingsScreen::updateItemSelection(int oldIdx, int newIdx) {
    // _rowObjs maps directly to items (header row is NOT in _rowObjs)
    int oldRow = oldIdx - _catRangeStart;
    int newRow = newIdx - _catRangeStart;
    auto setItemRowBg = [&](int row, bool selected) {
        if (row < 0 || row >= (int)_rowObjs.size()) return;
        int itemIdx = row + _catRangeStart;
        bool editable = isEditable(itemIdx);
        const auto& item = _items[itemIdx];
        bool armed = armedAction(item);
        lv_obj_set_style_bg_color(_rowObjs[row], lv_color_hex(
            armed ? Theme::BG_SURFACE : ((selected && editable) ? Theme::BG_HOVER : Theme::BG)), 0);
    };
    setItemRowBg(oldRow, false);
    setItemRowBg(newRow, true);
    if (newRow >= 0 && newRow < (int)_rowObjs.size()) {
        lv_obj_scroll_to_view(_rowObjs[newRow], LV_ANIM_OFF);
    }
}

void LvSettingsScreen::updateWifiSelection(int oldIdx, int newIdx) {
    if (oldIdx >= 0 && oldIdx < (int)_rowObjs.size()) {
        lv_obj_set_style_bg_color(_rowObjs[oldIdx], lv_color_hex(Theme::BG), 0);
        lv_obj_t* lbl = lv_obj_get_child(_rowObjs[oldIdx], 0);
        if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(Theme::TEXT_PRIMARY), 0);
    }
    if (newIdx >= 0 && newIdx < (int)_rowObjs.size()) {
        lv_obj_set_style_bg_color(_rowObjs[newIdx], lv_color_hex(Theme::BG_HOVER), 0);
        lv_obj_t* lbl = lv_obj_get_child(_rowObjs[newIdx], 0);
        if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(Theme::TEXT_PRIMARY), 0);
        lv_obj_scroll_to_view(_rowObjs[newIdx], LV_ANIM_OFF);
    }
}

void LvSettingsScreen::enterCategory(int catIdx) {
    if (catIdx < 0 || catIdx >= (int)_categories.size()) return;
    _categoryIdx = catIdx;
    auto& cat = _categories[catIdx];
    _catRangeStart = cat.startIdx;
    _catRangeEnd = cat.startIdx + cat.count;
    _selectedIdx = _catRangeStart;
    _editing = false;
    _textEditing = false;
    _freqEditing = false;
    _numericTyping = false;
    _editValueLbl = nullptr;
    if (!isEditable(_selectedIdx)) skipToNextEditable(1);
    _view = SettingsView::ITEM_LIST;
    rebuildItemList();
}

void LvSettingsScreen::exitToCategories() {
    _view = SettingsView::CATEGORY_LIST;
    _editing = false;
    _textEditing = false;
    _freqEditing = false;
    _numericTyping = false;
    _editValueLbl = nullptr;
    clearConfirmations();
    rebuildCategoryList();
}

bool LvSettingsScreen::handleKey(const KeyEvent& event) {
    if (_service && _service->settingsPending() && event.character != 0x1b && !event.del) return true;
    switch (_view) {
        case SettingsView::CATEGORY_LIST: {
            // LVGL focus group handles up/down navigation
            if (event.enter || event.character == '\n' || event.character == '\r') {
                // Get focused category from LVGL group
                lv_obj_t* focused = lv_group_get_focused(LvInput::group());
                if (focused) _categoryIdx = (int)(intptr_t)lv_obj_get_user_data(focused);
                enterCategory(_categoryIdx);
                return true;
            }
            return false;
        }

        case SettingsView::ITEM_LIST: {
            if (_items.empty()) return false;

            // Text edit mode - in-place label updates for responsiveness
            if (_textEditing) {
                auto& item = _items[_selectedIdx];
                auto updateEditLabel = [this]() {
                    if (_editValueLbl) {
                        String display = _editText + "_";
                        lv_label_set_text(_editValueLbl, display.c_str());
                        lv_obj_align(_editValueLbl, LV_ALIGN_RIGHT_MID, -8, 0);
                    }
                };
                if (event.enter || event.character == '\n' || event.character == '\r') {
                    bool assigned = false;
                    try { assigned = item.textSetter && item.textSetter(_editText); }
                    catch (const std::bad_alloc&) {}
                    if (!assigned) {
                        if (_ui) _ui->lvStatusBar().showToast("Settings memory unavailable; retry", 2000);
                        return true;
                    }
                    _textEditing = false;
                    _editValueLbl = nullptr;
                    applyAndSave();
                    rebuildItemList();
                    return true;
                }
                if (event.del || event.character == 8) {
                    if (_editText.length() > 0) { _editText.remove(_editText.length() - 1); updateEditLabel(); }
                    return true;
                }
                if (event.character == 0x1B) { _textEditing = false; _editValueLbl = nullptr; rebuildItemList(); return true; }
                if (event.character >= 0x20 && event.character <= 0x7E && (int)_editText.length() < item.maxTextLen) {
                    _editText += (char)event.character; updateEditLabel(); return true;
                }
                return true;
            }

            // Frequency digit-cursor edit mode - in-place label updates
            if (_freqEditing) {
                auto updateFreqLabel = [this]() {
                    if (_editValueLbl) {
                        String display = String("< ") + freqFormatWithCursor() + " >";
                        lv_label_set_text(_editValueLbl, display.c_str());
                        lv_obj_align(_editValueLbl, LV_ALIGN_RIGHT_MID, -8, -9);
                    }
                };
                if (event.left || event.character == ',' || event.character == 'a' || event.character == 'A') {
                    if (_freqCursor > 0) _freqCursor--;
                    updateFreqLabel(); return true;
                }
                if (event.right || event.character == '/' || event.character == 'd' || event.character == 'D') {
                    if (_freqCursor < 8) _freqCursor++;
                    updateFreqLabel(); return true;
                }
                // Wheel tunes the digit under the cursor VFO-style, carrying into
                // higher digits and clamped to the 9-digit displayable range.
                if (event.up || event.down) {
                    auto& item = _items[_selectedIdx];
                    long stepHz = 1;
                    for (int i = _freqCursor; i < 8; i++) stepHz *= 10;
                    long v = (long)freqRecompose() + (event.up ? stepHz : -stepHz);
                    long lo = item.minVal;
                    long hi = item.maxVal < 999999999 ? item.maxVal : 999999999;
                    if (v < lo) v = lo;
                    if (v > hi) v = hi;
                    _editValue = (int)v;
                    freqDecompose(_editValue);
                    updateFreqLabel(); return true;
                }
                if (event.character >= '0' && event.character <= '9') {
                    _freqDigits[_freqCursor] = event.character - '0';
                    _editValue = freqRecompose();
                    if (_freqCursor < 8) _freqCursor++;
                    updateFreqLabel(); return true;
                }
                if (event.enter || event.character == '\n' || event.character == '\r') {
                    auto& item = _items[_selectedIdx];
                    _editValue = freqRecompose();
                    if (!loRaFrequencyBand(_editValue)) {
                        if (_ui) _ui->lvStatusBar().showToast("Unsupported radio frequency", 2500);
                        return true;
                    }
                    try { if (item.setter) item.setter(_editValue); }
                    catch (const std::bad_alloc&) {
                        if (_ui) _ui->lvStatusBar().showToast("Settings memory unavailable; retry", 2000);
                        return true;
                    }
                    _freqEditing = false; _editing = false;
                    _editValueLbl = nullptr;
                    applyAndSave(); rebuildItemList(); return true;
                }
                if (event.del || event.character == 8) {
                    if (_freqCursor > 0) _freqCursor--;
                    updateFreqLabel(); return true;
                }
                if (event.character == 0x1B) {
                    _editValue = _freqOriginal;
                    _freqEditing = false; _editing = false;
                    _editValueLbl = nullptr;
                    rebuildItemList(); return true;
                }
                return true;
            }

            // Value edit mode
            if (_editing) {
                auto& item = _items[_selectedIdx];
                // Keep the focused row and viewport intact while turning the
                // wheel or typing. Rebuilding here restarts focus scrolling.
                auto updateValue = [&] {
                    if (_editValueLbl)
                        lv_label_set_text(_editValueLbl, formatEditedValue(item, _editValue).c_str());
                };
                // Direct digit entry for INTEGER fields
                if (item.type == SettingType::INTEGER && event.character >= '0' && event.character <= '9') {
                    int digit = event.character - '0';
                    if (!_numericTyping) {
                        _editValue = digit;
                        _numericTyping = true;
                    } else {
                        int newVal = _editValue * 10 + digit;
                        if (newVal <= item.maxVal) _editValue = newVal;
                    }
                    updateValue(); return true;
                }
                // Engaged values: up/right increases, down/left decreases.
                if (event.left || event.down) {
                    if (item.allowOff && _editValue <= item.minVal) _editValue = 0;
                    else {
                        _editValue -= item.step;
                        if (_editValue < item.minVal) _editValue = item.minVal;
                    }
                    _numericTyping = false;
                    updateValue(); return true;
                }
                if (event.right || event.up) {
                    if (item.allowOff && _editValue < item.minVal) _editValue = item.minVal;
                    else _editValue += item.step;
                    if (_editValue > item.maxVal) _editValue = item.maxVal;
                    _numericTyping = false;
                    updateValue(); return true;
                }
                if (event.enter || event.character == '\n' || event.character == '\r') {
                    if (_editValue < item.minVal && !(item.allowOff && _editValue == 0))
                        _editValue = item.minVal;
                    try { if (item.setter) item.setter(_editValue); }
                    catch (const std::bad_alloc&) {
                        if (_ui) _ui->lvStatusBar().showToast("Settings memory unavailable; retry", 2000);
                        return true;
                    }
                    _editing = false;
                    _numericTyping = false;
                    applyAndSave();
                    rebuildItemList(); return true;
                }
                if (event.del || event.character == 8) {
                    if (_numericTyping && _editValue > 0) {
                        _editValue /= 10;
                        updateValue();
                        return true;
                    } else if (event.repeat) {
                        return true;  // held repeat deletes digits but never exits edit
                    } else {
                        _editing = false;
                        _numericTyping = false;
                    }
                    rebuildItemList(); return true;
                }
                if (event.character == 0x1B) {
                    _editing = false; _numericTyping = false; rebuildItemList(); return true;
                }
                return true;
            }

            // Browse mode - LVGL focus group handles up/down
            if (hasPendingConfirmation() && (event.up || event.down || event.left || event.right || event.tab)) {
                clearConfirmations();
                rebuildItemList();
                if (_ui) _ui->lvStatusBar().showToast("Confirmation cancelled", 1000);
                return true;
            }
            if (event.del || event.character == 8 || event.character == 0x1B) {
                if (event.repeat) return true;  // back-nav only on a fresh tap
                if (hasPendingConfirmation()) {
                    clearConfirmations();
                    rebuildItemList();
                    if (_ui) _ui->lvStatusBar().showToast("Confirmation cancelled", 1000);
                    return true;
                }
                exitToCategories(); return true;
            }
            if (event.enter || event.character == '\n' || event.character == '\r') {
                // Sync _selectedIdx from LVGL focus
                lv_obj_t* focused = lv_group_get_focused(LvInput::group());
                if (focused) _selectedIdx = (int)(intptr_t)lv_obj_get_user_data(focused);
                if (!isEditable(_selectedIdx)) return true;
                auto& item = _items[_selectedIdx];
                if (item.type == SettingType::ACTION) {
                    if (!confirmableAction(item)) clearConfirmations();
                    if (item.action) item.action();
                    if (_view == SettingsView::ITEM_LIST) rebuildItemList();
                } else if (item.type == SettingType::TEXT_INPUT) {
                    clearConfirmations();
                    if (!item.textGetter) return true;
                    const auto& value = item.textGetter();
                    if (!UserConfig::trySetString(_editText, value.c_str(), value.length())) {
                        if (_ui) _ui->lvStatusBar().showToast("Settings memory unavailable; retry", 2000);
                        return true;
                    }
                    _textEditing = true;
                    rebuildItemList();
                } else if (item.type == SettingType::TOGGLE) {
                    clearConfirmations();
                    int val = item.getter ? item.getter() : 0;
                    try { if (item.setter) item.setter(val ? 0 : 1); }
                    catch (const std::bad_alloc&) {
                        if (_ui) _ui->lvStatusBar().showToast("Settings memory unavailable; retry", 2000);
                        return true;
                    }
                    applyAndSave();
                    rebuildItemList();
                } else if (strcmp(item.label, "Frequency") == 0 && item.type == SettingType::INTEGER) {
                    clearConfirmations();
                    // Radio-style digit cursor editor for frequency
                    _editing = true;
                    _editValue = item.getter ? item.getter() : 0;
                    _freqOriginal = _editValue;
                    freqDecompose(_editValue);
                    _freqCursor = 0;
                    _freqEditing = true;
                    rebuildItemList();
                } else {
                    clearConfirmations();
                    _editing = true;
                    _numericTyping = false;
                    _editValue = item.getter ? item.getter() : 0;
                    rebuildItemList();
                }
                return true;
            }
            return false;
        }

        case SettingsView::WIFI_PICKER: {
            // LVGL handles up/down navigation, click handler handles selection
            if (event.enter || event.character == '\n' || event.character == '\r') {
                if (_wifiScanActive) return true;
                lv_obj_t* focused = lv_group_get_focused(LvInput::group());
                if (focused) {
                    int idx = (int)(intptr_t)lv_obj_get_user_data(focused);
                    if (idx < (int)_wifiResults.size()) {
                        selectWifiResult(idx);
                    }
                }
                _wifiScanActive = false;
                _view = SettingsView::ITEM_LIST;
                rebuildItemList();
                return true;
            }
            if (event.del || event.character == 8 || event.character == 0x1B) {
                if (event.repeat) return true;  // back-nav only on a fresh tap
                _wifiScanActive = false;
                _view = SettingsView::ITEM_LIST;
                rebuildItemList();
                return true;
            }
            return false;
        }
    }
    return false;
}

bool LvSettingsScreen::cancelEditing() {
    if (!_editing && !_textEditing && !_freqEditing) return false;
    _editing = _textEditing = _freqEditing = _numericTyping = false;
    _editValueLbl = nullptr;
    rebuildItemList();
    return true;
}

bool LvSettingsScreen::handleLongPress() {
#if HAS_TRACKBALL
    // The stock Deck keyboard has no unambiguous Escape report. A hold while
    // editing cancels the draft before the destructive-action hold policy.
    if (cancelEditing()) return true;
#endif
    if (_view != SettingsView::ITEM_LIST || !hasPendingConfirmation()) return false;

    int focusedIdx = _selectedIdx;
    lv_obj_t* focused = lv_group_get_focused(LvInput::group());
    if (focused) {
        int candidate = (int)(intptr_t)lv_obj_get_user_data(focused);
        if (candidate >= 0 && candidate < (int)_items.size()) focusedIdx = candidate;
    }
    if (focusedIdx < 0 || focusedIdx >= (int)_items.size()) return false;

    const auto& item = _items[focusedIdx];
    if (_confirmingInitSD && labelEq(item.label, "Format SD Card")) {
        runFormatSD();
        return true;
    }
    if (_confirmingWipeSD && labelEq(item.label, "Erase " DEVICE_NAME " SD Data")) {
        runWipeSD();
        return true;
    }
    if (_confirmingReset && labelEq(item.label, "Erase Device")) {
        runFactoryReset();
        return true;
    }
    if (_confirmingDevMode && labelEq(item.label, "Developer Radio Controls")) {
        runEnableDevMode();
        return true;
    }

    if (_ui) _ui->lvStatusBar().showToast("Select armed action, then hold", 2000);
    return true;
}

void LvSettingsScreen::snapshotRebootSettings() {
    if (!_cfg) return;
    auto& s = _cfg->settings();
    _rebootSnap.wifiMode = s.wifiMode;
    _rebootSnap.wifiSTANetworks = s.wifiSTANetworks;
    _rebootSnap.wifiSTASelected = s.wifiSTASelected;
    _rebootSnap.autoIfaceEnabled = s.autoIfaceEnabled;
    _rebootSnap.sdStorageEnabled = s.sdStorageEnabled;
    _rebootSnap.loraEnabled = s.loraEnabled;
}

bool LvSettingsScreen::rebootSettingsChanged() const {
    return loraSettingsChanged() || interfaceSettingsChanged()
        || storageSettingsChanged() || tcpSettingsChanged();
}

bool LvSettingsScreen::loraSettingsChanged() const {
    if (!_cfg) return false;
    return _cfg->settings().loraEnabled != _rebootSnap.loraEnabled;
}

bool LvSettingsScreen::interfaceSettingsChanged() const {
    if (!_cfg) return false;
    const auto& s = _cfg->settings();
    if (s.wifiMode != _rebootSnap.wifiMode) return true;
    if (s.wifiSTASelected != _rebootSnap.wifiSTASelected) return true;
    if (s.autoIfaceEnabled != _rebootSnap.autoIfaceEnabled) return true;
    if (s.wifiSTANetworks.size() != _rebootSnap.wifiSTANetworks.size()) return true;
    for (size_t i = 0; i < s.wifiSTANetworks.size(); i++) {
        if (s.wifiSTANetworks[i].ssid != _rebootSnap.wifiSTANetworks[i].ssid) return true;
        if (s.wifiSTANetworks[i].password != _rebootSnap.wifiSTANetworks[i].password) return true;
    }
    return false;
}

bool LvSettingsScreen::storageSettingsChanged() const {
    if (!_cfg) return false;
    return _cfg->settings().sdStorageEnabled != _rebootSnap.sdStorageEnabled;
}

void LvSettingsScreen::snapshotTCPSettings() {
    if (!_cfg) return;
    _tcpSnap = _cfg->settings().tcpConnections;
}

bool LvSettingsScreen::tcpSettingsChanged() const {
    if (!_cfg) return false;
    const auto& endpoints = _cfg->settings().tcpConnections;
    if (endpoints.size() != _tcpSnap.size()) return true;
    for (size_t i = 0; i < endpoints.size(); ++i) {
        if (endpoints[i].host != _tcpSnap[i].host || endpoints[i].port != _tcpSnap[i].port ||
            endpoints[i].autoConnect != _tcpSnap[i].autoConnect) return true;
    }
    return false;
}

// --- Frequency digit-cursor editor helpers ---

void LvSettingsScreen::freqDecompose(int value) {
    // Decompose Hz value into 9 individual digits (left-padded with zeros)
    for (int i = 8; i >= 0; i--) {
        _freqDigits[i] = value % 10;
        value /= 10;
    }
}

int LvSettingsScreen::freqRecompose() const {
    int val = 0;
    for (int i = 0; i < 9; i++) val = val * 10 + _freqDigits[i];
    return val;
}

String LvSettingsScreen::freqFormatWithCursor() const {
    // Format as "NNN.NNN.NNN" with brackets around cursor digit
    char buf[24];
    char digits[9];
    for (int i = 0; i < 9; i++) digits[i] = '0' + _freqDigits[i];

    // Build string with cursor brackets: e.g., "920.[6]50.500"
    int pos = 0;
    for (int i = 0; i < 9; i++) {
        if (i == 3 || i == 6) buf[pos++] = '.';
        if (i == _freqCursor) {
            buf[pos++] = '[';
            buf[pos++] = digits[i];
            buf[pos++] = ']';
        } else {
            buf[pos++] = digits[i];
        }
    }
    buf[pos] = '\0';
    return String(buf);
}

void LvSettingsScreen::applyAndSave() {
    if (!_service || !_cfg) return;
    const bool wasRebootNeeded = _rebootNeeded;
    const bool tcpChanged = tcpSettingsChanged();
    _service->applySettings([this, wasRebootNeeded, tcpChanged](const handheld::Result& result) {
        _rebootNeeded = rebootSettingsChanged();
        if (_screen && _view == SettingsView::ITEM_LIST) rebuildItemList();
        if (!_ui || result.outcome != handheld::Outcome::Ok) return;
        _ui->lvStatusBar().showToast(
            _rebootNeeded ? (tcpChanged ? "TCP server saved; reboot to apply" : "Interface changes saved; reboot to apply") :
            wasRebootNeeded ? "Pending reboot cleared" :
            result.detail[0] ? result.detail : "Saved", 2000);
    });
}
