#include "SettingsScreen.h"
#include "Theme.h"
#include "config/Config.h"
#include "config/SettingsInput.h"
#include "config/AnnounceInterval.h"
#include "radio/RadioFrequency.h"
#include "radio/RadioPresets.h"
#include "radio/RadioSettings.h"
#include "util/DisplayText.h"
#include "storage/MessageStore.h"
#include "ui/RadioSettingsText.h"
#include "ui/StorageHistoryText.h"
#include "ui/VoiceSettingsText.h"
#include <algorithm>
#include <new>
#include <WiFi.h>

namespace {
// Three bounded rows keep the full current settings details inside the content
// area. Scan/draw twice instead of retaining another message or line cache.
template <typename Visitor>
void visitSettingsNotice(const char* text, Visitor visit) {
    constexpr size_t columns = (Theme::CONTENT_W - 20) / Theme::CHAR_W;
    const char* at = text;
    for (size_t row = 0; row < 3; ++row) {
        char line[columns + 1];
        size_t used = 0, wordBreak = 0;
        const char* nextWord = nullptr;
        while (*at && *at != '\n') {
            size_t available = 0;
            while (available < 4 && at[available]) ++available;
            bool escape = false;
            const size_t scalar = handheld::display::codepoint(
                reinterpret_cast<const uint8_t*>(at), available, true, escape);
            const size_t width = escape || *at == '\t' ? 1 : scalar;
            if (used + width > columns) break;
            if (*at == ' ' || *at == '\t') {
                wordBreak = used;
                nextWord = at + scalar;
            }
            if (escape) line[used++] = '?';
            else if (*at == '\t') line[used++] = ' ';
            else { memcpy(line + used, at, scalar); used += scalar; }
            at += scalar;
        }
        if (*at && *at != '\n' && nextWord) { used = wordBreak; at = nextWord; }
        while (used && line[used - 1] == ' ') --used;
        if (*at == '\n') ++at;
        while (*at == ' ' || *at == '\t') ++at;
        if (row == 2 && *at) {
            while (used > columns - 3) {
                --used;
                while (used && handheld::display::continuation(uint8_t(line[used]))) --used;
            }
            memcpy(line + used, "...", 3); used += 3;
        }
        line[used] = 0;
        visit(row, line, used);
        if (!*at) break;
    }
}

void drawSettingsNotice(M5Canvas& canvas, const char* text) {
    size_t rows = 0, widest = 0;
    visitSettingsNotice(text, [&](size_t row, const char*, size_t length) {
        rows = row + 1; widest = std::max(widest, length);
    });
    const int tw = widest * Theme::CHAR_W + 12;
    const int th = rows * Theme::CHAR_H + 8;
    const int tx = (Theme::CONTENT_W - tw) / 2;
    const int ty = Theme::CONTENT_Y + Theme::CONTENT_H - th - 4;
    canvas.fillRoundRect(tx, ty, tw, th, 3, Theme::SELECTION_BG);
    canvas.drawRoundRect(tx, ty, tw, th, 3, Theme::PRIMARY);
    canvas.setTextColor(Theme::PRIMARY);
    visitSettingsNotice(text, [&](size_t row, const char* line, size_t) {
        canvas.setCursor(tx + 6, ty + 4 + row * Theme::CHAR_H);
        canvas.print(line);
    });
}
} // namespace

// Lite UI edits a single STA network: the selected slot of the core
// multi-network model (::WiFiNetwork from UserConfig.h — distinct from the
// scan-result struct nested in SettingsScreen).
static ::WiFiNetwork& staNetwork(UserSettings& s) {
    if (s.wifiSTANetworks.empty()) s.wifiSTANetworks.push_back(::WiFiNetwork{});
    if (s.wifiSTASelected >= s.wifiSTANetworks.size()) s.wifiSTASelected = 0;
    return s.wifiSTANetworks[s.wifiSTASelected];
}

static const ::WiFiNetwork& staNetworkRO(const UserSettings& s) {
    static const ::WiFiNetwork kEmpty;
    if (s.wifiSTANetworks.empty()) return kEmpty;
    size_t idx = s.wifiSTASelected < s.wifiSTANetworks.size() ? s.wifiSTASelected : 0;
    return s.wifiSTANetworks[idx];
}

SettingsScreen::WiFiAction SettingsScreen::currentWiFiAction() const {
    if (!_config || settingsView().wifiMode != RAT_WIFI_STA) return WiFiAction::None;
    if (WiFi.status() == WL_CONNECTED) return WiFiAction::Disconnect;
    if (_network.connecting && _network.connecting()) return WiFiAction::Cancel;
    return staNetworkRO(settingsView()).ssid.isEmpty() ? WiFiAction::None : WiFiAction::Connect;
}

const UserSettings& SettingsScreen::settingsView() const {
    return _editing && _candidate ? _candidate->settings() : _config->settings();
}

bool SettingsScreen::ensureCandidate() {
    if (!_active || !_config) return false;
    if (_config->settingsPending() || _radioApplyPending) {
        showToast("Settings apply pending"); return false;
    }
    if (_candidate) return true;
    try {
        _candidate.emplace();
        if (_candidate->tryAssign(*_config)) return true;
    } catch (const std::bad_alloc&) {}
    releaseCandidate();
    showToast("Settings memory unavailable; retry", 2500);
    return false;
}

void SettingsScreen::releaseCandidate() { _candidate.reset(); }

void SettingsScreen::releaseEditor() {
    _editing = false;
    _editField = -1;
    _editInput.release();
    std::string().swap(_tcpPendingHost);
    std::string().swap(_editLabel);
    if (!_scanVisible) std::vector<WiFiNetwork>().swap(_scanResults);
}

void SettingsScreen::onEnter() {
    releaseCandidate();
    releaseEditor();
    std::vector<WiFiNetwork>().swap(_scanResults);
    _subMenu = MENU_MAIN;
    _active = true;
    _scanVisible = false;
    _confirmPending = false;
    for (auto& row : _focus) row = {};
    _propChoicePending = false; _propCount = 0;
    try { buildMainMenu(); }
    catch (const std::bad_alloc&) { clearRows(); showToast("Settings memory unavailable; retry", 2500); }
}

void SettingsScreen::onExit() {
    _active = false;
    _scanVisible = false; // The owner still retires a scan already in progress.
    _confirmPending = _propChoicePending = false;
    releaseCandidate();
    releaseEditor();
    _list.release(); _rowCount = 0;
    std::vector<WiFiNetwork>().swap(_scanResults);
    _propCount = 0;
}

void SettingsScreen::clearRows() { _list.clear(); _rowCount = 0; }

void SettingsScreen::addRow(const std::string& label, RowAction action, uint8_t value, uint16_t color) {
    if (_rowCount >= RowCapacity) return;
    _list.addItem(label, color);
    _rows[_rowCount++] = {action, value};
}

SettingsScreen::Row SettingsScreen::selectedRow() const {
    const int selected = _list.getSelectedIndex();
    return selected >= 0 && size_t(selected) < _rowCount ? _rows[selected] : Row{};
}

void SettingsScreen::restoreRow(Row row) {
    for (size_t i = 0; i < _rowCount; ++i) {
        if (_rows[i].action == row.action && _rows[i].value == row.value) {
            _list.setSelected(int(i)); return;
        }
    }
}

