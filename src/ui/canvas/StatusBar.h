#pragma once

#include <M5GFX.h>

class StatusBar {
public:
    void render(M5Canvas& canvas);

    void setDirtyFlag(bool* flag) { _dirty = flag; }
    void setTransportMode(const char* mode) { _transportMode = mode; if (_dirty) *_dirty = true; }
    void setLoRaOnline(bool online) { _loraOnline = online; if (_dirty) *_dirty = true; }
    void setTCPConnected(bool connected) { _tcpConnected = connected; if (_dirty) *_dirty = true; }
    void setWiFiState(bool enabled, bool active) {
        if (_wifiEnabled == enabled && _wifiActive == active) return;
        _wifiEnabled = enabled; _wifiActive = active;
        if (_dirty) *_dirty = true;
    }
    void setGPSTimeFix(bool fix) { _gpsTimeFix = fix; if (_dirty) *_dirty = true; }
    // -1 = disabled (hide); 0 = idle; >0 = N peers reachable
    void setAutoIfacePeers(int n) { if (_autoIfacePeers != n) { _autoIfacePeers = n; if (_dirty) *_dirty = true; } }
    void flashAnnounce() { _announceFlashUntil = millis() + 1500; if (_dirty) *_dirty = true; }

private:
    const char* _transportMode = "STANDALONE";
    bool _loraOnline = false;
    bool _tcpConnected = false;
    bool _wifiEnabled = false;
    bool _wifiActive = false;
    bool _gpsTimeFix = false;
    int _autoIfacePeers = -1;
    unsigned long _announceFlashUntil = 0;
    float _smoothedBattery = -1.0f;
    unsigned long _lastBatteryRead = 0;
    bool* _dirty = nullptr;
};
