#include "WiFiConnection.h"
#include "config/NetworkTiming.h"
#include "transport/WiFiInterface.h"
#include <esp_wifi.h>
#include <atomic>

namespace handheld {
namespace {
// Registered once before network startup, never removed. The SDK may retain or
// copy the callback while dispatching; it holds no object or helper lifetime.
std::atomic<uint32_t> disconnectSequence{0};
bool eventsRegistered = false; // boot/protocol owner only
}
void WiFiConnection::initializeEvents() {
    if (eventsRegistered) return;
    // UserConfig owns saved credentials. SDK reconnects must not write another
    // NVS copy outside the storage lease while microphone capture is active.
    WiFi.persistent(false);
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t) {
        if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
            disconnectSequence.fetch_add(1, std::memory_order_release);
    });
    eventsRegistered = true;
}
bool WiFiConnection::begin(const UserSettings& settings) {
    if (_closed) return false;
    initializeEvents();
    const bool replacing = _state != State::Disabled;
    stop();
    _attempt = 0;
    _observedDisconnect = disconnectSequence.load(std::memory_order_acquire);
    _ssid = ""; _password = "";
    const size_t selected = settings.wifiSTASelected;
    if (settings.wifiMode != RAT_WIFI_STA || selected >= settings.wifiSTANetworks.size() ||
        settings.wifiSTANetworks[selected].ssid.isEmpty()) {
        if (replacing) WiFi.disconnect(false);
        return false;
    }
    _ssid = settings.wifiSTANetworks[selected].ssid;
    _password = settings.wifiSTANetworks[selected].password;
    // A fresh selection also resets credentials/backoff when its SSID is equal.
    if (replacing || WiFi.status() == WL_CONNECTED) WiFi.disconnect(false);
    WiFi.setAutoReconnect(false);
    if (!WiFi.mode(WIFI_STA)) return false;
    if (settings.autoIfaceEnabled) WiFi.enableIpV6();
    _state = State::Waiting; _deadline = static_cast<uint32_t>(millis());
    return true;
}
bool WiFiConnection::connected() const {
    return _state != State::Disabled && WiFi.status() == WL_CONNECTED && WiFi.SSID() == _ssid;
}
void WiFiConnection::retry(uint32_t now) {
    static constexpr uint32_t backoff[] = {5000, 15000, 30000, network_timing::ReconnectMaxMs};
    _deadline = now + backoff[std::min<unsigned>(_attempt, 3)];
    if (_attempt < 3) ++_attempt;
    _state = State::Waiting;
}
WiFiConnection::Transition WiFiConnection::poll() {
    if (_closed) { pollSettlements(); return Transition::None; }
    const uint32_t now = static_cast<uint32_t>(millis());
    if (_scanning) { pollActiveScan(); return Transition::None; }
    if (_scanRequested && _state != State::Connecting) {
        _scanRequested = false; _scanning = true;
        _scanPreviousMode = WiFi.getMode();
        _scanStartedAt = now;
        if (!WiFiInterface::startAsyncScan()) {
            _scanResult = "";
            _scanOutcome = ScanResult::Failed;
            releaseScan();
        } else {
            _scanDeadline = static_cast<uint32_t>(millis()) + network_timing::WifiScanMs;
        }
        return Transition::None;
    }
    const auto sequence = disconnectSequence.load(std::memory_order_acquire);
    const bool disconnected = sequence != _observedDisconnect;
    _observedDisconnect = sequence;
    if (_state == State::Disabled) return Transition::None;
    if (connected()) {
        const auto transition = _state != State::Connected ? Transition::Connected :
            disconnected ? Transition::Reconnected : Transition::None;
        _state = State::Connected; _attempt = 0;
        return transition;
    }
    if (_state == State::Connected || (disconnected && _state == State::Connecting)) {
        const bool wasConnected = _state == State::Connected;
        retry(now);
        return wasConnected ? Transition::Disconnected : Transition::None;
    }
    if (_state == State::Connecting) {
        if (static_cast<int32_t>(now - _deadline) >= 0) { WiFi.disconnect(false); retry(now); }
        return Transition::None;
    }
    if (!_scanRequested && static_cast<int32_t>(now - _deadline) >= 0) {
        WiFi.begin(_ssid.c_str(), _password.c_str());
        _state = State::Connecting; _deadline = now + network_timing::WifiConnectMs;
    }
    return Transition::None;
}
void WiFiConnection::releaseScan() {
    WiFi.scanDelete();
    if (_scanPreviousMode == WIFI_OFF || _scanPreviousMode == WIFI_AP) WiFi.mode(_scanPreviousMode);
    _scanning = false;
}
void WiFiConnection::pollActiveScan() {
    const auto result = WiFi.scanComplete();
    if (result == WIFI_SCAN_RUNNING && static_cast<int32_t>(static_cast<uint32_t>(millis()) - _scanDeadline) < 0) return;
    // A terminal SDK error need not stop the driver scan. Retire it before
    // deleting results or allowing a retry, including the owner's deadline.
    if (result < 0) {
        esp_wifi_scan_stop();
        Serial.printf("[WIFI] Scan failed (status=%d elapsed=%lums heap=%lu largest=%lu)\n",
                      (int)result, (unsigned long)(static_cast<uint32_t>(millis()) - _scanStartedAt),
                      (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMaxAllocHeap());
    }
    _scanResult = "";
    _scanOutcome = ScanResult::Failed;
    if (result >= 0) {
        JsonDocument doc; auto rows = doc.to<JsonArray>();
        for (const auto& network : WiFiInterface::getScanResults(15)) {
            auto row = rows.add<JsonObject>(); row["ssid"] = network.ssid;
            row["rssi"] = network.rssi; row["encrypted"] = network.encrypted;
        }
        serializeJson(doc, _scanResult);
        _scanOutcome = ScanResult::Ready;
    }
    releaseScan();
}
void WiFiConnection::pollSettlements() {
    // Finish the scan already owned by a caller without starting RF work or
    // progressing the normal reconnect state machine during maintenance.
    if (_scanning) pollActiveScan();
    else if (_scanRequested) {
        _scanRequested = false;
        _scanResult = "";
        _scanOutcome = ScanResult::Cancelled;
    }
}
bool WiFiConnection::startScan() {
    if (_closed || scanning() || _scanOutcome != ScanResult::Pending) return false;
    initializeEvents();
    _scanRequested = true;
    return true;
}
ScanResult WiFiConnection::finishScan(String& json) {
    const auto outcome = _scanOutcome;
    if (outcome == ScanResult::Pending) return outcome;
    json = _scanResult; _scanResult = ""; _scanOutcome = ScanResult::Pending;
    return outcome;
}
void WiFiConnection::stop() {
    _state = State::Disabled;
    if (scanning()) {
        if (_scanning) { esp_wifi_scan_stop(); releaseScan(); }
        _scanRequested = false;
        _scanResult = "";
        _scanOutcome = ScanResult::Cancelled;
    }
}
} // namespace handheld