void SettingsScreen::rebuildMenu() {
    switch (_subMenu) {
        case MENU_MAIN: buildMainMenu(); break;
        case MENU_RADIO: buildRadioMenu(); break;
        case MENU_RADIO_PRESETS: buildRadioPresetsMenu(); break;
        case MENU_WIFI: buildWiFiMenu(); break;
        case MENU_WIFI_MODE: buildWiFiModeMenu(); break;
        case MENU_TCP: buildTCPMenu(); break;
        case MENU_SDCARD: buildSDCardMenu(); break;
        case MENU_DISPLAY: buildDisplayMenu(); break;
        case MENU_IDENTITY: buildIdentityMenu(); break;
        case MENU_AUDIO: buildAudioMenu(); break;
        case MENU_ABOUT: buildDeviceMenu(); break;
        case MENU_VOICE: buildVoiceMenu(); break;
        case MENU_PROPAGATION: buildPropagationMenu(); break;
        case MENU_PROPAGATION_CHOICE: showPropagationChoice(); break;
        case MENU_PROPAGATION_NODES: showPropagationNodes(); break;
        case MENU_WIFI_SCAN: buildScanResultsMenu(); break;
        case MENU_COUNT: break;
    }
}

void SettingsScreen::enterMenu(SubMenu menu) {
    _focus[_subMenu] = selectedRow();
    _subMenu = menu;
    rebuildMenu();
}

void SettingsScreen::backMenu() {
    const SubMenu child = _subMenu;
    _propChoicePending = false;
    if (child == MENU_WIFI_SCAN) {
        _scanVisible = false;
        std::vector<WiFiNetwork>().swap(_scanResults);
    }
    _subMenu = child == MENU_RADIO_PRESETS ? MENU_RADIO :
        child == MENU_TCP || child == MENU_WIFI_SCAN || child == MENU_WIFI_MODE ? MENU_WIFI :
        child == MENU_PROPAGATION_NODES ? MENU_PROPAGATION_CHOICE :
        child == MENU_PROPAGATION_CHOICE ? MENU_PROPAGATION : MENU_MAIN;
    rebuildMenu();
    restoreRow(_focus[_subMenu]);
}

void SettingsScreen::buildMainMenu() {
    clearRows();
    addRow("LoRa", RowAction::OpenMenu, MENU_RADIO);
    addRow("Wi-Fi & TCP", RowAction::OpenMenu, MENU_WIFI);
    addRow("Propagation", RowAction::OpenMenu, MENU_PROPAGATION);
    addRow("Voice Messages", RowAction::OpenMenu, MENU_VOICE);
    addRow("Identity", RowAction::OpenMenu, MENU_IDENTITY);
    addRow("Display", RowAction::OpenMenu, MENU_DISPLAY);
    addRow("Alerts", RowAction::OpenMenu, MENU_AUDIO);
    addRow("Storage", RowAction::OpenMenu, MENU_SDCARD);
    addRow("Device", RowAction::OpenMenu, MENU_ABOUT);
}

void SettingsScreen::buildRadioMenu() {
    clearRows();
    if (!_config) return;
    const auto& s = settingsView();
    char buf[40];

    // Active preset indicator
    snprintf(buf, sizeof(buf), "%s: %s", (_radioApplyPending || _config->settingsPending()) ? "Pending" : _candidate ? "Unsaved" : "Saved", RadioPresets::name(s));
    addRow(buf);
    addRow("Link Presets", RowAction::OpenMenu, MENU_RADIO_PRESETS);

    snprintf(buf, sizeof(buf), "Frequency: %lu Hz", (unsigned long)s.loraFrequency);
    addRow(buf, RowAction::Edit, 0);
    snprintf(buf, sizeof(buf), "TX Power: %d dBm", s.loraTxPower);
    addRow(buf, RowAction::Edit, 4);
    snprintf(buf, sizeof(buf), "SF: %d", s.loraSF);
    addRow(buf, RowAction::Edit, 1);
    snprintf(buf, sizeof(buf), "BW: %lu Hz", (unsigned long)s.loraBW);
    addRow(buf, RowAction::Edit, 2);
    snprintf(buf, sizeof(buf), "CR: %d", s.loraCR);
    addRow(buf, RowAction::Edit, 3);
    snprintf(buf, sizeof(buf), "Preamble: %ld symbols", s.loraPreamble);
    addRow(buf, RowAction::Edit, 5);
    addRow(handheld::ui::radio_settings::LocalRulesShort, RowAction::Info, 0, Theme::MUTED);

    addRow("< Back", RowAction::Back);
}

void SettingsScreen::buildRadioPresetsMenu() {
    clearRows();
    if (!_config) return;
    const int activeIdx = RadioPresets::detect(settingsView());
    for (int i = 0; i < RadioPresets::count; ++i) {
        char label[40];
        snprintf(label, sizeof(label), "%s[%s]",
                 i == activeIdx ? ">" : " ", RadioPresets::values[i].name);
        addRow(label, RowAction::Preset, uint8_t(i), i == activeIdx ? Theme::PRIMARY : 0);
    }
    addRow("< Back", RowAction::Back);
}

void SettingsScreen::buildWiFiMenu() {
    clearRows();
    _wifiAction = currentWiFiAction();
    _drawnWiFiAction = WiFiAction::None;
    if (!_config) return;
    const auto& s = settingsView();

    const char* modeNames[] = {"OFF", "AP", "STA"};
    char modeBuf[24];
    snprintf(modeBuf, sizeof(modeBuf), "Mode: %s", modeNames[s.wifiMode]);
    addRow(modeBuf, RowAction::OpenMenu, MENU_WIFI_MODE);

    if (s.wifiMode == RAT_WIFI_AP) {
        addRow(("AP SSID: " + std::string(s.wifiAPSSID.isEmpty() ? "(auto)" : s.wifiAPSSID.c_str())), RowAction::Edit, 0);
        addRow(s.wifiAPPassword.isEmpty() ? "AP Pass: (open)" : "AP Pass: ********", RowAction::Edit, 1);
    } else if (s.wifiMode == RAT_WIFI_STA) {
        if (_wifiAction == WiFiAction::Disconnect) {
            char statusBuf[48];
            snprintf(statusBuf, sizeof(statusBuf), "Connected: %s", WiFi.SSID().c_str());
            addRow(statusBuf);
            addRow("[Disconnect]", RowAction::WiFiConnection);
        } else if (_wifiAction == WiFiAction::Cancel) {
            addRow("Connecting...");
            addRow("[Cancel]", RowAction::WiFiConnection);
        } else if (_wifiAction == WiFiAction::Connect) {
            char statusBuf[48];
            snprintf(statusBuf, sizeof(statusBuf), "Saved: %s (offline)", staNetworkRO(s).ssid.c_str());
            addRow(statusBuf);
            addRow("[Connect]", RowAction::WiFiConnection);
        } else {
            addRow("No network configured");
            addRow("");
        }
        addRow("Scan Networks", RowAction::WiFiScan);
        char autoBuf[40];
        snprintf(autoBuf, sizeof(autoBuf), "Auto-discover LAN: %s",
                 s.autoIfaceEnabled ? "ON" : "OFF");
        addRow(autoBuf, RowAction::AutoDiscover);
        addRow("TCP Connections", RowAction::OpenMenu, MENU_TCP);
    }
    // OFF mode: only mode selector + back

    addRow("< Back", RowAction::Back);
}

