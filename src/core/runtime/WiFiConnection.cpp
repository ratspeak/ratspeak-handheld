#include "WiFiConnection.h"
#if !defined(RSCARDPUTER)
#include "transport/WiFiInterface.h"
#include <esp_wifi.h>

namespace handheld {
void WiFiConnection::begin(const UserSettings& settings) {
    const size_t selected = settings.wifiSTASelected;
    if (settings.wifiMode != RAT_WIFI_STA || selected >= settings.wifiSTANetworks.size() ||
        settings.wifiSTANetworks[selected].ssid.isEmpty()) return;
    _ssid = settings.wifiSTANetworks[selected].ssid;
    _password = settings.wifiSTANetworks[selected].password;
    WiFi.mode(WIFI_STA);
    if (settings.autoIfaceEnabled) WiFi.enableIpV6();
    WiFi.onEvent([this](WiFiEvent_t event, WiFiEventInfo_t) {
        if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) _disconnected.store(true, std::memory_order_release);
    });
    _state = State::Waiting; _deadline = millis();
}
void WiFiConnection::retry() {
    static constexpr uint32_t backoff[] = {5000, 15000, 60000, 300000};
    _deadline = millis() + backoff[std::min<unsigned>(_attempt, 3)];
    if (_attempt < 3) ++_attempt;
    _state = State::Waiting;
}
void WiFiConnection::poll() {
    const auto now = millis();
    const bool disconnected = _disconnected.exchange(false, std::memory_order_acq_rel);
    if (_scanning) {
        const auto result = WiFi.scanComplete();
        if (result != WIFI_SCAN_RUNNING || static_cast<int32_t>(now - _scanDeadline) >= 0) {
            if (result == WIFI_SCAN_RUNNING) esp_wifi_scan_stop();
            JsonDocument doc; auto rows = doc.to<JsonArray>();
            if (result >= 0) for (const auto& network : WiFiInterface::getScanResults(15)) {
                auto row = rows.add<JsonObject>(); row["ssid"] = network.ssid;
                row["rssi"] = network.rssi; row["encrypted"] = network.encrypted;
            }
            WiFi.scanDelete(); _scanResult = ""; serializeJson(doc, _scanResult);
            if (_scanPreviousMode == WIFI_OFF || _scanPreviousMode == WIFI_AP) WiFi.mode(_scanPreviousMode);
            _scanning = false; _scanDone = true;
        }
        return;
    }
    if (_scanRequested && _state != State::Connecting) {
        _scanRequested = false; _scanning = true; _scanDone = false;
        _scanPreviousMode = WiFi.getMode();
        WiFiInterface::startAsyncScan(); _scanDeadline = now + 15000;
        return;
    }
    if (_state == State::Disabled) return;
    if (WiFi.status() == WL_CONNECTED) { _state = State::Connected; _attempt = 0; return; }
    if (_state == State::Connected || (disconnected && _state == State::Connecting)) {
        retry(); return;
    }
    if (_state == State::Connecting) {
        if (static_cast<int32_t>(now - _deadline) >= 0) { WiFi.disconnect(false); retry(); }
        return;
    }
    if (!_scanRequested && static_cast<int32_t>(now - _deadline) >= 0) {
        WiFi.begin(_ssid.c_str(), _password.c_str());
        _state = State::Connecting; _deadline = now + 8000;
    }
}
void WiFiConnection::startScan() { _scanRequested = true; _scanDone = false; }
bool WiFiConnection::finishScan(String& json) {
    if (!_scanDone) return false;
    json = _scanResult; _scanResult = ""; _scanDone = false; return true;
}
void WiFiConnection::stop() {
    _state = State::Disabled;
    if (_scanning) esp_wifi_scan_stop();
    _scanning = _scanRequested = false;
}
} // namespace handheld
#endif
