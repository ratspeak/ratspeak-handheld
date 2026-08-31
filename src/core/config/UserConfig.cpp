#include "UserConfig.h"
#include "config/BoardConfig.h"
#include "radio/RadioFrequency.h"
#include <algorithm>
#if USERCONFIG_NVS_BACKUP
#include <Preferences.h>
#endif

namespace {
#if USERCONFIG_NVS_BACKUP
// Full-JSON NVS backup tier — internal flash, no SPI bus, wear-leveled
bool saveToNVS(const String& json) {
    Preferences prefs;
    if (!prefs.begin(NVS_NS_CFG, false)) return false;
    bool ok = prefs.putString("json", json) > 0;
    prefs.end();
    if (ok) Serial.println("[CONFIG] Saved to NVS");
    return ok;
}

String loadFromNVS() {
    Preferences prefs;
    if (!prefs.begin(NVS_NS_CFG, true)) return "";
    String json = prefs.getString("json", "");
    prefs.end();
    return json;
}
#endif
}

void UserConfig::sanitizeSettings() {
    if (_settings.radioRegion >= REGION_COUNT) _settings.radioRegion = REGION_AMERICAS;
    // Region selects a default; it must not override a supported manual tune,
    // including after the developer controls are locked again.
    if (!loRaFrequencyBand(_settings.loraFrequency)) {
        _settings.loraFrequency = REGION_FREQ[constrain((int)_settings.radioRegion, 0, REGION_COUNT - 1)];
    }
    _settings.loraSF = constrain(_settings.loraSF, 5, 12);
    _settings.loraBW = constrain(_settings.loraBW, 7800UL, 500000UL);
    _settings.loraCR = constrain(_settings.loraCR, 5, 8);
    _settings.loraTxPower = constrain(_settings.loraTxPower, -9, 22);
    _settings.loraPreamble = constrain(_settings.loraPreamble, 6L, 65L);
    _settings.batteryDisplay = constrain((int)_settings.batteryDisplay,
        (int)BATTERY_DISPLAY_PERCENT, (int)BATTERY_DISPLAY_BAR);
    _settings.batteryModel = constrain((int)_settings.batteryModel,
        (int)BATTERY_MODEL_LIPO, (int)BATTERY_MODEL_LINEAR);
    _settings.chargeThresholdV = constrain(_settings.chargeThresholdV,
        BATTERY_CHARGE_THRESHOLD_MIN, BATTERY_CHARGE_THRESHOLD_MAX);
    _settings.fullBatteryV = constrain(_settings.fullBatteryV,
        BATTERY_FULL_VOLTAGE_MIN, BATTERY_FULL_VOLTAGE_MAX);

    _settings.screenDimTimeout = constrain(_settings.screenDimTimeout, 5, 3600);
    _settings.screenOffTimeout = constrain(_settings.screenOffTimeout, 10, 7200);
    if (_settings.screenOffTimeout < _settings.screenDimTimeout) {
        _settings.screenOffTimeout = _settings.screenDimTimeout;
    }
    _settings.brightness = constrain(_settings.brightness, 1, 100);
    _settings.keyboardBrightness = keyboardBacklightPercent(keyboardBacklightChoice(_settings.keyboardBrightness));
    _settings.trackballSpeed = constrain(_settings.trackballSpeed, 1, 5);
    _settings.touchSensitivity = constrain(_settings.touchSensitivity, 1, 5);
    _settings.audioVolume = constrain(_settings.audioVolume, 0, 100);
    if (_settings.timezoneIdx >= 21) _settings.timezoneIdx = 6;
    _settings.autoIfaceMaxPeers = constrain(_settings.autoIfaceMaxPeers, 1, 16);

    std::vector<TCPEndpoint> cleanTcp;
    cleanTcp.reserve(std::min((size_t)MAX_TCP_CONNECTIONS, _settings.tcpConnections.size()));
    for (auto& ep : _settings.tcpConnections) {
        ep.host.trim();
        if (ep.host.isEmpty() || ep.port == 0) continue;
        cleanTcp.push_back(ep);
        if (cleanTcp.size() >= MAX_TCP_CONNECTIONS) break;
    }
    _settings.tcpConnections = cleanTcp;
}

