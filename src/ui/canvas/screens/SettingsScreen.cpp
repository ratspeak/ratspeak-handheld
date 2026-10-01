#include "SettingsScreen.h"
#include "Theme.h"
#include "config/Config.h"
#include "config/SettingsInput.h"
#include "config/AnnounceInterval.h"
#include "radio/RadioFrequency.h"
#include "radio/RadioPresets.h"
#include "radio/RadioSettings.h"
#include "util/DisplayText.h"
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
    if (!_config || _candidate.settings().wifiMode != RAT_WIFI_STA) return WiFiAction::None;
    if (WiFi.status() == WL_CONNECTED) return WiFiAction::Disconnect;
    if (_network.connecting && _network.connecting()) return WiFiAction::Cancel;
    return staNetworkRO(_candidate.settings()).ssid.isEmpty() ? WiFiAction::None : WiFiAction::Connect;
}

void SettingsScreen::onEnter() {
    _candidateReady = _config && _candidate.tryAssign(*_config);
    if (_candidateReady) _candidateDirty = false;
    _subMenu = MENU_MAIN;
    _editing = false;
    _editField = -1;
    _propChoicePending = false; _propCount = 0;
    buildMainMenu();
}

void SettingsScreen::buildMainMenu() {
    _list.clear();
    _list.addItem("Radio");
    _list.addItem("Wi-Fi");
    _list.addItem("SD Card");
    _list.addItem("Display");
    _list.addItem("Audio");
    _list.addItem("About");
    _list.addItem("Factory Reset", Theme::ERROR);
    _list.addItem("Propagation");
    _list.addItem("Voice");
}

void SettingsScreen::buildRadioMenu() {
    _list.clear();
    if (!_config) return;
    auto& s = _candidate.settings();
    char buf[40];

    // Active preset indicator
    snprintf(buf, sizeof(buf), "%s: %s", (_radioApplyPending || _config->settingsPending()) ? "Pending" : _candidateDirty ? "Unsaved" : "Saved", RadioPresets::name(s));
    _list.addItem(buf);

    // Presets (items 1..RadioPresets::count)
    int activeIdx = RadioPresets::detect(s);
    for (int i = 0; i < RadioPresets::count; i++) {
        char label[40];
        snprintf(label, sizeof(label), "%s[%s]",
                 (i == activeIdx) ? ">" : " ", RadioPresets::values[i].name);
        _list.addItem(label, (i == activeIdx) ? Theme::PRIMARY : 0);
    }

    // Editable fields (items RadioPresets::count+1 .. RadioPresets::count+6)
    snprintf(buf, sizeof(buf), "Frequency: %lu Hz", (unsigned long)s.loraFrequency);
    _list.addItem(buf);
    snprintf(buf, sizeof(buf), "SF: %d", s.loraSF);
    _list.addItem(buf);
    snprintf(buf, sizeof(buf), "BW: %lu Hz", (unsigned long)s.loraBW);
    _list.addItem(buf);
    snprintf(buf, sizeof(buf), "CR: %d", s.loraCR);
    _list.addItem(buf);
    snprintf(buf, sizeof(buf), "TX Power: %d dBm", s.loraTxPower);
    _list.addItem(buf);
    snprintf(buf, sizeof(buf), "Preamble: %ld symbols", s.loraPreamble);
    _list.addItem(buf);

    _list.addItem("< Back");
}

