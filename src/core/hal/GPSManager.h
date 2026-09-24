#pragma once

#include <Arduino.h>
#include "config/Config.h"

#if HAS_GPS

#include <HardwareSerial.h>
#include <Preferences.h>
#include "hal/NMEAParser.h"
#include "config/BoardConfig.h"

class GPSManager {
public:
    void setPowerControl(bool (*control)(bool)) { _powerControl = control; }
    void begin();
    void loop();       // Non-blocking: reads available UART bytes, feeds parser
    void stop();       // Disable UART, clear state

    // Time
    bool hasTimeFix() const { return _timeValid; }

    // Location
    bool hasLocationFix() const { return _locationValid; }
    double latitude() const { return _parser.data().latitude; }
    double longitude() const { return _parser.data().longitude; }
    double altitude() const { return _parser.data().altitude; }
    int satellites() const { return _parser.data().satellites; }
    double hdop() const { return _parser.data().hdop; }
    uint8_t fixQuality() const { return _parser.data().fixQuality; }

    // Diagnostics
    uint32_t charsProcessed() const { return _parser.charsProcessed(); }
    uint32_t sentencesParsed() const { return _parser.sentencesParsed(); }
    uint32_t fixAgeMs() const;
    uint32_t timeSyncCount() const { return _timeSyncCount; }
    bool isRunning() const { return _running; }

    // Configuration (set from outside before or after begin())
    void setPosixTZ(const char* tz);
    void setTimeEnabled(bool enable) {
        if (enable && !_timeEnabled) _lastTimeSyncMs = millis() - TIME_SYNC_INTERVAL_MS;
        _timeEnabled = enable;
        if (!enable) _timeValid = false;
    }
    void setLocationEnabled(bool enable) {
        _parser.setParseLocation(enable);
        if (!enable) { _locationValid = false; _parser.data().locationValid = false; }
    }

private:
    bool syncSystemTime();
    void restoreTimeFromNVS();
    void persistToNVS();
    bool tryBaudRate(uint32_t baud);

    bool (*_powerControl)(bool) = nullptr;
    NMEAParser _parser;
    HardwareSerial _serial{2};   // UART2 on ESP32-S3
    bool _running = false;
    bool _timeEnabled = true;
    bool _timeValid = false;
    bool _locationValid = false;
    unsigned long _lastTimeSyncMs = 0;
    unsigned long _lastFixMs = 0;
    unsigned long _lastPersistMs = 0;
    uint32_t _timeSyncCount = 0;
    char _posixTZ[48] = "EST5EDT,M3.2.0,M11.1.0";  // POSIX TZ string
    uint32_t _detectedBaud = 0;

    // Baud auto-detect state
    bool _baudDetected = false;
    // A fix at millis()==0 is valid; pack this flag in existing padding.
    bool _hadLocationFix = false;
    int _baudAttemptIdx = 0;
    unsigned long _baudAttemptStart = 0;
    static constexpr uint32_t BAUD_RATES[] = {38400, 115200, 9600};
    static constexpr int BAUD_RATE_COUNT = 3;
    static constexpr unsigned long BAUD_DETECT_TIMEOUT_MS = 3000;

    static constexpr unsigned long TIME_SYNC_INTERVAL_MS = 60000;   // Re-sync every 60s
    static constexpr unsigned long PERSIST_INTERVAL_MS = 300000;     // Persist to NVS every 5 min
    static constexpr unsigned long FIX_TIMEOUT_MS = 10000;           // Fix stale after 10s

    static constexpr const char* NVS_NAMESPACE = "gps";
};

#endif // HAS_GPS