bool UserConfig::parseJson(const String& json) {
    Serial.printf("[CONFIG] Parsing config (%d bytes)\n", json.length());

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("[CONFIG] Parse error: %s\n", err.c_str());
        return false;
    }
    // All supported persisted configurations contain the radio frequency.
    // Valid JSON such as null, [] or {} is not a configuration: accepting it
    // would silently replace the user's settings with defaults.
    if (!doc.is<JsonObject>() || !doc["lora_freq"].is<uint32_t>()) return false;

    _settings.radioRegion   = constrain((int)(doc["radio_region"] | 0), 0, REGION_COUNT - 1);
    _settings.loraFrequency = doc["lora_freq"] | (long)LORA_DEFAULT_FREQ;
    _settings.loraSF        = doc["lora_sf"]   | (int)LORA_DEFAULT_SF;
    _settings.loraBW        = doc["lora_bw"]   | (long)LORA_DEFAULT_BW;
    _settings.loraCR        = doc["lora_cr"]   | (int)LORA_DEFAULT_CR;
    _settings.loraTxPower   = doc["lora_txp"]  | (int)LORA_DEFAULT_TX_POWER;
    _settings.loraPreamble  = doc["lora_pre"]  | (long)LORA_DEFAULT_PREAMBLE;
    _settings.loraEnabled   = doc["lora_on"]   | true;

    // WiFi mode — migrate from legacy wifi_enabled bool
    int mode = doc["wifi_mode"] | -1;
    if (mode >= 0) {
        _settings.wifiMode = (RatWiFiMode)constrain(mode, 0, 2);
    } else {
        _settings.wifiMode = (doc["wifi_enabled"] | true) ? RAT_WIFI_AP : RAT_WIFI_OFF;
    }
    int restoreMode = doc["wifi_restore_mode"] | (int)(_settings.wifiMode == RAT_WIFI_OFF ? RAT_WIFI_STA : _settings.wifiMode);
    _settings.wifiRestoreMode = (RatWiFiMode)constrain(restoreMode, 1, 2);
    if (_settings.wifiMode != RAT_WIFI_OFF) _settings.wifiRestoreMode = _settings.wifiMode;
    _settings.wifiAPSSID     = doc["wifi_ap_ssid"]     | "";
    _settings.wifiAPPassword = doc["wifi_ap_pass"]     | WIFI_AP_PASSWORD;
    _settings.wifiSTASelected = constrain((int)(doc["wifi_sta_selected"] | 0), 0, (int)WIFI_STA_MAX_NETWORKS - 1);

    // Migrate legacy single-network config into the multi-network list.
    _settings.wifiSTANetworks.clear();
    JsonArray staArr = doc["wifi_sta_networks"];
    if (staArr) {
        for (JsonObject obj : staArr) {
            if (_settings.wifiSTANetworks.size() >= WIFI_STA_MAX_NETWORKS) break;
            WiFiNetwork n;
            n.ssid = obj["ssid"] | "";
            n.password = obj["pass"] | "";
            _settings.wifiSTANetworks.push_back(n);
        }
    } else {
        WiFiNetwork legacy;
        legacy.ssid = doc["wifi_sta_ssid"] | "";
        legacy.password = doc["wifi_sta_pass"] | "";
        if (!legacy.ssid.isEmpty()) _settings.wifiSTANetworks.push_back(legacy);
    }

    // AutoInterface (LAN auto-discovery)
    _settings.autoIfaceEnabled  = doc["autoiface_en"]    | false;
    _settings.autoIfaceGroupId  = doc["autoiface_group"] | "reticulum";
    _settings.autoIfaceMaxPeers = doc["autoiface_max"]   | BOARD_DEFAULT_AUTOIFACE_MAX_PEERS;

    // TCP outbound connections
    _settings.tcpConnections.clear();
    JsonArray tcpArr = doc["tcp_connections"];
    if (tcpArr) {
        for (JsonObject obj : tcpArr) {
            if (_settings.tcpConnections.size() >= MAX_TCP_CONNECTIONS) break;
            TCPEndpoint ep;
            ep.host = obj["host"] | "";
            ep.port = obj["port"] | TCP_DEFAULT_PORT;
            ep.autoConnect = obj["auto"] | true;
            if (!ep.host.isEmpty()) _settings.tcpConnections.push_back(ep);
        }
    }

    _settings.screenDimTimeout = doc["screen_dim"] | 30;
    _settings.screenOffTimeout = doc["screen_off"] | 60;
    // Brightness: stored as 1-100%. Migrate old 0-255 values.
    int rawBri = doc["brightness"] | BOARD_DEFAULT_BRIGHTNESS;
    if (rawBri > 100) rawBri = rawBri * 100 / 255;  // Migrate from PWM to percentage
    _settings.brightness = constrain(rawBri, 1, 100);
    _settings.denseFontMode    = doc["dense_font"] | false;
    _settings.themeLight       = doc["theme_light"] | false;
    _settings.keyboardBrightness = constrain(doc["kb_brightness"] | 0, 0, 100);
    _settings.keyboardAutoOn     = doc["kb_auto_on"] | false;
    _settings.keyboardAutoOff    = doc["kb_auto_off"] | false;
    _settings.trackballSpeed   = doc["trackball_speed"] | 3;
    _settings.touchSensitivity = doc["touch_sens"] | 3;
    _settings.bleEnabled       = false;

    // battery settings
    _settings.batteryDisplay = doc["batt_display"] | BATTERY_DISPLAY_BAR;
    _settings.batteryModel = doc["batt_model"] | BATTERY_MODEL_LIPO;
    _settings.chargeThresholdV = doc["charge_thresh_v"] | BATTERY_CHARGE_THRESHOLD_DEFAULT;
    _settings.fullBatteryV = doc["full_battery_v"] | BATTERY_FULL_VOLTAGE_DEFAULT;

    _settings.gpsTimeEnabled     = doc["gps_time"]     | true;