void SettingsScreen::buildWiFiMenu() {
    _list.clear();
    _wifiAction = currentWiFiAction();
    _drawnWiFiAction = WiFiAction::None;
    if (!_config) return;
    auto& s = _candidate.settings();

    // Item 0: Mode selector
    const char* modeNames[] = {"OFF", "AP", "STA"};
    char modeBuf[24];
    snprintf(modeBuf, sizeof(modeBuf), "Mode: %s", modeNames[s.wifiMode]);
    _list.addItem(modeBuf);

    if (s.wifiMode == RAT_WIFI_AP) {
        // Items 1-2: AP fields
        String apSSID = s.wifiAPSSID.isEmpty() ? "(auto)" : s.wifiAPSSID;
        _list.addItem(("AP SSID: " + std::string(apSSID.c_str())));
        _list.addItem(s.wifiAPPassword.isEmpty() ? "AP Pass: (open)" : "AP Pass: ********");
    } else if (s.wifiMode == RAT_WIFI_STA) {
        // Item 1: Connection status
        if (_wifiAction == WiFiAction::Disconnect) {
            char statusBuf[48];
            snprintf(statusBuf, sizeof(statusBuf), "Connected: %s", WiFi.SSID().c_str());
            _list.addItem(statusBuf);
            _list.addItem("[Disconnect]");          // Item 2
        } else if (_wifiAction == WiFiAction::Cancel) {
            _list.addItem("Connecting...");
            _list.addItem("[Cancel]");              // Item 2
        } else if (_wifiAction == WiFiAction::Connect) {
            char statusBuf[48];
            snprintf(statusBuf, sizeof(statusBuf), "Saved: %s (offline)", staNetworkRO(s).ssid.c_str());
            _list.addItem(statusBuf);
            _list.addItem("[Connect]");             // Item 2
        } else {
            _list.addItem("No network configured");
            _list.addItem("");                      // Item 2 placeholder
        }
        _list.addItem("Scan Networks");             // Item 3
        _list.addItem("TCP Connections");            // Item 4
        // Item 5: AutoInterface (LAN auto-discovery via IPv6 multicast)
        char autoBuf[40];
        snprintf(autoBuf, sizeof(autoBuf), "Auto-discover LAN: %s",
                 s.autoIfaceEnabled ? "ON" : "OFF");
        _list.addItem(autoBuf);
    }
    // OFF mode: only mode selector + back

    _list.addItem("< Back");
}

void SettingsScreen::buildTCPMenu() {
    _list.clear();
    if (!_config) return;
    auto& s = _candidate.settings();

    _list.addItem("+ Add Connection");

    for (size_t i = 0; i < s.tcpConnections.size(); i++) {
        auto& ep = s.tcpConnections[i];
        char buf[64];
        snprintf(buf, sizeof(buf), "%s:%d [%s]",
                 ep.host.c_str(), ep.port,
                 ep.autoConnect ? "ON" : "OFF");
        _list.addItem(buf);
    }

    _list.addItem("< Back");
}

void SettingsScreen::addTCPConnection(const std::string& host, uint16_t port) {
    if (!_config) return;
    auto& s = _candidate.settings();
    if (s.tcpConnections.size() >= MAX_TCP_CONNECTIONS) {
        showToast("Max 4 connections");
        return;
    }

    TCPEndpoint ep;
    if (!UserConfig::trySetString(ep.host, host.data(), host.size())) {
        showToast("Settings memory unavailable; retry", 2500); return;
    }
    ep.port = port;
    if (ep.host.isEmpty()) return;

    // Reachability is transient. The transport owner attempts saved endpoints;
    // saving one must not block this UI or depend on the peer being online now.
    try { s.tcpConnections.push_back(std::move(ep)); }
    catch (const std::bad_alloc&) { showToast("Settings memory unavailable; retry", 2500); return; }
    if (applyAndSave()) showToast("Added! Reboot to connect");
    buildTCPMenu();
}

void SettingsScreen::toggleTCPConnection(int index) {
    if (!_config) return;
    auto& s = _candidate.settings();
    if (index < 0 || index >= (int)s.tcpConnections.size()) return;

    s.tcpConnections[index].autoConnect = !s.tcpConnections[index].autoConnect;
    applyAndSave();
    buildTCPMenu();
}

void SettingsScreen::removeTCPConnection(int index) {
    if (!_config) return;
    auto& s = _candidate.settings();
    if (index < 0 || index >= (int)s.tcpConnections.size()) return;

    s.tcpConnections.erase(s.tcpConnections.begin() + index);
    if (applyAndSave()) showToast("Removed");
    buildTCPMenu();
}

// =============================================================================
// SD Card submenu
// =============================================================================