void SettingsScreen::buildTCPMenu() {
    clearRows();
    if (!_config) return;
    const auto& s = settingsView();

    addRow("+ Add Connection", RowAction::TCPAdd);

    for (size_t i = 0; i < s.tcpConnections.size(); i++) {
        auto& ep = s.tcpConnections[i];
        char buf[64];
        snprintf(buf, sizeof(buf), "%s:%d [%s]",
                 ep.host.c_str(), ep.port,
                 ep.autoConnect ? "ON" : "OFF");
        addRow(buf, RowAction::TCPToggle, uint8_t(i));
    }

    addRow("< Back", RowAction::Back);
}

void SettingsScreen::buildWiFiModeMenu() {
    clearRows();
    if (!_config) return;
    const RatWiFiMode active = settingsView().wifiMode;
    const char* names[] = {"OFF", "AP hotspot", "STA client"};
    for (uint8_t mode = RAT_WIFI_OFF; mode <= RAT_WIFI_STA; ++mode) {
        char label[24];
        snprintf(label, sizeof(label), "%s[%s]", mode == active ? ">" : " ", names[mode]);
        addRow(label, RowAction::WiFiMode, mode, mode == active ? Theme::PRIMARY : 0);
    }
    addRow("< Back", RowAction::Back);
    _list.setSelected(active);
}

bool SettingsScreen::addTCPConnection(const std::string& host, uint16_t port) {
    struct Release { std::optional<UserConfig>& value; ~Release() { value.reset(); } } release{_candidate};
    if (!ensureCandidate()) return false;
    auto& s = _candidate->settings();
    if (s.tcpConnections.size() >= MAX_TCP_CONNECTIONS) {
        showToast("Max 4 connections");
        return false;
    }

    TCPEndpoint ep;
    if (!UserConfig::trySetString(ep.host, host.data(), host.size())) {
        showToast("Settings memory unavailable; retry", 2500); return false;
    }
    ep.port = port;
    if (ep.host.isEmpty()) return false;

    // Reachability is transient. The transport owner attempts saved endpoints;
    // saving one must not block this UI or depend on the peer being online now.
    try { s.tcpConnections.push_back(std::move(ep)); }
    catch (const std::bad_alloc&) { showToast("Settings memory unavailable; retry", 2500); return false; }
    if (!applyAndSave()) return false;
    showToast("Connection added");
    buildTCPMenu();
    return true;
}

void SettingsScreen::toggleTCPConnection(int index) {
    if (!ensureCandidate()) return;
    auto& s = _candidate->settings();
    if (index < 0 || index >= (int)s.tcpConnections.size()) { releaseCandidate(); return; }

    s.tcpConnections[index].autoConnect = !s.tcpConnections[index].autoConnect;
    applyAndSave();
    buildTCPMenu();
}

void SettingsScreen::removeTCPConnection(int index) {
    if (!ensureCandidate()) return;
    auto& s = _candidate->settings();
    if (index < 0 || index >= (int)s.tcpConnections.size()) { releaseCandidate(); return; }

    s.tcpConnections.erase(s.tcpConnections.begin() + index);
    if (applyAndSave()) showToast("Removed");
    buildTCPMenu();
}

// =============================================================================
// Storage submenu
// =============================================================================

void SettingsScreen::buildSDCardMenu() {
    clearRows();
    char buf[32];
    const size_t flashTotal = _flash && _flash->isReady() ? _flash->totalBytes() : 0;
    if (flashTotal) snprintf(buf, sizeof(buf), "Flash: %lu/%lu KB",
                             (unsigned long)(_flash->usedBytes() / 1024), (unsigned long)(flashTotal / 1024));
    else snprintf(buf, sizeof(buf), "Flash: Error");
    addRow(buf);
    snprintf(buf, sizeof(buf), "%s: %s", handheld::ui::storage_history::Label,
             _messages ? handheld::ui::storage_history::mode(_messages->historyMode()) : "Unknown");
    addRow(buf);

    if (_sdStore && _sdStore->isReady()) {
        addRow("SD Card: Ready");

        uint64_t total = _sdStore->totalBytes();
        uint64_t used = _sdStore->usedBytes();
        uint64_t free = total > used ? total - used : 0;

        snprintf(buf, sizeof(buf), "Total: %llu MB", total / (1024 * 1024));
        addRow(buf);
        snprintf(buf, sizeof(buf), "Used: %llu MB", used / (1024 * 1024));
        addRow(buf);
        snprintf(buf, sizeof(buf), "Free: %llu MB", free / (1024 * 1024));
        addRow(buf);

        addRow("Initialize & Restart", RowAction::SDInitialize);
        addRow("Wipe Data & Restart", RowAction::SDWipe, 0, Theme::ERROR);
    } else {
        addRow("SD Card: Not Found");
    }

    addRow("< Back", RowAction::Back);
}

void SettingsScreen::sdCardFormat() {
    if (!_sdStore || !_sdStore->isReady()) {
        showToast("No SD card");
        return;
    }

    requestMaintenance(handheld::Operation::FormatSD);
}

// =============================================================================
// WiFi Scanner
// =============================================================================

void SettingsScreen::startWiFiScan() {
    if (_subMenu != MENU_WIFI_SCAN) _focus[_subMenu] = selectedRow();
    _subMenu = MENU_WIFI_SCAN;
    _scanVisible = true;
    if (!_scanPending) {
        _scanResults.clear();
        _scanPending = _network.startScan && _network.finishScan && _network.startScan();
        _scanOutcome = _scanPending ? handheld::ScanResult::Pending : handheld::ScanResult::Failed;
    }
    buildScanResultsMenu();
}

bool SettingsScreen::pollNetworkResults() {
    const bool changed = pollWiFiStatus();
    if (!_scanPending || !_network.finishScan) return changed;
    String json;
    const auto result = _network.finishScan(json);
    if (result == handheld::ScanResult::Pending) return changed;
    _scanPending = false; _scanOutcome = result; _scanResults.clear();
    // The owner must retire the scan, but a hidden screen has no consumer for
    // its JSON or rows. Never recreate hidden settings allocations here.
    if (!_active || !_scanVisible || _subMenu != MENU_WIFI_SCAN) {
        std::vector<WiFiNetwork>().swap(_scanResults);
        return changed;
    }
    if (result == handheld::ScanResult::Ready) {
        JsonDocument doc;
        const auto error = deserializeJson(doc, json);
        if (error || !doc.is<JsonArray>() || doc.size() > 15) {
            _scanOutcome = error == DeserializationError::NoMemory ?
                handheld::ScanResult::NoMemory : handheld::ScanResult::Failed;
        } else {
            for (const auto row : doc.as<JsonArray>()) {
                if (!row["ssid"].is<const char*>() || !row["rssi"].is<int>() || !row["encrypted"].is<bool>()) {
                    _scanResults.clear(); _scanOutcome = handheld::ScanResult::Failed; break;
                }
                WiFiNetwork network;
                const char* ssid = row["ssid"].as<const char*>();
                if (!UserConfig::trySetString(network.ssid, ssid, strlen(ssid))) {
                    _scanResults.clear(); _scanOutcome = handheld::ScanResult::NoMemory; break;
                }
                network.rssi = row["rssi"].as<int>();
                network.encType = row["encrypted"].as<bool>() ? 1 : WIFI_AUTH_OPEN;
                try { _scanResults.push_back(std::move(network)); }
                catch (const std::bad_alloc&) {
                    _scanResults.clear(); _scanOutcome = handheld::ScanResult::NoMemory; break;
                }
            }
        }
    }
    if (result == handheld::ScanResult::Ready && _scanOutcome != handheld::ScanResult::Ready)
        Serial.printf("[WIFI] Scan results unavailable (bytes=%u heap=%lu largest=%lu)\n",
                      (unsigned)json.length(), (unsigned long)ESP.getFreeHeap(),
                      (unsigned long)ESP.getMaxAllocHeap());
    if (_active && _scanVisible && _subMenu == MENU_WIFI_SCAN)
        buildScanResultsMenu(_list.getSelectedIndex() == _list.itemCount() - 1);
    return true;
}

