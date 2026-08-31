#pragma once

#if !defined(RSCARDPUTER)
#include "config/UserConfig.h"
#include <WiFi.h>
#include <atomic>

namespace handheld {
// Selected-profile STA reconnect and user scans share one owner. SDK event
// callbacks only set bits; they never disconnect, render, or call the backend.
class WiFiConnection {
public:
    void begin(const UserSettings& settings);
    void poll();
    void startScan();
    bool finishScan(String& json);
    void stop();
    bool scanning() const { return _scanning || _scanRequested; }
private:
    enum class State { Disabled, Waiting, Connecting, Connected };
    void retry();
    State _state = State::Disabled;
    String _ssid, _password;
    uint32_t _deadline = 0, _scanDeadline = 0;
    uint8_t _attempt = 0;
    bool _scanRequested = false, _scanning = false, _scanDone = false;
    wifi_mode_t _scanPreviousMode = WIFI_OFF;
    std::atomic<bool> _disconnected{false};
    String _scanResult;
};
} // namespace handheld
#endif
