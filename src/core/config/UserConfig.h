#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
#include "storage/FlashStore.h"
#include "storage/SDStore.h"
#include "config/Config.h"
#include "config/BoardConfig.h"

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

    // Display
    uint16_t screenDimTimeout = 30;   // seconds
    uint16_t screenOffTimeout = 60;   // seconds
    uint8_t brightness = BOARD_DEFAULT_BRIGHTNESS;  // Percentage 1-100, per-board default
    bool denseFontMode = false;       // T-Deck Plus: adaptive font toggle
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
    uint8_t touchSensitivity = 3;     // 1-5

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

    // Storage. Removable SD stores plaintext unless explicitly enabled; boards
    // whose legacy installs always used SD default it on (BOARD_DEFAULT_SD_STORAGE).
    bool sdStorageEnabled = BOARD_DEFAULT_SD_STORAGE;

    // Announce
    uint16_t announceInterval = 30; // minutes, 30-360

    // Developer mode — unlocks custom radio parameters
    bool devMode = false;
};

class UserConfig {
public:
    // Flash-only (original API, kept for compatibility)
    bool load(FlashStore& flash);
    bool save(FlashStore& flash);

    // Internal flash is authoritative; removable SD is a repairable mirror.
    bool load(SDStore& sd, FlashStore& flash);
    bool save(SDStore& sd, FlashStore& flash);
    bool flushPending(SDStore& sd, FlashStore& flash);
    bool mirrorPending() const { return _mirrorPending; }
    bool recoveryRequired() const { return _recoveryRequired; }

    UserSettings& settings() { return _settings; }
    const UserSettings& settings() const { return _settings; }

    // Value snapshots for the UI/service boundary; no storage access.
    String encode() { return serializeToJson(); }
    bool decode(const String& json) { return parseJson(json); }

private:
    bool parseJson(const String& json);
    String serializeToJson();
    void sanitizeSettings();

    UserSettings _settings;
    bool _mirrorPending = false;
    bool _recoveryRequired = false;
};
