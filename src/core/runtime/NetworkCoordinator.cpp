#include "NetworkCoordinator.h"
#include "config/NetworkTiming.h"
#include "config/NetworkSettings.h"
#include "protocol/RustInterfacePump.h"
#include "transport/TcpClientSet.h"
#include "transport/RnsAutoInterface.h"
#include "transport/WiFiInterface.h"
#include <esp_netif.h>
#include <new>
#include <algorithm>
#include <utility>

namespace handheld {
namespace {
bool linkLocal(const IPv6Address& address) {
    return address[0] == 0xfe && (address[1] & 0xc0) == 0x80;
}
uint32_t staScope() {
    auto* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    return sta ? esp_netif_get_netif_impl_index(sta) : 1;
}
}
void NetworkCoordinator::deferAuto(uint32_t now) {
    WiFi.enableIpV6();
    _deferred = true; _deferredAt = _autoCheckedAt = now;
    _autoCheckMs = network_timing::AutoInitialMs;
    _autoRetryMs = network_timing::AutoRetryMs;
    _autoNoticeSent = false;
}
void NetworkCoordinator::retireTransports() {
    // Detach before driver stop/delete: late owned frames cannot use old keys.
    _tcp.stop();
    _pump.attachAuto(nullptr); _auto.stop();
    _pump.attachWifiAp(nullptr);
    if (_ap) { delete _ap; _ap = nullptr; }
    _connected = false; _deferred = false; _reload = false;
    _apApply = false; _apResult = HotspotResult::None;
}
bool NetworkCoordinator::begin(const UserSettings& settings, RatWiFiMode mode) {
    if (_closed) return false;
    WiFiConnection::initializeEvents();
    retireTransports();
    _wifi.stop(); _helpersStopped = false;
    WiFi.disconnect(false);
    if (mode == RAT_WIFI_STA) return _wifi.begin(settings);
    if (mode == RAT_WIFI_OFF) return WiFi.mode(WIFI_OFF);
    if (mode != RAT_WIFI_AP) return false;
    _ap = new (std::nothrow) WiFiInterface();
    if (!_ap) { WiFi.mode(WIFI_OFF); return false; }
    // An empty SSID still selects the generated name; its saved password applies.
    if (!_ap->setAPCredentials(settings.wifiAPSSID.c_str(), settings.wifiAPPassword.c_str()) ||
        !_ap->start()) { delete _ap; _ap = nullptr; WiFi.mode(WIFI_OFF); return false; }
    _pump.attachWifiAp(_ap);
    return true;
}
void NetworkCoordinator::disconnect() {
    if (_closed) return;
    retireTransports(); _wifi.stop(); WiFi.disconnect(false); WiFi.mode(WIFI_OFF);
}
void NetworkCoordinator::settingsCommitted(const UserSettings& settings, uint8_t changes) {
    if (_closed) return;
    if (changes & network_settings::Tcp) requestTcpReload();
    if (settings.wifiMode != RAT_WIFI_AP) _apApply = false;
    else if (_ap && (changes & network_settings::AccessPoint)) _apApply = true;
}
uint8_t NetworkCoordinator::poll(const UserSettings& settings, unsigned long rnsDuration, Budget budget,
                                 HotspotAdmission hotspotAdmission) {
    if (_closed) { pollSettlements(); return None; }
    uint8_t events = None;
    const bool explicitReload = _reload;
    const auto transition = _wifi.poll();
    const bool connectedNow = _wifi.connected();
    const bool reconnected = transition == WiFiConnection::Transition::Reconnected;
    if (_connected && (!connectedNow || reconnected)) {
        _tcp.stop(); _pump.attachAuto(nullptr); _auto.stop();
        _connected = false; _deferred = false; events |= Disconnected;
    }
    if (connectedNow && !_connected) {
        _connected = true; events |= Connected;
        _reload = true;
    }
    const uint32_t now = uint32_t(millis());
    if (!settings.autoIfaceEnabled && (_deferred || _auto.isOnline())) {
        _pump.attachAuto(nullptr); _auto.stop(); _deferred = false;
    }
    if (_connected && settings.autoIfaceEnabled && !_deferred && !_auto.isOnline())
        deferAuto(now);
    if (_deferred && now - _autoCheckedAt >= _autoCheckMs) {
        _autoCheckedAt = now;
        const IPv6Address address = WiFi.localIPv6();
        if (linkLocal(address)) {
            if (_auto.start(settings.autoIfaceGroupId.c_str(), settings.autoIfaceMaxPeers,
                            (const uint8_t*)address, staScope())) {
                _deferred = false; _lastLinkCheck = now;
                _pump.attachAuto(&_auto); events |= AutoStarted;
            } else {
                _auto.stop(); events |= AutoFailed;
                _autoCheckMs = _autoRetryMs;
                _autoRetryMs = std::min(_autoRetryMs * 2, network_timing::AutoRetryMaxMs);
            }
        } else {
            _autoCheckMs = network_timing::AutoPollMs;
            if (!_autoNoticeSent && now - _deferredAt >= network_timing::AutoNoticeMs) {
                _autoNoticeSent = true; events |= AutoTimeout;
            }
        }
    }
    if (_auto.isOnline() && _connected && now - _lastLinkCheck >= network_timing::AutoPollMs) {
        _lastLinkCheck = now;
        const IPv6Address address = WiFi.localIPv6();
        if (linkLocal(address)) _auto.notifyLinkChange((const uint8_t*)address, staScope());
        else {
            _pump.attachAuto(nullptr); _auto.stop(); deferAuto(now);
        }
    }
    if (_reload && _connected) {
        _reload = false;
        if (_tcp.reload(_pump, settings.tcpConnections)) {
            if (explicitReload) events |= TcpReloaded;
        }
        else Serial.println("[TCP] Invalid endpoint settings; existing owner retained");
    }
    // Only this new blocking step needs fresh radio/audio admission. Scanner
    // settlement and retired TCP helper progress above/below must keep running.
    if (settings.wifiMode != RAT_WIFI_AP) _apApply = false;
    if (_apApply && _ap && !_wifi.scanning() && hotspotAdmission && hotspotAdmission()) {
        _apApply = false;
        WiFiInterface::APCredentials prepared;
        if (!WiFiInterface::prepareAPCredentials(settings.wifiAPSSID.c_str(),
                                                 settings.wifiAPPassword.c_str(), prepared)) {
            _apResult = HotspotResult::NeedsRestart; // Existing service is intact.
        } else {
            _pump.attachWifiAp(nullptr);
            _ap->stop();
            _ap->setAPCredentials(std::move(prepared));
            if (_ap->start()) {
                _pump.attachWifiAp(_ap);
                _apResult = HotspotResult::Applied;
            } else _apResult = HotspotResult::Unavailable;
        }
        events |= HotspotChanged;
    }
    _tcp.maintain();
    if (rnsDuration > budget.overloadMs) events |= TcpSkipped;
    else {
        if (_ap) _ap->loop();
        _tcp.poll(budget.tcpMs);
    }
    // AutoInterface already bounds socket work; TCP pressure must not starve it.
    _auto.loop();
    return events;
}
void NetworkCoordinator::closeAdmissions() {
    _closed = true; _deferred = false; _reload = false;
    _apApply = false; _apResult = HotspotResult::None;
    _wifi.closeAdmissions();
}
void NetworkCoordinator::pollSettlements() {
    _wifi.pollSettlements();
    // maintain() can construct a pending replacement before stop() cancels it.
    if (_helpersStopped) _tcp.maintain();
}
void NetworkCoordinator::stopHelpers() {
    if (_helpersStopped) return;
    closeAdmissions(); retireTransports(); _wifi.stop();
    WiFi.disconnect(false); WiFi.mode(WIFI_OFF); _helpersStopped = true;
}
bool NetworkCoordinator::quiescent() const {
    return _helpersStopped && !_wifi.scanning() && _tcp.quiescent() && WiFi.getMode() == WIFI_OFF;
}
} // namespace handheld