void SettingsScreen::buildScanResultsMenu(bool preserveBack) {
    clearRows();

    if (_scanPending) {
        addRow("Scanning...");
    } else if (_scanOutcome == handheld::ScanResult::Failed) {
        addRow("Scan failed; retry");
    } else if (_scanOutcome == handheld::ScanResult::NoMemory) {
        addRow("Not enough memory; retry");
    } else if (_scanOutcome == handheld::ScanResult::Cancelled) {
        addRow("Scan cancelled");
    } else if (_scanResults.empty()) {
        addRow("No networks found");
    } else {
        for (size_t i = 0; i < _scanResults.size(); ++i) {
            const auto& net = _scanResults[i];
            char buf[48];
            const char* lock = (net.encType == WIFI_AUTH_OPEN) ? "" : "*";
            snprintf(buf, sizeof(buf), "%s%s (%d dBm)",
                     lock, net.ssid.c_str(), net.rssi);
            addRow(buf, RowAction::WiFiNetwork, uint8_t(i));
        }
    }

    addRow("Rescan", RowAction::WiFiScan);
    addRow("< Back", RowAction::Back);
    // Status rows are informational. Leave retry usable after a failure and
    // retain Back if the user deliberately moved there while scanning.
    _list.setSelected(preserveBack ? _list.itemCount() - 1 :
        _scanResults.empty() ? _list.itemCount() - 2 : 0);
}

void SettingsScreen::disconnectWiFi() {
    if (!_network.disconnect) { showToast("Network unavailable"); return; }
    _network.disconnect();
    showToast(_wifiAction == WiFiAction::Cancel ? "Connection cancelled" : "Disconnected");
    buildWiFiMenu();
}

bool SettingsScreen::pollWiFiStatus() {
    if (!_active || _subMenu != MENU_WIFI || _editing || _wifiAction == currentWiFiAction()) return false;
    const int selected = _list.getSelectedIndex();
    buildWiFiMenu();
    _list.setSelected(selected);
    return true;
}

void SettingsScreen::activateWiFiAction() {
    // A connection can finish between the last refresh and Enter. Update a
    // stale label without executing the opposite action on that same press.
    if (pollWiFiStatus() || _wifiAction != _drawnWiFiAction) return;
    switch (_wifiAction) {
        case WiFiAction::Connect: connectWiFi(); break;
        case WiFiAction::Cancel: case WiFiAction::Disconnect: disconnectWiFi(); break;
        case WiFiAction::None: return;
    }
    restoreRow({RowAction::WiFiConnection, 0});
}

void SettingsScreen::connectWiFi() {
    if (!_config) return;
    auto& s = _config->settings();
    if (staNetworkRO(s).ssid.isEmpty()) {
        showToast("No SSID set");
        return;
    }
    showToast(_network.connect && _network.connect() ? "Connecting..." : "Connection unavailable");
    buildWiFiMenu();
}

void SettingsScreen::selectNetwork(int index) {
    if (index < 0 || index >= (int)_scanResults.size()) return;
    if (!_config) return;

    // Opening a result is only an editor draft. Do not alter the saved profile
    // or carry another network's secret into an open/new network's editor.
    const auto& selected = _scanResults[index];
    const auto& saved = staNetworkRO(settingsView());
    const bool keepPassword = selected.encType != WIFI_AUTH_OPEN && saved.ssid == selected.ssid;
    try {
        const std::string password = keepPassword ? saved.password.c_str() : "";
        _scanVisible = false;
        _subMenu = MENU_WIFI;
        buildWiFiMenu();
        restoreRow({RowAction::WiFiScan, 0});
        startEditing(1, password);
    } catch (const std::bad_alloc&) {
        _editing = false; _editInput.setActive(false); _editInput.setSubmitCallback(nullptr);
        showToast("Settings memory unavailable; retry", 2500); return;
    }
    // Keep the selected network bound to retries through the stable result row;
    // each submission stages it against the latest committed configuration.
    _editInput.setSubmitCallback([this, index](const std::string& value) {
        if (!_active || !_editing || _subMenu != MENU_WIFI || _editField != 1 ||
            !_config || settingsView().wifiMode != RAT_WIFI_STA ||
            index < 0 || size_t(index) >= _scanResults.size()) {
            showToast("Network selection expired");
            return;
        }
        if (!ensureCandidate()) return;
        const auto& selectedSSID = _scanResults[index].ssid;
        try {
            if (!UserConfig::trySetString(staNetwork(_candidate->settings()).ssid,
                    selectedSSID.c_str(), selectedSSID.length())) {
                releaseCandidate(); showToast("Settings memory unavailable; retry", 2500); return;
            }
        } catch (const std::bad_alloc&) { releaseCandidate(); showToast("Settings memory unavailable; retry", 2500); return; }
        commitEdit(value);
    });
}

// =============================================================================
// Display / Audio menus
// =============================================================================

void SettingsScreen::buildDisplayMenu() {
    clearRows();
    if (!_config) return;
    const auto& s = settingsView();
    char buf[40];

    snprintf(buf, sizeof(buf), "Brightness: %d%%", s.brightness);
    addRow(buf, RowAction::Edit, 0);
    snprintf(buf, sizeof(buf), "Dim timeout: %ds", s.screenDimTimeout);
    addRow(buf, RowAction::Edit, 1);
    snprintf(buf, sizeof(buf), "Off timeout: %ds", s.screenOffTimeout);
    addRow(buf, RowAction::Edit, 2);
    addRow("< Back", RowAction::Back);
}

void SettingsScreen::buildIdentityMenu() {
    clearRows();
    if (!_config) return;
    const auto& s = settingsView();
    char buf[40];
    addRow(("Name: " + std::string(s.displayName.isEmpty() ? "(none)" : s.displayName.c_str())), RowAction::Edit, 3);
    addRow("ID: " + std::string(_identityHash.c_str()));
    if (s.announceInterval == handheld::announce::Off) addRow("Auto Announce: OFF", RowAction::Edit, 4);
    else {
        snprintf(buf, sizeof(buf), "Auto Announce: %um", unsigned(s.announceInterval));
        addRow(buf, RowAction::Edit, 4);
    }
    addRow("< Back", RowAction::Back);
}

void SettingsScreen::buildAudioMenu() {
    clearRows();
    if (!_config) return;
    const auto& s = settingsView();
    char buf[40];

    addRow(s.audioEnabled ? "Audio: ON" : "Audio: OFF", RowAction::AudioToggle);
    snprintf(buf, sizeof(buf), "Volume: %d%%", s.audioVolume);
    addRow(buf, RowAction::Edit, 1);
    addRow("< Back", RowAction::Back);
}