void SettingsScreen::buildSDCardMenu() {
    _list.clear();

    if (_sdStore && _sdStore->isReady()) {
        _list.addItem("Status: INSERTED");

        char buf[32];
        uint64_t total = _sdStore->totalBytes();
        uint64_t used = _sdStore->usedBytes();
        uint64_t free = total > used ? total - used : 0;

        snprintf(buf, sizeof(buf), "Total: %llu MB", total / (1024 * 1024));
        _list.addItem(buf);
        snprintf(buf, sizeof(buf), "Used: %llu MB", used / (1024 * 1024));
        _list.addItem(buf);
        snprintf(buf, sizeof(buf), "Free: %llu MB", free / (1024 * 1024));
        _list.addItem(buf);

        _list.addItem("Initialize & Restart");
        _list.addItem("Wipe Data & Restart", Theme::ERROR);
    } else {
        _list.addItem("Status: NOT INSERTED");
    }

    _list.addItem("< Back");
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
    _subMenu = MENU_WIFI_SCAN;
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
    if (result == handheld::ScanResult::Ready) {
        JsonDocument doc;
        if (deserializeJson(doc, json) || !doc.is<JsonArray>() || doc.size() > 15) {
            _scanOutcome = handheld::ScanResult::Failed;
        } else {
            for (const auto row : doc.as<JsonArray>()) {
                if (!row["ssid"].is<const char*>() || !row["rssi"].is<int>() || !row["encrypted"].is<bool>()) {
                    _scanResults.clear(); _scanOutcome = handheld::ScanResult::Failed; break;
                }
                WiFiNetwork network;
                const char* ssid = row["ssid"].as<const char*>();
                if (!UserConfig::trySetString(network.ssid, ssid, strlen(ssid))) {
                    _scanResults.clear(); _scanOutcome = handheld::ScanResult::Failed; break;
                }
                network.rssi = row["rssi"].as<int>();
                network.encType = row["encrypted"].as<bool>() ? 1 : WIFI_AUTH_OPEN;
                try { _scanResults.push_back(std::move(network)); }
                catch (const std::bad_alloc&) {
                    _scanResults.clear(); _scanOutcome = handheld::ScanResult::Failed; break;
                }
            }
        }
    }
    if (result == handheld::ScanResult::Ready && _scanOutcome == handheld::ScanResult::Failed)
        Serial.printf("[WIFI] Scan results unavailable (bytes=%u heap=%lu largest=%lu)\n",
                      (unsigned)json.length(), (unsigned long)ESP.getFreeHeap(),
                      (unsigned long)ESP.getMaxAllocHeap());
    if (_subMenu == MENU_WIFI_SCAN) buildScanResultsMenu();
    return true;
}

void SettingsScreen::buildScanResultsMenu() {
    _list.clear();

    if (_scanPending) {
        _list.addItem("Scanning...");
    } else if (_scanOutcome == handheld::ScanResult::Failed) {
        _list.addItem("Scan failed; retry");
    } else if (_scanOutcome == handheld::ScanResult::Cancelled) {
        _list.addItem("Scan cancelled");
    } else if (_scanResults.empty()) {
        _list.addItem("No networks found");
    } else {
        for (auto& net : _scanResults) {
            char buf[48];
            const char* lock = (net.encType == WIFI_AUTH_OPEN) ? "" : "*";
            snprintf(buf, sizeof(buf), "%s%s (%d dBm)",
                     lock, net.ssid.c_str(), net.rssi);
            _list.addItem(buf);
        }
    }

    _list.addItem("Rescan");
    _list.addItem("< Back");
}

void SettingsScreen::disconnectWiFi() {
    if (!_network.disconnect) { showToast("Network unavailable"); return; }
    _network.disconnect();
    showToast(_wifiAction == WiFiAction::Cancel ? "Connection cancelled" : "Disconnected");
    buildWiFiMenu();
}

bool SettingsScreen::pollWiFiStatus() {
    if (_subMenu != MENU_WIFI || _editing || _wifiAction == currentWiFiAction()) return false;
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
    _list.setSelected(2);
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

    auto& s = _candidate.settings();
    staNetwork(s).ssid = _scanResults[index].ssid;
    Serial.printf("[WIFI] Selected: %s\n", staNetwork(s).ssid.c_str());

    // Open password editor
    _subMenu = MENU_WIFI;
    // field 1 = STA password in WiFi STA mode
    startEditing(1, staNetworkRO(s).password.c_str());
    // A failed transaction restores canonical settings. Keep the selected
    // network bound to retries through the existing, stable scan-result row.
    _editInput.setSubmitCallback([this, index](const std::string& value) {
        if (!_editing || _subMenu != MENU_WIFI || _editField != 1 ||
            !_config || _candidate.settings().wifiMode != RAT_WIFI_STA ||
            index < 0 || size_t(index) >= _scanResults.size()) {
            showToast("Network selection expired");
            return;
        }
        const auto& selectedSSID = _scanResults[index].ssid;
        try {
            if (!UserConfig::trySetString(staNetwork(_candidate.settings()).ssid,
                    selectedSSID.c_str(), selectedSSID.length())) {
                showToast("Settings memory unavailable; retry", 2500); return;
            }
        } catch (const std::bad_alloc&) { showToast("Settings memory unavailable; retry", 2500); return; }
        commitEdit(value);
    });
}

