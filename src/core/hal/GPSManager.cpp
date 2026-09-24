#include "GPSManager.h"
#include "runtime/TaskOwner.h"
#include "hal/ClockConfidence.h"

#if HAS_GPS

#include <sys/time.h>
#include <time.h>

constexpr uint32_t GPSManager::BAUD_RATES[];

void GPSManager::begin() {
    handheld::assertDeviceOwner();
    if (_running) return;
    if (_powerControl && !_powerControl(true)) {
        Serial.println("[GPS] Board revision unknown; GNSS left disabled");
        return;
    }

    // Restore last-known time from NVS on boot
    if (_timeEnabled) restoreTimeFromNVS();

    // Previous-session sentences must not lock a newly powered receiver to
    // the first candidate rate. Preserve the user's location opt-in only.
    const bool parseLocation = _parser.parseLocation();
    _parser = NMEAParser{};
    _parser.setParseLocation(parseLocation);
    _locationValid = false;
    _hadLocationFix = false;

    // Prefer the board's factory rate, then keep cycling until valid NMEA.
    _baudDetected = false;
    _baudAttemptIdx = 0;
    for (int i = 0; i < BAUD_RATE_COUNT; ++i) {
        if (BAUD_RATES[i] == GPS_BAUD) { _baudAttemptIdx = i; break; }
    }
    _baudAttemptStart = millis();
    _serial.begin(BAUD_RATES[_baudAttemptIdx], SERIAL_8N1, GPS_RX, GPS_TX);
    Serial.printf("[GPS] UART started, trying %lu baud (GPIO RX=%d TX=%d)\n",
                  (unsigned long)BAUD_RATES[_baudAttemptIdx], GPS_RX, GPS_TX);
    _running = true;
}

void GPSManager::loop() {
    handheld::assertDeviceOwner();
    if (!_running) return;

    // Read available bytes (up to 64 per call to stay non-blocking)
    int count = 0;
    while (_serial.available() && count < 64) {
        char c = _serial.read();
        _parser.feed(c);
        count++;
    }

    // Baud rate auto-detection: if no valid sentence after timeout, try next rate
    if (!_baudDetected) {
        if (_parser.sentencesParsed() > 0) {
            _baudDetected = true;
            Serial.printf("[GPS] Baud detected: %lu (%lu sentences parsed)\n",
                          (unsigned long)baudRate(), (unsigned long)_parser.sentencesParsed());
        } else if (uint32_t(millis() - _baudAttemptStart) >= BAUD_DETECT_TIMEOUT_MS) {
            // A quiet receiver may still be powering up. Exhausting one pass
            // is not evidence of its baud rate and must not disable discovery.
            _baudAttemptIdx = (_baudAttemptIdx + 1) % BAUD_RATE_COUNT;
            _serial.end();
            _serial.begin(BAUD_RATES[_baudAttemptIdx], SERIAL_8N1, GPS_RX, GPS_TX);
            _baudAttemptStart = millis();
        }
    }

    // Check for new time data
    NMEAData& d = _parser.data();
    if (d.timeUpdated) {
        d.timeUpdated = false;
        const bool allowSync = d.timeFromFix ||
            (_timeSyncCount == 0 && handheld::ClockConfidence::canSeedApproximate());
        const bool firstFix = d.timeFromFix && !handheld::ClockConfidence::synchronized();
        if (_timeEnabled && d.timeValid && allowSync &&
            (millis() - _lastTimeSyncMs >= TIME_SYNC_INTERVAL_MS || _timeSyncCount == 0 || firstFix) &&
            syncSystemTime()) {
            // Persist time to NVS so reboots without WiFi/GPS have approximate time
            persistToNVS();
            _lastPersistMs = millis();
        }
    }

    // Check for new location data
    if (d.locationUpdated) {
        d.locationUpdated = false;
        _locationValid = d.locationValid;
        if (_locationValid) {
            _lastFixMs = millis();
            _hadLocationFix = true;
        }
    }

    // Stale fix detection
    if (_locationValid && (millis() - _lastFixMs >= FIX_TIMEOUT_MS)) {
        // Don't clear _timeValid — system clock is still set and accurate.
        // Only clear location validity since position may have changed.
        _locationValid = false;
    }
}

void GPSManager::printDiagnostics() const {
    handheld::assertDeviceOwner();
    Serial.printf("GPS: %s baud=%lu %s bytes=%lu sentences=%lu sats=%d quality=%u time-fix=%s syncs=%lu\n",
                  _running ? "RUNNING" : "STOPPED", (unsigned long)baudRate(),
                  _baudDetected ? "detected" : "searching",
                  (unsigned long)charsProcessed(), (unsigned long)sentencesParsed(),
                  satellites(), unsigned(fixQuality()), hasTimeFix() ? "YES" : "no",
                  (unsigned long)timeSyncCount());
}