// Start editing a field — show TextInput with current value
void SettingsScreen::startEditing(int field, const std::string& currentValue) {
    if (!_active || !_config || _config->settingsPending() || _radioApplyPending) return;
    releaseCandidate();
    try {
    _editField = field;
    _editLabel = _subMenu == MENU_IDENTITY && field == 4 ? "Announce: 0=OFF, 30-360m" : "";
    _editing = true;
    _editInput.clear();
    _editInput.setAllowEmptySubmit((_subMenu == MENU_IDENTITY && field == 3) ||
        (_subMenu == MENU_WIFI && (settingsView().wifiMode == RAT_WIFI_AP ? (field == 0 || field == 1) :
                                  settingsView().wifiMode == RAT_WIFI_STA && field == 1)));
    if (!_editInput.setText(currentValue)) {
        releaseEditor(); showToast("Settings memory unavailable; retry", 2500); return;
    }
    _editInput.setActive(true);
    _editInput.setMaxLength(_subMenu == MENU_WIFI && field == 0 ? 32 : 64);
    // Unsigned numeric fields take digits only; TX power keeps its sign.
    _editInput.setNumericOnly(_subMenu == MENU_DISPLAY || _subMenu == MENU_AUDIO || _subMenu == MENU_VOICE ||
        (_subMenu == MENU_RADIO && field != 4) || (_subMenu == MENU_IDENTITY && field == 4));
    _editInput.setSubmitCallback([this](const std::string& value) {
        commitEdit(value);
    });
    } catch (const std::bad_alloc&) {
        releaseEditor();
        showToast("Settings memory unavailable; retry", 2500);
    }
}

// Apply edited value to settings
void SettingsScreen::commitEdit(const std::string& value) {
    if (_config && (_config->settingsPending() || _radioApplyPending)) { showToast("Settings apply pending"); return; }
    if (!_active || !_editing || !_config) return;
    struct Release { std::optional<UserConfig>& value; ~Release() { value.reset(); } } release{_candidate};
    const Row editedRow = selectedRow();
    const SubMenu editMenu = _subMenu;
    // Host collection is only editor text; it does not need a settings copy.
    if (_subMenu == MENU_TCP && _editField == 99 && !value.empty()) {
        try {
            std::string host(value), label("Port (1-65535):");
            // Prepare before changing the editor stage. A refused allocation
            // leaves the host field intact and ready for another submission.
            if (!_editInput.setText("4242")) {
                showToast("Settings memory unavailable; retry", 2500); return;
            }
            _tcpPendingHost.swap(host);
            _editLabel.swap(label);
            _editField = 100;
            _editInput.setMaxLength(5);
            _editInput.setNumericOnly(true);
        } catch (const std::bad_alloc&) { showToast("Settings memory unavailable; retry", 2500); }
        return;
    }
    // Scalars stage on the stack; only string and list edits clone settings.
    UserScalars next = _config->settings();
    const auto assign = [this, &value](String& target) {
        if (UserConfig::trySetString(target, value.data(), value.size())) return true;
        showToast("Settings memory unavailable; retry", 2500); return false;
    };
    try {

    if (_subMenu == MENU_VOICE) {
        int32_t v;
        if (_editField == 0) {
            if (!handheld::settings::parseInteger(value, 0, 100, v)) { showToast("Volume: 0-100%"); return; }
            next.voice.volume = uint8_t(v);
        } else if (_editField == 1) {
            if (!handheld::settings::parseInteger(value, 1, 50, v)) { showToast("Enter a number from 1 to 50"); return; }
            next.voice.maxMessages = uint8_t(v);
        } else return;
        if (!applyScalars(next)) return;
        buildVoiceMenu();
    } else if (_subMenu == MENU_PROPAGATION_CHOICE) {
        handheld::propagation::Settings parsed;
        if (value.size() != 32 || !parsed.setManual(value.data(), value.size())) {
            showToast("Enter 32 hexadecimal characters", 2500); return;
        }
        if (!savePropagationAddress(parsed.manual)) return;
    } else if (_subMenu == MENU_RADIO) {
        int32_t v;
        if (!handheld::settings::parseInteger(value, -9, LORA_MAX_FREQUENCY, v)) {
            showToast("Enter a valid whole number");
            return;
        }
        switch (_editField) {
            case 0:  // Frequency in Hz, independent of region presets.
                if (v < 0 || v > (long)LORA_MAX_FREQUENCY || !loRaFrequencyBand((uint32_t)v)) {
                    showToast("Unsupported frequency");
                    return;
                }
                next.loraFrequency = (uint32_t)v;
                break;
            case 1:  // SF: 5-12
                if (v < 5 || v > 12) { showToast("SF: 5-12"); return; }
                next.loraSF = (uint8_t)v;
                break;
            case 2:  // BW: 7800-500000
                if (v < 7800 || v > 500000) { showToast("BW: 7800-500000 Hz"); return; }
                next.loraBW = (uint32_t)v;
                break;
            case 3:  // CR: 5-8
                if (v < 5 || v > 8) { showToast("CR: 5-8"); return; }
                next.loraCR = (uint8_t)v;
                break;
            case 4:  // TX Power
                if (v < -9 || v > LORA_MAX_TX_POWER) { showToast("TX power: -9 to 22 dBm"); return; }
                next.loraTxPower = (int8_t)v;
                break;
            case 5:
                if (v < 6 || v > 65) { showToast("Preamble: 6-65 symbols"); return; }
                next.loraPreamble = v;
                break;
            default: return;
        }
        if (!applyScalars(next)) return;
        buildRadioMenu();
    } else if (_subMenu == MENU_TCP) {
        if (_editField == 100 && !value.empty()) {
            // Port submitted — validate and add
            int port = atoi(value.c_str());
            if (port < 1 || port > 65535) {
                showToast("Port 1-65535");
                return;
            } else {
                if (!addTCPConnection(_tcpPendingHost, (uint16_t)port)) return;
            }
            _tcpPendingHost.clear();
        }
        buildTCPMenu();
    } else if (_subMenu == MENU_WIFI) {
        if (!ensureCandidate()) return;
        auto& s = _candidate->settings();
        // Fields: 0=SSID, 1=Password (AP or STA depending on mode)
        if (s.wifiMode == RAT_WIFI_AP) {
            switch (_editField) {
                case 0: if (!assign(s.wifiAPSSID)) return; break;
                case 1: if (!assign(s.wifiAPPassword)) return; break;
            }
        } else if (s.wifiMode == RAT_WIFI_STA) {
            switch (_editField) {
                case 0: if (!assign(staNetwork(s).ssid)) return; break;
                case 1: if (!assign(staNetwork(s).password)) return; break;
            }
        }
        if (!applyAndSave()) return; // Keep the editor and its input for retry.
        if (_config->settings().wifiMode == RAT_WIFI_STA && _editField == 1) {
            connectWiFi();  // Live reconnect with new credentials
        }
        buildWiFiMenu();
    } else if (_subMenu == MENU_DISPLAY || _subMenu == MENU_IDENTITY) {
        int32_t v = 0;
        if (_editField == 4) {
            if (!handheld::settings::parseInteger(value, 0, handheld::announce::MaximumMinutes, v) ||
                (v != handheld::announce::Off && v < handheld::announce::MinimumMinutes)) {
                showToast("Announce: 0=OFF or 30-360m"); return;
            }
        } else if (_editField != 3 && !handheld::settings::parseInteger(value,
                _editField == 0 ? 1 : _editField == 1 ? 5 : 10,
                _editField == 0 ? 100 : _editField == 1 ? 3600 : 7200, v)) {
            showToast(_editField == 0 ? "Brightness: 1-100%" :
                _editField == 1 ? "Dim timeout: 5-3600s" : "Off timeout: 10-7200s");
            return;
        }
        if (_editField == 3) {
            if (!ensureCandidate() || !assign(_candidate->settings().displayName) || !applyAndSave()) return;
        } else {
            // Off may not precede dim: the edited value stands, its pair follows visibly.
            const char* paired = nullptr;
            switch (_editField) {
                case 0: next.brightness = (uint8_t)v; break;
                case 1:
                    next.screenDimTimeout = (uint16_t)v;
                    if (next.screenOffTimeout < v) { next.screenOffTimeout = (uint16_t)v; paired = "Saved; off timeout raised to dim"; }
                    break;
                case 2:
                    next.screenOffTimeout = (uint16_t)v;
                    if (next.screenDimTimeout > v) { next.screenDimTimeout = (uint16_t)v; paired = "Saved; dim timeout lowered to off"; }
                    break;
                case 4: next.announceInterval = uint16_t(v); break;
                default: return;
            }
            if (!applyScalars(next)) return;
            if (paired) showToast(paired, 2500);
        }
        if (_subMenu == MENU_IDENTITY) buildIdentityMenu();
        else buildDisplayMenu();
    } else if (_subMenu == MENU_AUDIO) {
        if (_editField == 1) {
            int32_t v;
            if (!handheld::settings::parseInteger(value, 0, 100, v)) {
                showToast("Volume: 0-100%"); return;
            }
            next.audioVolume = (uint8_t)v;
        }
        if (!applyScalars(next)) return;
        buildAudioMenu();
    }

    _editing = false;
    _editField = -1;
    _editInput.setActive(false);
    releaseCandidate();
    if (_subMenu == editMenu) restoreRow(editedRow);
    } catch (const std::bad_alloc&) { showToast("Settings memory unavailable; retry", 2500); }
}