// =============================================================================
// Display / Audio menus
// =============================================================================

void SettingsScreen::buildDisplayMenu() {
    _list.clear();
    if (!_config) return;
    auto& s = _candidate.settings();
    char buf[40];

    snprintf(buf, sizeof(buf), "Brightness: %d%%", s.brightness);
    _list.addItem(buf);
    snprintf(buf, sizeof(buf), "Dim timeout: %ds", s.screenDimTimeout);
    _list.addItem(buf);
    snprintf(buf, sizeof(buf), "Off timeout: %ds", s.screenOffTimeout);
    _list.addItem(buf);

    String name = s.displayName.isEmpty() ? "(none)" : s.displayName;
    _list.addItem(("Name: " + std::string(name.c_str())));
    if (s.announceInterval == handheld::announce::Off) _list.addItem("Auto Announce: OFF");
    else {
        snprintf(buf, sizeof(buf), "Auto Announce: %um", unsigned(s.announceInterval));
        _list.addItem(buf);
    }
    _list.addItem("< Back");
}

void SettingsScreen::buildAudioMenu() {
    _list.clear();
    if (!_config) return;
    auto& s = _candidate.settings();
    char buf[40];

    _list.addItem(s.audioEnabled ? "Audio: ON" : "Audio: OFF");
    snprintf(buf, sizeof(buf), "Volume: %d%%", s.audioVolume);
    _list.addItem(buf);
    _list.addItem("< Back");
}

// Start editing a field — show TextInput with current value
void SettingsScreen::startEditing(int field, const std::string& currentValue) {
    _editField = field;
    _editLabel = _subMenu == MENU_DISPLAY && field == 4 ? "Announce: 0=OFF, 30-360m" : "";
    _editing = true;
    _editInput.clear();
    _editInput.setText(currentValue);
    _editInput.setActive(true);
    _editInput.setMaxLength(_subMenu == MENU_WIFI && field == 0 ? 32 : 64);
    _editInput.setSubmitCallback([this](const std::string& value) {
        commitEdit(value);
    });
}