#if LEGACY_GPS_LOC_MIGRATION
    _settings.gpsLocationEnabled = doc["gps_location"].isNull()
        ? (doc["gps_loc"] | false)
        : (doc["gps_location"] | false);
#else
    _settings.gpsLocationEnabled = doc["gps_location"] | false;
#endif
    _settings.timezoneIdx        = doc["tz_idx"]       | 6;
    _settings.timezoneSet        = doc["tz_set"]       | false;
    _settings.use24HourTime      = doc["time_24h"]     | false;

    _settings.audioEnabled = doc["audio_on"]  | true;
    _settings.audioVolume  = doc["audio_vol"] | 80;

    _settings.displayName = doc["display_name"] | "";
    _settings.sdStorageEnabled = doc["sd_storage"] | _settings.sdStorageEnabled;
    _settings.announceInterval = doc["announce_int"] | 30;
    if (_settings.announceInterval < 30) _settings.announceInterval = 30;
    if (_settings.announceInterval > 360) _settings.announceInterval = 360;
    _settings.devMode     = doc["dev_mode"]     | false;

    sanitizeSettings();
    Serial.println("[CONFIG] Settings loaded");
    return true;
}

String UserConfig::serializeToJson() {
    sanitizeSettings();
    JsonDocument doc;

    doc["radio_region"] = _settings.radioRegion;
    doc["lora_freq"] = _settings.loraFrequency;
    doc["lora_sf"]   = _settings.loraSF;
    doc["lora_bw"]   = _settings.loraBW;
    doc["lora_cr"]   = _settings.loraCR;
    doc["lora_txp"]  = _settings.loraTxPower;
    doc["lora_pre"]  = _settings.loraPreamble;
    doc["lora_on"]   = _settings.loraEnabled;

    doc["wifi_mode"] = (int)_settings.wifiMode;
    doc["wifi_restore_mode"] = (int)_settings.wifiRestoreMode;
    doc["wifi_ap_ssid"] = _settings.wifiAPSSID;
    doc["wifi_ap_pass"] = _settings.wifiAPPassword;
    doc["wifi_sta_selected"] = (int)constrain((int)_settings.wifiSTASelected, 0, (int)WIFI_STA_MAX_NETWORKS - 1);
    JsonArray staArr = doc["wifi_sta_networks"].to<JsonArray>();
    for (size_t slot = 0; slot < WIFI_STA_MAX_NETWORKS; slot++) {
        JsonObject obj = staArr.add<JsonObject>();
        if (slot < _settings.wifiSTANetworks.size()) {
            obj["ssid"] = _settings.wifiSTANetworks[slot].ssid;
            obj["pass"] = _settings.wifiSTANetworks[slot].password;
        } else {
            obj["ssid"] = "";
            obj["pass"] = "";
        }
    }

    doc["autoiface_en"]    = _settings.autoIfaceEnabled;
    doc["autoiface_group"] = _settings.autoIfaceGroupId;
    doc["autoiface_max"]   = _settings.autoIfaceMaxPeers;

    JsonArray tcpArr = doc["tcp_connections"].to<JsonArray>();
    for (auto& ep : _settings.tcpConnections) {
        JsonObject obj = tcpArr.add<JsonObject>();
        obj["host"] = ep.host;
        obj["port"] = ep.port;
        obj["auto"] = ep.autoConnect;
    }

    doc["screen_dim"] = _settings.screenDimTimeout;
    doc["screen_off"] = _settings.screenOffTimeout;
    doc["brightness"] = _settings.brightness;
    doc["dense_font"] = _settings.denseFontMode;
    doc["theme_light"] = _settings.themeLight;
    doc["kb_brightness"] = _settings.keyboardBrightness;
    doc["kb_auto_on"] = _settings.keyboardAutoOn;
    doc["kb_auto_off"] = _settings.keyboardAutoOff;
    doc["trackball_speed"] = _settings.trackballSpeed;
    doc["touch_sens"] = _settings.touchSensitivity;
    doc["ble_enabled"] = false;

    // battery settings
    doc["batt_display"] = _settings.batteryDisplay;
    doc["batt_model"]   = _settings.batteryModel;
    doc["charge_thresh_v"] = _settings.chargeThresholdV;
    doc["full_battery_v"]  = _settings.fullBatteryV;

    doc["gps_time"]     = _settings.gpsTimeEnabled;
    doc["gps_location"] = _settings.gpsLocationEnabled;
    doc["tz_idx"]       = _settings.timezoneIdx;
    doc["tz_set"]       = _settings.timezoneSet;
    doc["time_24h"]     = _settings.use24HourTime;

    doc["audio_on"]  = _settings.audioEnabled;
    doc["audio_vol"] = _settings.audioVolume;

    doc["display_name"] = _settings.displayName;
    doc["sd_storage"] = _settings.sdStorageEnabled;
    doc["announce_int"] = _settings.announceInterval;
    doc["dev_mode"]     = _settings.devMode;

    String json;
    if (doc.overflowed() || serializeJson(doc, json) != measureJson(doc)) return "";
    return json;
}

