#pragma once

#include "ScanResult.h"
#include "config/UserConfig.h"
#include <WiFi.h>

namespace handheld {
// Selected-profile STA reconnect and user scans share one owner. SDK event
// callback publishes a boot-lifetime sequence; it never refers to this owner.
// All methods except that captureless SDK callback run on the protocol owner.
class WiFiConnection {
public:
    enum class Transition : uint8_t { None, Connected, Disconnected, Reconnected };
    WiFiConnection() = default;
    WiFiConnection(const WiFiConnection&) = delete;
    WiFiConnection& operator=(const WiFiConnection&) = delete;
    // Call on the boot owner before starting WiFi; repeated calls are inert.
    static void initializeEvents();
    bool begin(const UserSettings& settings);
    Transition poll();
    void pollSettlements();
    bool startScan();
    ScanResult finishScan(String& json);
    void stop();
    void closeAdmissions() { _closed = true; }
    bool connected() const;
    // Includes the owner's scheduled retry/backoff, until explicitly stopped.
    bool connecting() const { return !_closed && _state != State::Disabled && !connected(); }
    bool scanning() const { return _scanning || _scanRequested; }
private:
    enum class State { Disabled, Waiting, Connecting, Connected };
    void retry(uint32_t now);
    void pollActiveScan();
    void cancelActiveScan();
    void releaseScan();
    State _state = State::Disabled;
    String _ssid, _password;
    uint32_t _deadline = 0, _scanDeadline = 0, _scanStartedAt = 0;
    uint8_t _attempt = 0;
    bool _scanRequested = false, _scanning = false;
    bool _scanRetiring = false, _scanRestore = true, _scanReset = false;
    uint32_t _scanRetireDeadline = 0, _scanRestoreAt = 0;
    ScanResult _scanOutcome = ScanResult::Pending;
    wifi_mode_t _scanPreviousMode = WIFI_OFF;
    uint32_t _observedDisconnect = 0;
    bool _closed = false;
    String _scanResult;
};
} // namespace handheld
