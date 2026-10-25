#pragma once
#include "WiFiConnection.h"
#include <stdint.h>
class RustInterfacePump;
class TcpClientSet;
class RnsAutoInterface;
class WiFiInterface;

namespace handheld {
// Protocol-owner coordinator. Drivers retain their own bounded queues and SDK
// helper lifetime; settings are borrowed only during a call, never retained.
// Before destroying borrowed drivers/pump, stopHelpers then poll to quiescent.
class NetworkCoordinator {
public:
    struct Budget { uint16_t tcpMs, overloadMs; };
    static constexpr Budget CardBudget{12, 200}, LargeBudget{35, 500};
    enum Event : uint8_t { None=0, Connected=1, Disconnected=2, AutoStarted=4,
        AutoFailed=8, AutoTimeout=16, TcpSkipped=32, TcpReloaded=64, HotspotChanged=128 };
    enum class HotspotResult : uint8_t { None, Applied, NeedsRestart, Unavailable };
    NetworkCoordinator(WiFiConnection& wifi, RustInterfacePump& pump,
                       TcpClientSet& tcp, RnsAutoInterface& automatic)
        : _wifi(wifi), _pump(pump), _tcp(tcp), _auto(automatic) {}
    NetworkCoordinator(const NetworkCoordinator&) = delete;
    NetworkCoordinator& operator=(const NetworkCoordinator&) = delete;
    // Fresh selection/reset; false means the selected mode could not start.
    bool begin(const UserSettings& settings) { return begin(settings, settings.wifiMode); }
    // Boot-loop recovery starts in a mode other than the saved one, without saving it.
    bool begin(const UserSettings& settings, RatWiFiMode mode);
    // Executed immediately before the optional AP change, after normal network
    // work. A missing predicate denies live AP changes, never ordinary polling.
    using HotspotAdmission = bool (*)();
    uint8_t poll(const UserSettings& settings, unsigned long rnsDuration, Budget budget,
                 HotspotAdmission hotspotAdmission = nullptr);
    // Notify only after durable commit/recovery, borrowing the canonical values.
    // Saved mode changes remain reboot-required; they may cancel a queued apply.
    void settingsCommitted(const UserSettings& settings, uint8_t changes);
    bool hotspotApplyPending() const { return _apApply; }
    HotspotResult hotspotResult() const { return _apResult; }
    void requestTcpReload() { if (!_closed) _reload = true; }
    void disconnect(); // user action; later begin/scans remain available
    void closeAdmissions(); // maintenance: permanent for this owner incarnation
    void pollSettlements(); // no normal reconnect, socket polling or new helper
    void stopHelpers();
    bool quiescent() const;
    bool connected() const { return _connected && _wifi.connected(); }
    WiFiInterface* accessPoint() const { return _ap; }
    bool autoDeferred() const { return _deferred; }
    uint32_t autoDeferredElapsed() const { return _deferred ? uint32_t(millis()) - _deferredAt : 0; }
private:
    void retireTransports();
    void deferAuto(uint32_t now);
    WiFiConnection& _wifi;
    RustInterfacePump& _pump;
    TcpClientSet& _tcp;
    RnsAutoInterface& _auto;
    WiFiInterface* _ap = nullptr; // only owned dynamic driver, at most one
    uint32_t _deferredAt = 0, _lastLinkCheck = 0;
    uint32_t _autoCheckedAt = 0, _autoCheckMs = 0, _autoRetryMs = 0;
    bool _autoNoticeSent = false;
    bool _connected = false, _deferred = false, _reload = false;
    bool _closed = false, _helpersStopped = false, _apApply = false;
    HotspotResult _apResult = HotspotResult::None;
};
} // namespace handheld