bool UserConfig::load(FlashStore& flash) {
    _recoveryRequired = false;
    String json = flash.readString(PATH_USER_CONFIG);
    if (!json.isEmpty() && parseJson(json)) return true;
    // Do not overwrite an existing, unreadable primary during automatic repair.
    // Its backup remains available for explicit recovery.
    if (flash.exists(PATH_USER_CONFIG)) { _recoveryRequired = true; return false; }
    json = flash.readString(PATH_USER_CONFIG ".bak");
    if (!json.isEmpty() && parseJson(json)) return true;
#if USERCONFIG_NVS_BACKUP
    json = loadFromNVS();
    if (!json.isEmpty()) {
        Serial.println("[CONFIG] Recovered from NVS (flash file missing)");
        if (parseJson(json)) return true;
    }
#endif
    _recoveryRequired = !flash.isReady() || flash.exists(PATH_USER_CONFIG) || flash.exists(PATH_USER_CONFIG ".bak");
    Serial.println(_recoveryRequired ? "[CONFIG] Saved config unreadable; recovery required" : "[CONFIG] No saved config, using defaults");
    return false;
}

bool UserConfig::save(FlashStore& flash) {
    String json = serializeToJson();
    if (json.isEmpty() || _recoveryRequired) return false;
    bool ok = flash.writeString(PATH_USER_CONFIG, json);
    if (ok) Serial.println("[CONFIG] Settings saved to flash");
#if USERCONFIG_NVS_BACKUP
    if (ok) saveToNVS(json);
#endif
    return ok;
}

bool UserConfig::load(SDStore& sd, FlashStore& flash) {
    if (load(flash)) {
        // A stale/removable mirror must never override a committed flash value.
        _mirrorPending = _settings.sdStorageEnabled;
        return true;
    }
    if (!flash.isReady() || _recoveryRequired) return false;
    String backup = sd.isReady() ? sd.readString(SD_PATH_USER_CONFIG) : String();
    if (!backup.isEmpty() && parseJson(backup)) {
        _settings.sdStorageEnabled = true;
        _recoveryRequired = false;
        if (save(flash)) {
            _mirrorPending = true;
            Serial.println("[CONFIG] Restored settings from SD backup");
            return true;
        }
        _recoveryRequired = true;
        return false;
    }
    _recoveryRequired = sd.isReady() &&
        (sd.exists(SD_PATH_USER_CONFIG) || sd.exists(SD_PATH_USER_CONFIG ".bak"));
    return false;
}

bool UserConfig::save(SDStore& sd, FlashStore& flash) {
    // Publish one authoritative commit before touching any secondary copy.
    // A failed flash commit leaves both the live settings and SD unchanged.
    if (!save(flash)) return false;
    _mirrorPending = _settings.sdStorageEnabled;
    flushPending(sd, flash);
    return true;
}

bool UserConfig::flushPending(SDStore& sd, FlashStore& flash) {
    (void)flash;
    if (!_mirrorPending || !_settings.sdStorageEnabled) { _mirrorPending = false; return true; }
    // Optional absent media cannot prevent a safe shutdown: flash is durable.
    if (!sd.isReady()) return true;
    const String json = serializeToJson();
    if (json.isEmpty()) return false;
    if (sd.readString(SD_PATH_USER_CONFIG) == json) { _mirrorPending = false; return true; }
    if (!sd.ensureDir(SD_PATH_ROOT) || !sd.ensureDir(SD_PATH_CONFIG_DIR) ||
        !sd.writeString(SD_PATH_USER_CONFIG, json)) return false;
    _mirrorPending = false;
    return true;
}