// Apply edited value to settings
void SettingsScreen::commitEdit(const std::string& value) {
    if (_config && (_config->settingsPending() || _radioApplyPending)) { showToast("Settings apply pending"); return; }
    if (!_config) return;
    auto& s = _candidate.settings();
    const auto assign = [this, &value](String& target) {
        if (UserConfig::trySetString(target, value.data(), value.size())) return true;
        showToast("Settings memory unavailable; retry", 2500); return false;
    };
    try {

    if (_subMenu == MENU_PROPAGATION_CHOICE) {
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
                s.loraFrequency = (uint32_t)v;
                break;
            case 1:  // SF: 5-12
                if (v < 5 || v > 12) { showToast("SF: 5-12"); return; }
                s.loraSF = (uint8_t)v;
                break;
            case 2:  // BW: 7800-500000
                if (v < 7800 || v > 500000) { showToast("BW: 7800-500000 Hz"); return; }
                s.loraBW = (uint32_t)v;
                break;
            case 3:  // CR: 5-8
                if (v < 5 || v > 8) { showToast("CR: 5-8"); return; }
                s.loraCR = (uint8_t)v;
                break;
            case 4:  // TX Power
                if (v < -9 || v > LORA_MAX_TX_POWER) { showToast("TX power: -9 to 22 dBm"); return; }
                s.loraTxPower = (int8_t)v;
                break;
            case 5:
                if (v < 6 || v > 65) { showToast("Preamble: 6-65 symbols"); return; }
                s.loraPreamble = v;
                break;
            default: return;
        }
        if (!applyAndSave()) return;
        buildRadioMenu();
    } else if (_subMenu == MENU_TCP) {
        if (_editField == 99 && !value.empty()) {
            // Host submitted — stash and open port input
            _tcpPendingHost = value;
            _editField = 100;
            _editing = true;
            _editLabel = "Port (1-65535):";
            _editInput.clear();
            _editInput.setText("4242");
            _editInput.setActive(true);
            _editInput.setMaxLength(5);
            _editInput.setNumericOnly(true);
            _editInput.setSubmitCallback([this](const std::string& v) {
                commitEdit(v);
            });
            return;  // Stay in editing mode
        }
        if (_editField == 100 && !value.empty()) {
            // Port submitted — validate and add
            int port = atoi(value.c_str());
            if (port < 1 || port > 65535) {
                showToast("Port 1-65535");
                buildTCPMenu();
            } else {
                addTCPConnection(_tcpPendingHost, (uint16_t)port);
            }
            _tcpPendingHost.clear();
        }
        buildTCPMenu();
    } else if (_subMenu == MENU_WIFI) {
        // Fields: 0=SSID, 1=Password (AP or STA depending on mode)
        if (_candidate.settings().wifiMode == RAT_WIFI_AP) {
            switch (_editField) {
                case 0: if (!assign(s.wifiAPSSID)) return; break;
                case 1: if (!assign(s.wifiAPPassword)) return; break;
            }
        } else if (_candidate.settings().wifiMode == RAT_WIFI_STA) {
            switch (_editField) {
                case 0: if (!assign(staNetwork(s).ssid)) return; break;
                case 1: if (!assign(staNetwork(s).password)) return; break;
            }
        }
        if (!applyAndSave()) return; // Keep the editor and its input for retry.
        if (_candidate.settings().wifiMode == RAT_WIFI_STA && _editField == 1) {
            connectWiFi();  // Live reconnect with new credentials
        }
        buildWiFiMenu();
    } else if (_subMenu == MENU_DISPLAY) {
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
        switch (_editField) {
            case 0: s.brightness = (uint8_t)v; break;
            case 1: s.screenDimTimeout = (uint16_t)v; break;
            case 2: s.screenOffTimeout = (uint16_t)v; break;
            case 3: if (!assign(s.displayName)) return; break;
            case 4: s.announceInterval = uint16_t(v); break;
            default: return;
        }
        if (!applyAndSave()) return;
        buildDisplayMenu();
    } else if (_subMenu == MENU_AUDIO) {
        if (_editField == 1) {
            int32_t v;
            if (!handheld::settings::parseInteger(value, 0, 100, v)) {
                showToast("Volume: 0-100%"); return;
            }
            s.audioVolume = (uint8_t)v;
        }
        if (!applyAndSave()) return;
        buildAudioMenu();
    }

    _editing = false;
    _editField = -1;
    } catch (const std::bad_alloc&) { showToast("Settings memory unavailable; retry", 2500); }
}