// Get current value of a field as string for editing
std::string SettingsScreen::getCurrentValue(SubMenu menu, int field) {
    if (!_config) return "";
    const auto& s = settingsView();
    char buf[32];

    if (menu == MENU_RADIO) {
        switch (field) {
            case 0: snprintf(buf, sizeof(buf), "%lu", (unsigned long)s.loraFrequency); return buf;
            case 1: snprintf(buf, sizeof(buf), "%d", s.loraSF); return buf;
            case 2: snprintf(buf, sizeof(buf), "%lu", (unsigned long)s.loraBW); return buf;
            case 3: snprintf(buf, sizeof(buf), "%d", s.loraCR); return buf;
            case 4: snprintf(buf, sizeof(buf), "%d", s.loraTxPower); return buf;
            case 5: snprintf(buf, sizeof(buf), "%ld", s.loraPreamble); return buf;
        }
    } else if (menu == MENU_WIFI) {
        if (s.wifiMode == RAT_WIFI_AP) {
            switch (field) {
                case 0: return s.wifiAPSSID.c_str();
                case 1: return s.wifiAPPassword.c_str();
            }
        } else if (s.wifiMode == RAT_WIFI_STA) {
            switch (field) {
                case 0: return staNetworkRO(s).ssid.c_str();
                case 1: return staNetworkRO(s).password.c_str();
            }
        }
    } else if (menu == MENU_DISPLAY || menu == MENU_IDENTITY) {
        switch (field) {
            case 0: snprintf(buf, sizeof(buf), "%d", s.brightness); return buf;
            case 1: snprintf(buf, sizeof(buf), "%d", s.screenDimTimeout); return buf;
            case 2: snprintf(buf, sizeof(buf), "%d", s.screenOffTimeout); return buf;
            case 3: return s.displayName.c_str();
            case 4: snprintf(buf, sizeof(buf), "%u", unsigned(s.announceInterval)); return buf;
        }
    } else if (menu == MENU_AUDIO) {
        if (field == 1) { snprintf(buf, sizeof(buf), "%d", s.audioVolume); return buf; }
    }
    return "";
}

void SettingsScreen::render(M5Canvas& canvas) {
    if (!_editing) releaseEditor();
    pollPropagationUI();
    if (!_config) {
        Theme::useSmallFont(canvas); canvas.setTextColor(Theme::MUTED);
        canvas.drawString("Settings unavailable", 4, Theme::CONTENT_Y + 8);
        canvas.drawString("Esc=back", 4, Theme::CONTENT_Y + 22);
        return;
    }
    if (_subMenu == MENU_ABOUT) {
        char value[32];
        for (size_t i = 0; i < _rowCount; ++i) {
            if (_rows[i].action == RowAction::Heap) {
                snprintf(value, sizeof value, "Heap: %lu bytes", (unsigned long)ESP.getFreeHeap());
                _list.updateItem(i, value, 0);
            } else if (_rows[i].action == RowAction::Uptime) {
                snprintf(value, sizeof value, "Up: %lus", millis() / 1000);
                _list.updateItem(i, value, 0);
            }
        }
    }

    int y0 = Theme::CONTENT_Y;

    // Header with accent bar
    const char* headers[] = {"SETTINGS", "LORA", "WI-FI & TCP", "TCP CONNECTIONS",
                             "STORAGE", "DISPLAY", "ALERTS", "DEVICE", "WIFI SCAN",
                             "PROPAGATION", "MANUAL NODE", "PROPAGATION NODES", "VOICE MESSAGES",
                             "IDENTITY", "LINK PRESETS", "WI-FI MODE"};
    static_assert(sizeof(headers) / sizeof(headers[0]) == MENU_COUNT,
                  "Every settings menu needs a visible header");
    const int headerH = Theme::SECTION_HEADER_H;
    canvas.fillRect(0, y0, Theme::CONTENT_W, headerH, Theme::BG_SURFACE);
    canvas.fillRect(0, y0 + 2, 3, headerH - 4, Theme::ACCENT);
    canvas.setTextColor(Theme::ACCENT);
    Theme::useUiFont(canvas);
    canvas.drawString(headers[_subMenu], 8, y0 + 2);
    canvas.drawFastHLine(0, y0 + headerH, Theme::CONTENT_W, Theme::DIVIDER);
    Theme::useSmallFont(canvas);

    if (_editing) {
        // Show field name
        canvas.setTextColor(Theme::MUTED);
        std::string label = _editLabel.empty() ? "Edit value:" : _editLabel;
        canvas.drawString(label.c_str(), 4, y0 + headerH + 5);

        // Show text input
        _editInput.render(canvas, 0, y0 + headerH + 19, Theme::CONTENT_W);

        // Hint
        canvas.setTextColor(Theme::MUTED);
        canvas.drawString("Enter=save", 4, y0 + headerH + 36);
        if (_subMenu == MENU_VOICE && _editField == 1) {
            canvas.drawString(handheld::ui::voice_settings::StorageScope, 4, y0 + headerH + 50);
            canvas.drawString(handheld::ui::voice_settings::Expiry, 4, y0 + headerH + 62);
        }
        if (_subMenu == MENU_RADIO && (_editField == 0 || _editField == 4)) {
            // Frequency and power edits wrap the shared line for this width.
            const std::string rules = handheld::ui::radio_settings::LocalRules;
            const size_t cut = rules.rfind(' ', size_t(Theme::CONTENT_W - 8) / Theme::CHAR_W);
            canvas.drawString(rules.substr(0, cut).c_str(), 4, y0 + headerH + 50);
            canvas.drawString(rules.substr(cut + 1).c_str(), 4, y0 + headerH + 62);
        }
    } else {
        _list.render(canvas, 0, y0 + headerH + 2, Theme::CONTENT_W,
                     Theme::CONTENT_H - headerH - 3, true,
                     _subMenu == MENU_PROPAGATION || _subMenu == MENU_PROPAGATION_NODES ||
                     _subMenu == MENU_IDENTITY);
        if (_subMenu == MENU_WIFI) _drawnWiFiAction = _wifiAction;
    }

    // Confirmation dialog overlay
    if (_confirmPending) {
        const char* prompt = _confirmAction == 0 ? "Factory Reset? Y/N" :
            _confirmAction == 1 ? "Wipe SD Data? Y/N" : "Init SD & Restart? Y/N";
        int tw = strlen(prompt) * Theme::CHAR_W + 16;
        int th = Theme::CHAR_H + 10;
        int tx = (Theme::CONTENT_W - tw) / 2;
        int ty = Theme::CONTENT_Y + Theme::CONTENT_H / 2 - th / 2;
        canvas.fillRoundRect(tx, ty, tw, th, 3, Theme::BG);
        canvas.drawRoundRect(tx, ty, tw, th, 3, Theme::ERROR);
        canvas.setTextColor(Theme::ERROR);
        canvas.setCursor(tx + 8, ty + 5);
        canvas.print(prompt);
    }

    // Toast overlay (drawn on top of everything)
    if (_toastMessage && millis() < _toastUntil) {
        drawSettingsNotice(canvas, _toastMessage);
    } else {
        _toastMessage = nullptr;
    }
}

