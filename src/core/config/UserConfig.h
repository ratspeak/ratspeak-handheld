#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
#include <cstring>
#include <new>
#include <utility>
#include "storage/FlashStore.h"
#include "storage/SDStore.h"
#include "config/Config.h"
#include "config/BoardConfig.h"
#include "config/ConfigMemory.h"
#include "config/PropagationSettings.h"
#include "voice/VoiceTypes.h"

enum RatWiFiMode : uint8_t { RAT_WIFI_OFF = 0, RAT_WIFI_AP = 1, RAT_WIFI_STA = 2 };

struct WiFiNetwork {
    String ssid;
    String password;
};

constexpr size_t WIFI_STA_MAX_NETWORKS = 3;

// Retain the persisted percentage field while presenting three useful levels.
inline constexpr int keyboardBacklightChoice(int percent) {
    return percent <= 0 ? 0 : percent <= 5 ? 1 : 2;
}
inline constexpr uint8_t keyboardBacklightPercent(int choice) {
    return choice <= 0 ? 0 : choice == 1 ? 5 : 100;
}
inline constexpr const char* keyboardBacklightLabel(int percent) {
    return percent <= 0 ? "OFF" : percent <= 5 ? "LOW" : "HIGH";
}

constexpr uint8_t BATTERY_DISPLAY_PERCENT = 0;
constexpr uint8_t BATTERY_DISPLAY_BAR = 1;
constexpr uint8_t BATTERY_MODEL_LIPO = 0;
constexpr uint8_t BATTERY_MODEL_LINEAR = 1;
constexpr float BATTERY_CHARGE_THRESHOLD_DEFAULT = 4.0f;
constexpr float BATTERY_FULL_VOLTAGE_DEFAULT = 3.9f;
constexpr float BATTERY_CHARGE_THRESHOLD_MIN = 3.80f;
constexpr float BATTERY_CHARGE_THRESHOLD_MAX = 4.30f;
constexpr float BATTERY_FULL_VOLTAGE_MIN = 3.50f;
constexpr float BATTERY_FULL_VOLTAGE_MAX = 4.20f;

struct TCPEndpoint {
    String host;
    uint16_t port = TCP_DEFAULT_PORT;
    bool autoConnect = true;
};

struct UserSettings {
    // Radio
    uint8_t radioRegion = REGION_AMERICAS;
    uint32_t loraFrequency = LORA_DEFAULT_FREQ;
    uint8_t loraSF = LORA_DEFAULT_SF;
    uint32_t loraBW = LORA_DEFAULT_BW;
    uint8_t loraCR = LORA_DEFAULT_CR;
    int8_t loraTxPower = LORA_DEFAULT_TX_POWER;
    long loraPreamble = LORA_DEFAULT_PREAMBLE;
    bool loraEnabled = true;

    // WiFi
    RatWiFiMode wifiMode = BOARD_DEFAULT_WIFI_MODE;
    RatWiFiMode wifiRestoreMode = RAT_WIFI_STA;
    String wifiAPSSID;
    String wifiAPPassword = WIFI_AP_PASSWORD;
    std::vector<WiFiNetwork> wifiSTANetworks;
    uint8_t wifiSTASelected = 0;

    // AutoInterface (Reticulum LAN auto-discovery via IPv6 multicast).
    // Active only in STA mode; opt-in until proven stable on real APs.
    bool   autoIfaceEnabled  = false;
    String autoIfaceGroupId  = "reticulum";
    uint8_t autoIfaceMaxPeers = BOARD_DEFAULT_AUTOIFACE_MAX_PEERS;

    // TCP outbound connections (STA mode only)
    std::vector<TCPEndpoint> tcpConnections;

    handheld::propagation::Settings propagation;
    handheld::voice::Settings voice;

    // Display
    uint16_t screenDimTimeout = 30;   // seconds
    uint16_t screenOffTimeout = 60;   // seconds
    uint8_t brightness = BOARD_DEFAULT_BRIGHTNESS;  // Percentage 1-100, per-board default
    bool denseFontMode = false;       // Persisted compatibility field; no active renderer control.
    bool themeLight = false;          // false = dark (original palette)

    // Battery
    uint8_t batteryDisplay = BATTERY_DISPLAY_BAR;
    uint8_t batteryModel = BATTERY_MODEL_LIPO;
    float chargeThresholdV = BATTERY_CHARGE_THRESHOLD_DEFAULT;
    float fullBatteryV = BATTERY_FULL_VOLTAGE_DEFAULT;

    // Keyboard
    uint8_t keyboardBrightness = 0;   // Persisted/hardware percentage: OFF 0, LOW 5, HIGH 100.
    bool keyboardAutoOn = false;      // Backlight ON when switching to ACTIVE power state
    bool keyboardAutoOff = false;     // Backlight OFF when switching from ACTIVE power state