// Get current value of a field as string for editing
std::string SettingsScreen::getCurrentValue(SubMenu menu, int field) {
    if (!_config) return "";
    auto& s = _candidate.settings();
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
    } else if (menu == MENU_DISPLAY) {
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
    pollPropagationUI();
    if (!_candidateReady) {
        Theme::useSmallFont(canvas); canvas.setTextColor(Theme::MUTED);
        canvas.drawString("Settings memory unavailable", 4, Theme::CONTENT_Y + 8);
        canvas.drawString("Enter=retry", 4, Theme::CONTENT_Y + 22);
        return;
    }
    if (_subMenu == MENU_ABOUT) {
        renderAbout(canvas);
        return;
    }

    int y0 = Theme::CONTENT_Y;

    // Header with accent bar
    const char* headers[] = {"SETTINGS", "RADIO", "WIFI", "TCP CONNECTIONS",
                             "SD CARD", "DISPLAY", "AUDIO", "ABOUT", "WIFI SCAN",
                             "PROPAGATION", "MANUAL NODE", "PROPAGATION NODES", "VOICE"};
    static_assert(sizeof(headers) / sizeof(headers[0]) == MENU_VOICE + 1,
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
    } else {
        _list.render(canvas, 0, y0 + headerH + 2, Theme::CONTENT_W,
                     Theme::CONTENT_H - headerH - 3, true,
                     _subMenu == MENU_PROPAGATION || _subMenu == MENU_PROPAGATION_NODES);
        if (_subMenu == MENU_WIFI) _drawnWiFiAction = _wifiAction;
    }

    // Confirmation dialog overlay
    if (_confirmPending) {
        const char* prompt = _confirmAction == 0 ?
            "Factory Reset? Y/N" : "Wipe SD Data? Y/N";
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

void SettingsScreen::renderAbout(M5Canvas& canvas) {
    int y0 = Theme::CONTENT_Y;
    const int headerH = Theme::SECTION_HEADER_H;
    canvas.fillRect(0, y0, Theme::CONTENT_W, headerH, Theme::BG_SURFACE);
    canvas.fillRect(0, y0 + 2, 3, headerH - 4, Theme::ACCENT);
    Theme::useUiFont(canvas);
    canvas.setTextColor(Theme::ACCENT);
    canvas.drawString("About", 8, y0 + 2);
    canvas.drawFastHLine(0, y0 + headerH, Theme::CONTENT_W, Theme::DIVIDER);

    Theme::useSmallFont(canvas);
    int y = y0 + headerH + 5;
    canvas.setTextColor(Theme::PRIMARY);
    canvas.drawString("rsCardputer v" RSCARDPUTER_VERSION_STRING, 4, y); y += 10;

    canvas.setTextColor(Theme::SECONDARY);
    canvas.drawString("M5Stack Cardputer Adv", 4, y); y += 10;
    canvas.drawString("Cap LoRa-1262 (SX1262)", 4, y); y += 10;

    canvas.setTextColor(Theme::MUTED);
    canvas.drawString("ratspeak.org", 4, y); y += 12;

    canvas.setTextColor(Theme::SECONDARY);
    String idLine = "ID: " + _identityHash;
    canvas.drawString(idLine.c_str(), 4, y); y += 10;

    char heap[32];
    snprintf(heap, sizeof(heap), "Heap: %lu bytes", (unsigned long)ESP.getFreeHeap());
    canvas.drawString(heap, 4, y); y += 10;

    char uptime[32];
    snprintf(uptime, sizeof(uptime), "Up: %lus", millis() / 1000);
    canvas.drawString(uptime, 4, y);
}

bool SettingsScreen::handleKey(const KeyEvent& event) {
    if (!_candidateReady) {
        if (event.enter) onEnter();
        else if ((event.escape || event.backspace) && _backCb) _backCb();
        return true;
    }
    if (_config && (_config->settingsPending() || _radioApplyPending)) { showToast("Settings apply pending"); return true; }
    if (event.repeat && (event.forwardDelete || (event.backspace && !_editing))) return true;
    // Handle confirmation dialog
    if (_confirmPending) {
        if (event.character == 'y' || event.character == 'Y') {
            _confirmPending = false;
            if (_confirmAction == 0) {
                factoryReset();
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
            if (!_config || !_candidate.tryAssign(*_config)) {
                showToast("Settings memory unavailable; retry", 2500); return true;
            }
            _candidateDirty = false;
            _editing = false;
            _editField = -1;
            _tcpPendingHost.clear();
            _editLabel.clear();
            return true;
        }
        if (_subMenu == MENU_PROPAGATION_NODES) { showPropagationChoice(); return true; }
        if (_subMenu == MENU_PROPAGATION_CHOICE) {
            _subMenu = MENU_PROPAGATION; buildPropagationMenu(); return true;
        }
        if (_subMenu == MENU_TCP) {
            _subMenu = MENU_WIFI;
            buildWiFiMenu();
            return true;
        }
        if (_subMenu == MENU_WIFI_SCAN) {
            _subMenu = MENU_WIFI;
            buildWiFiMenu();
            return true;
        }
        if (_subMenu != MENU_MAIN) {
            _subMenu = MENU_MAIN;
            buildMainMenu();
            return true;
        }
        return false;
    }

    if (_editing) {
        if (_editInput.handleKey(event)) return true;
        return false;
    }

    // The printed Fn+Backspace Delete action removes a TCP connection.
    if (event.forwardDelete && _subMenu == MENU_TCP) {
        int sel = _list.getSelectedIndex();
        int tcpIdx = sel - 1;  // item 0 is "Add", items 1..N are connections
        if (tcpIdx >= 0 && tcpIdx < (int)_candidate.settings().tcpConnections.size()) {
            removeTCPConnection(tcpIdx);
            return true;
        }
    }

    if (event.navUp()) {
        _list.scrollUp();
        return true;
    }
    if (event.navDown()) {
        _list.scrollDown();
        return true;
    }

    // Enter — select item
    if (event.enter) {
        int sel = _list.getSelectedIndex();

        if (_subMenu == MENU_MAIN) {
            switch (sel) {
                case 0: _subMenu = MENU_RADIO; buildRadioMenu(); break;
                case 1: _subMenu = MENU_WIFI; buildWiFiMenu(); break;
                case 2: _subMenu = MENU_SDCARD; buildSDCardMenu(); break;
                case 3: _subMenu = MENU_DISPLAY; buildDisplayMenu(); break;
                case 4: _subMenu = MENU_AUDIO; buildAudioMenu(); break;
                case 5: _subMenu = MENU_ABOUT; break;
                case 6: _confirmPending = true; _confirmAction = 0; break;
                case 7: _subMenu = MENU_PROPAGATION; buildPropagationMenu(); break;
                case 8: _subMenu = MENU_VOICE; buildVoiceMenu(); break;
            }
            return true;
        }

        if (_subMenu == MENU_PROPAGATION || _subMenu == MENU_PROPAGATION_CHOICE ||
            _subMenu == MENU_PROPAGATION_NODES) { activatePropagationRow(sel); return true; }

        if(_subMenu==MENU_VOICE) {activateVoiceRow(sel);return true;}

        // WiFi scan results handling
        if (_subMenu == MENU_WIFI_SCAN) {
            int lastItem = _list.itemCount() - 1;
            int rescanItem = lastItem - 1;

            if (sel == lastItem) {
                // "< Back"
                _subMenu = MENU_WIFI;
                buildWiFiMenu();
            } else if (sel == rescanItem) {
                // "[Rescan]"
                startWiFiScan();
            } else if (sel < (int)_scanResults.size()) {
                selectNetwork(sel);
            }
            return true;
        }

        // "Back" is always last item
        if (sel == _list.itemCount() - 1) {
            if (_subMenu == MENU_TCP) {
                _subMenu = MENU_WIFI;
                buildWiFiMenu();
            } else {
                _subMenu = MENU_MAIN;
                buildMainMenu();
            }
            return true;
        }

        // Handle radio presets (items 1..RadioPresets::count; item 0 is the "Active:" label)
        if (_subMenu == MENU_RADIO && sel >= 1 && sel <= RadioPresets::count) {
            applyRadioPreset(sel - 1);
            return true;
        }
        // Item 0 ("Active: ...") is non-interactive
        if (_subMenu == MENU_RADIO && sel == 0) {
            return true;
        }

        // Toggle audio on/off (item 0 in Audio menu)
        if (_subMenu == MENU_AUDIO && sel == 0) {
            auto& s = _candidate.settings();
            s.audioEnabled = !s.audioEnabled;
            applyAndSave();
            buildAudioMenu();
            return true;
        }

        // Cycle WiFi mode (item 0 in WiFi menu)
        if (_subMenu == MENU_WIFI && sel == 0) {
            auto& s = _candidate.settings();
            s.wifiMode = (RatWiFiMode)(((int)s.wifiMode + 1) % 3);
            if (applyAndSave()) showToast("Reboot to apply");
            buildWiFiMenu();
            return true;
        }

        // STA mode WiFi menu actions
        if (_subMenu == MENU_WIFI && _candidate.settings().wifiMode == RAT_WIFI_STA) {
            if (sel == 1) {
                // Status line (non-interactive)
                return true;
            }
            if (sel == 2) {
                if (!event.repeat) activateWiFiAction();
                return true;
            }
            if (sel == 3) {
                // [Scan Networks]
                startWiFiScan();
                return true;
            }
            if (sel == 4) {
                // > TCP Connections
                _subMenu = MENU_TCP;
                buildTCPMenu();
                return true;
            }
            if (sel == 5) {
                // Toggle Auto-discover LAN (AutoInterface).  IPv6 enable
                // is one-shot per WiFi init, so changes take effect on
                // next reboot.
                auto& s = _candidate.settings();
                s.autoIfaceEnabled = !s.autoIfaceEnabled;
                if (applyAndSave()) showToast("Reboot to apply");
                buildWiFiMenu();
                return true;
            }
        }

        // SD Card menu actions
        if (_subMenu == MENU_SDCARD) {
            if (_sdStore && _sdStore->isReady()) {
                // Item 4 = Initialize, Item 5 = Wipe All Data
                if (sel == 4) {
                    sdCardFormat();
                    return true;
                }
                if (sel == 5) {
                    _confirmPending = true;
                    _confirmAction = 1;
                    return true;
                }
            }
            // Info items are non-interactive
            return true;
        }

        // TCP submenu actions
        if (_subMenu == MENU_TCP) {
            if (sel == 0) {
                // Add new connection — open host input first
                _editField = 99;
                _editing = true;
                _editLabel = "Host:";
                _editInput.clear();
                _editInput.setActive(true);
                _editInput.setMaxLength(64);
                _editInput.setNumericOnly(false);
                _editInput.setSubmitCallback([this](const std::string& value) {
                    commitEdit(value);
                });
                return true;
            }
            int tcpIdx = sel - 1;
            if (tcpIdx >= 0 && tcpIdx < (int)_candidate.settings().tcpConnections.size()) {
                toggleTCPConnection(tcpIdx);
                return true;
            }
            return true;  // Back handled above
        }

        // Edit the selected field (offset by 1+RadioPresets::count for radio header+presets, 1 for WiFi mode)
        int fieldIdx = sel;
        if (_subMenu == MENU_RADIO) fieldIdx -= (1 + RadioPresets::count);
        if (_subMenu == MENU_WIFI) fieldIdx -= 1;
        std::string currentVal = getCurrentValue(_subMenu, fieldIdx);
        startEditing(fieldIdx, currentVal);
        return true;
    }

    return false;
}

void SettingsScreen::showToast(const char* msg, unsigned long durationMs) {
    _toastMessage = msg;
    _toastUntil = millis() + durationMs;
}

bool SettingsScreen::applyAndSave() {
    if (!_config || !_flash || !_candidateReady) { showToast("Settings unavailable"); return false; }
    if (_radioApplyPending) { showToast("Waiting for radio", 2500); return false; }
    if (_config->settingsPending()) { showToast("Settings recovery pending", 2500); return false; }
    if (!_saveCb) { showToast("Settings unavailable"); return false; }
    _candidateDirty = true;
    const auto saved = _saveCb(_candidate);
    if (!saved.complete()) {
        showToast(saved.detail ? saved.detail : "Save failed; retry changes", 2500);
        return false;
    }
    _candidateDirty = false;
    applyCommitted(false); // This candidate already owns the committed values.
    return !_radioApplyPending;
}

void SettingsScreen::applyCommitted(bool refreshCandidate) {
    if (!_config || _config->settingsPending()) return;
    if (refreshCandidate) {
        _candidateReady = _candidate.tryAssign(*_config);
        if (_candidateReady) _candidateDirty = false;
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
    if (!_candidateReady) showToast("Saved; settings view unavailable", 2500);
}

void SettingsScreen::finishRadioApply(RadioApply result) {
    _radioApplyPending = result == RadioApply::Pending;
    if (_subMenu == MENU_RADIO) {
        const int selected = _list.getSelectedIndex();
        buildRadioMenu(); _list.setSelected(selected);
    }
    if (_radioApplyPending) { showToast("Saved; waiting for radio", 2500); return; }
    const bool announce = _presetAnnouncePending && result == RadioApply::Applied;
    _presetAnnouncePending = false; // Retire before any backend callback.
    if (result == RadioApply::RebootRequired) { showToast("Saved; radio needs reboot", 2500); return; }
    if (result == RadioApply::Unavailable) { showToast("Saved; radio unavailable", 2500); return; }
    if (!announce) { showToast(_config->mirrorPending() ? "Saved; backup pending" : "Saved!"); return; }
    if (!_backend || !_backend->protocolReady()) { showToast("Preset applied (no announce)"); return; }
    const auto appData = encodeAnnounceName(_config->settings().displayName);
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
    _presetAnnouncePending = true;
    RadioPresets::apply(_candidate.settings(), preset);
    const bool saved = applyAndSave();
    if (!saved && !_config->settingsPending() && !_radioApplyPending) _presetAnnouncePending = false;
    buildRadioMenu();
}

void SettingsScreen::factoryReset() {
    requestMaintenance(handheld::Operation::FactoryReset);
}

void SettingsScreen::requestMaintenance(handheld::Operation operation) {
    _maintenanceReason="Maintenance unavailable";
    if (!_maintenanceCb || !_maintenanceCb(operation)) showToast(_maintenanceReason);
}