void SettingsScreen::buildDeviceMenu() {
    clearRows();
    addRow("rsCardputer v" RSCARDPUTER_VERSION_STRING);
    addRow("M5Stack Cardputer Adv");
    addRow("Cap LoRa-1262 (SX1262)");
    char value[32];
    snprintf(value, sizeof value, "Heap: %lu bytes", (unsigned long)ESP.getFreeHeap());
    addRow(value, RowAction::Heap);
    snprintf(value, sizeof value, "Up: %lus", millis() / 1000);
    addRow(value, RowAction::Uptime);
    addRow("Factory Reset", RowAction::FactoryReset, 0, Theme::ERROR);
    addRow("< Back", RowAction::Back);
}

bool SettingsScreen::handleKey(const KeyEvent& event) {
    if (!_active) return false;
    if (!_editing) releaseEditor();
    if (!_config) {
        if ((event.escape || event.backspace) && _backCb) { _backCb(); return true; }
        return !(event.tab || event.navLeft() || event.navRight());
    }
    if (event.repeat && (event.forwardDelete || (event.backspace && !_editing))) return true;
    // Handle confirmation dialog
    if (_confirmPending) {
        if (event.character == 'y' || event.character == 'Y') {
            _confirmPending = false;
            if (_confirmAction == 0) {
                factoryReset();
            } else if (_confirmAction == 2) {
                sdCardFormat();
            } else {
                if (_sdStore && _sdStore->isReady()) {
                    requestMaintenance(handheld::Operation::WipeSD);
                }
            }
        } else if (event.character == 'n' || event.character == 'N' ||
                   event.escape || event.backspace) {
            _confirmPending = false;
            showToast("Cancelled");
        }
        return true;
    }

    // Escape goes back from anywhere; a fresh Backspace also leaves an empty editor.
    if (event.escape || (event.backspace && !event.repeat &&
                         (!_editing || _editInput.getText().empty()))) {
        if (_editing) {
            releaseCandidate();
            releaseEditor();
            std::vector<WiFiNetwork>().swap(_scanResults);
            // Rows still describe the committed values from before the editor
            // opened. Cancelling needs neither a new snapshot nor new rows.
            return true;
        }
        if (_subMenu != MENU_MAIN) {
            try { backMenu(); }
            catch (const std::bad_alloc&) {
                // Even a refused parent menu allocation must not block exit.
                if (_backCb) _backCb();
            }
            return true;
        }
        if (_backCb) { _backCb(); return true; }
        return false;
    }

    // Persistence/radio work belongs to the board owner and may outlive this
    // screen. It blocks another mutation, never navigation out of Settings.
    if (_config->settingsPending() || _radioApplyPending) {
        if (event.tab || event.navLeft() || event.navRight()) return false;
        if (_editing) { showToast("Settings apply pending"); return true; }
        if (event.enter && selectedRow().action != RowAction::OpenMenu && selectedRow().action != RowAction::Back) {
            showToast("Settings apply pending"); return true;
        }
        if (event.forwardDelete) { showToast("Settings apply pending"); return true; }
    }

    if (_editing) {
        bool consumed = false;
        try { consumed = _editInput.handleKey(event); }
        catch (const std::bad_alloc&) { showToast("Settings memory unavailable; retry", 2500); return true; }
        // A submit may close the editor. Retire its callable/text only after
        // TextInput has returned, so its callback and argument remain alive.
        if (!_editing) {
            releaseEditor();
            std::vector<WiFiNetwork>().swap(_scanResults);
        }
        return consumed;
    }

    // The printed Fn+Backspace Delete action removes a TCP connection.
    if (event.forwardDelete && _subMenu == MENU_TCP) {
        const Row selected = selectedRow();
        if (selected.action == RowAction::TCPToggle) {
            removeTCPConnection(selected.value);
            return true;
        }
    }

    if (_subMenu == MENU_WIFI_SCAN && _scanResults.empty() && (event.navUp() || event.navDown())) {
        _list.setSelected(event.navUp() ? _list.itemCount() - 2 : _list.itemCount() - 1);
        return true;
    }
    if (event.navUp()) {
        _list.scrollUp();
        return true;
    }
    if (event.navDown()) {
        _list.scrollDown();
        return true;
    }

    // Selection dispatch is independent of the row's current position.
    if (event.enter) {
        if (!_rowCount) { onEnter(); return true; }
        const Row selected = selectedRow();
        try {
        switch (selected.action) {
            case RowAction::OpenMenu: enterMenu(SubMenu(selected.value)); break;
            case RowAction::Back: backMenu(); break;
            case RowAction::Edit:
                startEditing(selected.value, getCurrentValue(_subMenu, selected.value)); break;
            case RowAction::Preset: applyRadioPreset(selected.value); break;
            case RowAction::WiFiMode: {
                // One save per explicit choice; reselecting the active mode only returns.
                UserScalars next = _config->settings();
                if (next.wifiMode != RatWiFiMode(selected.value)) {
                    next.wifiMode = RatWiFiMode(selected.value);
                    if (next.wifiMode != RAT_WIFI_OFF) next.wifiRestoreMode = next.wifiMode;
                    if (!applyScalars(next)) break;
                    showToast("Reboot to apply");
                }
                backMenu(); break;
            }
            case RowAction::WiFiConnection:
                if (!event.repeat) activateWiFiAction();
                break;
            case RowAction::WiFiScan:
                if (!event.repeat) startWiFiScan();
                break;
            case RowAction::WiFiNetwork:
                if (!event.repeat) selectNetwork(selected.value);
                break;
            case RowAction::AutoDiscover: {
                // The network owner starts or stops discovery live.
                UserScalars next = _config->settings();
                next.autoIfaceEnabled = !next.autoIfaceEnabled;
                applyScalars(next);
                buildWiFiMenu(); restoreRow(selected); break;
            }
            case RowAction::TCPAdd:
                startEditing(99, ""); _editLabel = "Host:";
                _editInput.setNumericOnly(false); break;
            case RowAction::TCPToggle:
                toggleTCPConnection(selected.value); restoreRow(selected); break;
            case RowAction::AudioToggle: {
                UserScalars next = _config->settings(); next.audioEnabled = !next.audioEnabled;
                applyScalars(next); buildAudioMenu(); break;
            }
            case RowAction::SDInitialize: case RowAction::SDWipe:
                if (_sdStore && _sdStore->isReady()) {
                    _confirmPending = true; _confirmAction = selected.action == RowAction::SDWipe ? 1 : 2;
                } else showToast("No SD card");
                break;
            case RowAction::FactoryReset: _confirmPending = true; _confirmAction = 0; break;
            case RowAction::Voice: activateVoiceRow(selected.value); break;
            case RowAction::Propagation: activatePropagationRow(selected.value); break;
            case RowAction::Info: case RowAction::Heap: case RowAction::Uptime: break;
        }
        } catch (const std::bad_alloc&) {
            releaseCandidate();
            showToast("Settings memory unavailable; retry", 2500);
        }
        return true;
    }

    return false;
}