    // Trackball
    uint8_t trackballSpeed = 3;       // 1-5 sensitivity

    // Touch
    uint8_t touchSensitivity = 3;     // Legacy 1-5 field; current touch HAL uses fixed calibration.

    // BLE
    bool bleEnabled = false;

    // GPS & Time
    bool gpsTimeEnabled = true;      // GPS time sync (default ON)
    bool gpsLocationEnabled = false; // GPS position tracking (default OFF, user must opt in)
    uint8_t timezoneIdx = 6;         // Index into TIMEZONE_TABLE (default: New York EST/EDT)
    bool timezoneSet = false;        // false = show timezone picker at boot
    bool use24HourTime = false;      // false = 12h (no AM/PM), true = 24h

    // Audio
    bool audioEnabled = true;
    uint8_t audioVolume = 80;  // 0-100

    // Identity
    String displayName;
    bool nameComplete = false; // Mirrored from the bound identity slot; empty is a choice.

    // Storage. Removable SD stores plaintext unless explicitly enabled; boards
    // whose legacy installs always used SD default it on (BOARD_DEFAULT_SD_STORAGE).
    bool sdStorageEnabled = BOARD_DEFAULT_SD_STORAGE;

    // Announce
    uint16_t announceInterval = 30; // 0 = OFF; otherwise minutes, 30-360

    // Developer mode — unlocks custom radio parameters
    bool devMode = false;
};

class UserConfig {
public:
    UserConfig() = default;
    UserConfig(const UserConfig&) = delete;
    UserConfig& operator=(const UserConfig&) = delete;
    UserConfig(UserConfig&&) noexcept = default;
    UserConfig& operator=(UserConfig&&) noexcept = default;
    // Arduino String copies can silently become empty on allocation refusal.
    // Prepare and verify every owned value before publishing any replacement.
    bool tryAssign(const UserConfig& source);
    void swap(UserConfig& other) noexcept;
    static bool trySetString(String& target, const char* bytes, size_t length) {
        if ((!bytes && length) || length > StoredLimit) return false;
        try {
            String prepared;
            if (length && (!handheld::config::Memory::reserveString(prepared, length) ||
                           !prepared.concat(bytes, length))) return false;
            if (prepared.length() != length || (length && std::memcmp(prepared.c_str(), bytes, length)) ||
                !handheld::config::Memory::admits(0)) return false;
            std::swap(target, prepared);
            return true;
        } catch (const std::bad_alloc&) { return false; }
    }
    // Flash-only (original API, kept for compatibility)
    bool load(FlashStore& flash);
    bool save(FlashStore& flash);

    // Internal flash is authoritative; removable SD is a repairable mirror.
    bool load(SDStore& sd, FlashStore& flash);
    bool save(SDStore& sd, FlashStore& flash);
    bool flushPending(SDStore& sd, FlashStore& flash);
    bool mirrorPending() const { return _mirrorPending || _nvsMirrorPending; }
    bool recoveryRequired() const { return _recoveryRequired; }
    bool settingsPending() const { return _namePending; }
    enum class Source : uint8_t { Absent, Flash, FlashBackup, Nvs, Sd, Unavailable, Invalid };
    Source source() const { return _source; }

    UserSettings& settings() { return _settings; }
    const UserSettings& settings() const { return _settings; }

    // Value snapshots for the UI/service boundary; no storage access.
    String encode() { return serializeToJson(false, SnapshotLimit); }
    bool decode(const String& json) { return parseJson(json, false); }
    bool decode(const char* json, size_t length) { return parseJson(json, length, false); }
    static constexpr size_t StoredLimit = 32768;
    static constexpr size_t NvsStringLimit = 4000; // pinned SDK maximum, including NUL
    static constexpr size_t SnapshotLimit = 4096;
    static constexpr size_t JsonAllocationLimit = 2 * StoredLimit;

private:
    friend class SettingsTransaction;
    bool parseJson(const String& json, bool persisted = true, bool* unavailable = nullptr);
    bool parseJson(const char* json, size_t length, bool persisted = true, bool* unavailable = nullptr);
    String serializeToJson(bool persisted = true, size_t limit = StoredLimit, bool* unavailable = nullptr);
    bool saveRequired(FlashStore& flash);
    bool saveRequired(FlashStore& flash, const String& encoded);
    void copyStateFrom(const UserConfig& source) noexcept;
    uint8_t _nameIdentity[16]{};
    bool _nameBound = false;
    bool _namePending = false;
    Source _source = Source::Absent;
    static void sanitizeSettings(UserSettings& settings);

    UserSettings _settings;
    bool _mirrorPending = false;
    bool _nvsMirrorPending = false;
    bool _recoveryRequired = false;
};