void GPSManager::stop() {
    handheld::assertDeviceOwner();
    if (!_running) return;

    // Persist current data before stopping
    if (_timeEnabled && (_timeValid || _locationValid)) {
        persistToNVS();
    }

    _serial.end();
    if (_powerControl) _powerControl(false);
    _running = false;
    _baudDetected = false;
    _locationValid = false;
    Serial.println("[GPS] Stopped");
}

uint32_t GPSManager::fixAgeMs() const {
    if (!_hadLocationFix) return UINT32_MAX;
    return millis() - _lastFixMs;
}

bool GPSManager::syncSystemTime() {
    const NMEAData& d = _parser.data();
    if (!d.timeValid || (!d.timeFromFix && !handheld::ClockConfidence::canSeedApproximate())) return false;

    // Sanity check year range (reject spoofed/garbage data)
    if (d.year < 2024 || d.year > 2030) {
        Serial.printf("[GPS] Rejected time: year %d out of range\n", d.year);
        return false;
    }

    // Convert GPS UTC date/time to Unix epoch using arithmetic
    // (avoids mktime() TZ contamination and timegm() unavailability on ESP32)
    auto daysFromCivil = [](int y, int m, int d) -> int32_t {
        // Converts civil date to days since 1970-01-01 (Howard Hinnant algorithm)
        y -= (m <= 2);
        int era = (y >= 0 ? y : y - 399) / 400;
        unsigned yoe = (unsigned)(y - era * 400);
        unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + (int32_t)doe - 719468;
    };

    int32_t days = daysFromCivil(d.year, d.month, d.day);
    time_t epoch = (time_t)days * 86400 + d.hour * 3600 + d.minute * 60 + d.second;
    if (epoch < 1700000000) {
        Serial.printf("[GPS] Rejected time: epoch %ld too low\n", (long)epoch);
        return false;
    }

    struct timeval tv = {};
    tv.tv_sec = epoch;
    tv.tv_usec = 0;
    if (settimeofday(&tv, nullptr) != 0) return false;

    _timeValid = d.timeFromFix;
    if (d.timeFromFix) handheld::ClockConfidence::markSynchronized(epoch);
    _lastTimeSyncMs = millis();
    _timeSyncCount++;

    // Set TZ so localtime() returns correct local time (with DST if applicable)
    setenv("TZ", _posixTZ, 1);
    tzset();

    Serial.printf("[GPS] System time %s: %04d-%02d-%02d %02d:%02d:%02d UTC (sync #%lu, sats=%d)\n",
                  d.timeFromFix ? "synced" : "approximate",
                  d.year, d.month, d.day, d.hour, d.minute, d.second,
                  (unsigned long)_timeSyncCount, (int)d.satellites);
    return true;
}

void GPSManager::restoreTimeFromNVS() {
    if (!handheld::ClockConfidence::canSeedApproximate()) return;
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, true)) return;  // read-only

    int64_t storedEpoch = prefs.getLong64("epoch", 0);
    prefs.end();

    if (storedEpoch > 1700000000) {
        struct timeval tv = {};
        tv.tv_sec = (time_t)storedEpoch;
        tv.tv_usec = 0;
        if (settimeofday(&tv, nullptr) != 0) return;

        // Set TZ
        setenv("TZ", _posixTZ, 1);
        tzset();

        time_t now = time(nullptr);
        struct tm converted;
        struct tm* local = localtime_r(&now, &converted);
        if (local) {
            Serial.printf("[GPS] Restored time from NVS: %04d-%02d-%02d %02d:%02d (stale, approximate)\n",
                          local->tm_year + 1900, local->tm_mon + 1, local->tm_mday,
                          local->tm_hour, local->tm_min);
        }
        // Note: _timeValid stays false — this is approximate/stale time.
        // The system clock is set so timestamps work, but the GPS indicator
        // won't show as "fixed" until a real satellite fix arrives.
    }
}

void GPSManager::persistToNVS() {
    time_t now = time(nullptr);
    if (now < 1700000000) return;  // Don't persist if we don't have real time

    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, false)) return;  // read-write

    prefs.putLong64("epoch", (int64_t)now);

    const NMEAData& d = _parser.data();
    if (_parser.parseLocation() && d.locationValid) {
        // Store as integers (microdegrees) to avoid float NVS issues
        prefs.putLong("lat_ud", (int32_t)(d.latitude * 1000000.0));
        prefs.putLong("lon_ud", (int32_t)(d.longitude * 1000000.0));
        prefs.putLong("alt_cm", (int32_t)(d.altitude * 100.0));
    }

    prefs.end();
}

void GPSManager::setPosixTZ(const char* tz) {
    strncpy(_posixTZ, tz, sizeof(_posixTZ) - 1);
    _posixTZ[sizeof(_posixTZ) - 1] = '\0';
}

#endif // HAS_GPS