void SettingsScreen::showToast(const char* msg, unsigned long durationMs) {
    _toastMessage = msg;
    _toastUntil = millis() + durationMs;
}

bool SettingsScreen::applyAndSave() {
    struct Release { std::optional<UserConfig>& value; ~Release() { value.reset(); } } release{_candidate};
    if (!_config || !_flash || !_candidate) { showToast("Settings unavailable"); return false; }
    if (_radioApplyPending) { showToast("Waiting for radio", 2500); return false; }
    if (_config->settingsPending()) { showToast("Settings recovery pending", 2500); return false; }
    if (!_saveCb) { showToast("Settings unavailable"); return false; }
    const auto saved = _saveCb(*_candidate);
    // The editor retains the user's text; retries re-stage it against the
    // latest committed config. No hidden/failed full snapshot is retained.
    releaseCandidate();
    return finishSave(saved);
}

bool SettingsScreen::applyScalars(const UserScalars& scalars) {
    if (!_active || !_config) return false;
    if (_config->settingsPending() || _radioApplyPending) { showToast("Settings apply pending"); return false; }
    if (!_flash || !_scalarSaveCb) { showToast("Settings unavailable"); return false; }
    releaseCandidate(); // A staged full copy would be stale after this commit.
    return finishSave(_scalarSaveCb(scalars));
}

bool SettingsScreen::finishSave(const SettingsTransaction::Result& saved) {
    if (!saved.complete()) {
        if (_config->settingsPending()) {
            _editing = false; _editInput.setActive(false);
        }
        showToast(saved.detail ? saved.detail : "Save failed; retry changes", 2500);
        return false;
    }
    applyCommitted(false);
    if (_radioApplyPending) { _editing = false; _editInput.setActive(false); }
    return !_radioApplyPending;
}

void SettingsScreen::applyCommitted(bool refreshCandidate) {
    if (!_config || _config->settingsPending()) return;
    if (refreshCandidate) {
        releaseCandidate();
        _editing = false; _editInput.setActive(false);
    }
    auto& s = _config->settings();
    if (_backend) {_backend->configureVoice(s.voice);_backend->configurePropagation(s.propagation);}
    const auto radio = _radioApply ? _radioApply(s, true) : RadioApply::Unavailable;

    // Apply power settings
    if (_power) {
        _power->setDimTimeout(s.screenDimTimeout);
        _power->setOffTimeout(s.screenOffTimeout);
        _power->setBrightness(s.brightness);
    }

    // Apply audio settings
    if (_audio) {
        _audio->setEnabled(s.audioEnabled);
        _audio->setVolume(s.audioVolume);
    }

    finishRadioApply(radio);
    if (_active && refreshCandidate) {
        const Row selected = selectedRow();
        rebuildMenu(); restoreRow(selected);
    }
}

void SettingsScreen::finishRadioApply(RadioApply result) {
    _radioApplyPending = result == RadioApply::Pending;
    if (_active && (_subMenu == MENU_RADIO || _subMenu == MENU_RADIO_PRESETS)) {
        const Row selected = selectedRow();
        rebuildMenu(); restoreRow(selected);
    }
    if (_radioApplyPending) { showToast("Saved; waiting for radio", 2500); return; }
    const bool announce = _presetAnnouncePending && result == RadioApply::Applied;
    _presetAnnouncePending = false; // Retire before any backend callback.
    if (result == RadioApply::RebootRequired) { showToast("Saved; radio needs reboot", 2500); return; }
    if (result == RadioApply::Unavailable) { showToast("Saved; radio unavailable", 2500); return; }
    if (!announce) { showToast("Settings saved"); return; }
    if (!_backend || !_backend->protocolReady()) { showToast("Preset applied (no announce)"); return; }
    const auto appData = encodeAnnounceName(_config->settings().displayName, _backend->destinationHashHex().c_str());
    const auto sent = _backend->announce(appData.data(), appData.size());
    showToast(sent == ProtocolBackend::AnnounceResult::Sent ? "Preset applied + announced" :
        sent == ProtocolBackend::AnnounceResult::Deferred ? "Preset applied; announce queued" :
        "Preset applied (announce failed)");
}

bool SettingsScreen::pollRadioApply(bool accepting) {
    if (!accepting) {
        const bool pending = _radioApplyPending;
        _radioApplyPending = _presetAnnouncePending = false;
        if (pending && _radioApply) _radioApply(_config->settings(), false);
        return pending;
    }
    if (!_radioApplyPending) return false;
    const auto result = _radioApply ? _radioApply(_config->settings(), true) : RadioApply::Unavailable;
    if (result == RadioApply::Pending) return false;
    finishRadioApply(result);
    return true;
}

void SettingsScreen::applyRadioPreset(int preset) {
    if (_config && (_config->settingsPending() || _radioApplyPending)) { showToast("Settings apply pending"); return; }
    if (!_config || preset < 0 || preset >= RadioPresets::count) return;
    UserScalars next = _config->settings();
    RadioPresets::apply(next, preset);
    _presetAnnouncePending = true;
    const bool saved = applyScalars(next);
    if (!saved && !_config->settingsPending() && !_radioApplyPending) _presetAnnouncePending = false;
    const Row selected = selectedRow();
    rebuildMenu(); restoreRow(selected);
}

void SettingsScreen::factoryReset() {
    requestMaintenance(handheld::Operation::FactoryReset);
}

void SettingsScreen::requestMaintenance(handheld::Operation operation) {
    _maintenanceReason="Maintenance unavailable";
    if (!_maintenanceCb || !_maintenanceCb(operation)) showToast(_maintenanceReason);
}
