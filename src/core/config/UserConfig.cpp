#include "UserConfig.h"
#include "config/AnnounceInterval.h"
#include "config/BoardConfig.h"
#include "radio/RadioFrequency.h"
#include "radio/RadioBandwidth.h"
#include "storage/StorageJsonAllocator.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#if USERCONFIG_NVS_BACKUP
#include <Preferences.h>
#include <nvs.h>
#endif

namespace {
bool sameConfigString(const String& left, const String& right) {
    return left.length() == right.length() && (!left.length() ||
        !std::memcmp(left.c_str(), right.c_str(), left.length()));
}

bool sameConfigStrings(const UserSettings& left, const UserSettings& right) {
    if (!sameConfigString(left.wifiAPSSID, right.wifiAPSSID) ||
        !sameConfigString(left.wifiAPPassword, right.wifiAPPassword) ||
        !sameConfigString(left.autoIfaceGroupId, right.autoIfaceGroupId) ||
        !sameConfigString(left.displayName, right.displayName) ||
        left.wifiSTANetworks.size() != right.wifiSTANetworks.size() ||
        left.tcpConnections.size() != right.tcpConnections.size()) return false;
    for (size_t i = 0; i < left.wifiSTANetworks.size(); ++i)
        if (!sameConfigString(left.wifiSTANetworks[i].ssid, right.wifiSTANetworks[i].ssid) ||
            !sameConfigString(left.wifiSTANetworks[i].password, right.wifiSTANetworks[i].password)) return false;
    for (size_t i = 0; i < left.tcpConnections.size(); ++i)
        if (!sameConfigString(left.tcpConnections[i].host, right.tcpConnections[i].host)) return false;
    return true;
}

bool configCopyFits(const UserSettings& settings, size_t& charge) {
    if (settings.wifiSTANetworks.size() > WIFI_STA_MAX_NETWORKS ||
        settings.tcpConnections.size() > MAX_TCP_CONNECTIONS) return false;
    size_t remaining = UserConfig::StoredLimit;
    charge = handheld::config::Memory::charge(settings.wifiSTANetworks.size() * sizeof(WiFiNetwork)) +
        handheld::config::Memory::charge(settings.tcpConnections.size() * sizeof(TCPEndpoint));
    const auto take = [&remaining, &charge](const String& value) {
        if (value.length() > remaining) return false;
        remaining -= value.length();
        charge += handheld::config::Memory::stringCharge(value.length());
        return true;
    };
    if (!take(settings.wifiAPSSID) || !take(settings.wifiAPPassword) ||
        !take(settings.autoIfaceGroupId) || !take(settings.displayName)) return false;
    for (const auto& network : settings.wifiSTANetworks)
        if (!take(network.ssid) || !take(network.password)) return false;
    for (const auto& endpoint : settings.tcpConnections) if (!take(endpoint.host)) return false;
    return true;
}

bool assignConfigString(String& target, const char* source) {
    return UserConfig::trySetString(target, source, std::strlen(source));
}

template<typename T>
bool reserveConfigVector(std::vector<T>& values, size_t count) {
    if (!handheld::config::Memory::admits(handheld::config::Memory::charge(count * sizeof(T)))) return false;
    values.reserve(count);
    return handheld::config::Memory::admits(0);
}

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

using ConfigBytes = std::unique_ptr<char, decltype(&std::free)>;
ConfigBytes loadFromNVS(bool& available, bool& present, size_t& length) {
    ConfigBytes bytes(nullptr, &std::free);
    available = false; present = false; length = 0;
    nvs_handle_t handle;
    const esp_err_t opened = nvs_open(NVS_NS_CFG, NVS_READONLY, &handle);
    if (opened == ESP_ERR_NVS_NOT_FOUND) { available = true; return bytes; }
    if (opened != ESP_OK) return bytes;
    struct Close { nvs_handle_t handle; ~Close() { nvs_close(handle); } } close{handle};
    size_t capacity = 0;
    const auto probe = nvs_get_str(handle, "json", nullptr, &capacity);
    if (probe == ESP_ERR_NVS_NOT_FOUND) { available = true; return bytes; }
    present = true;
    if (probe != ESP_OK) return bytes;
    available = true;
    // The pinned Preferences::getString overload uses char buf[len] on the
    // caller's stack. Bound the source before allocating, then parse these
    // bytes directly without a second full-size Arduino String copy.
    if (!capacity || capacity > UserConfig::NvsStringLimit) return bytes;
    if (!handheld::config::Memory::admits(handheld::config::Memory::charge(capacity))) {
        available = false; return bytes;
    }
    bytes.reset(static_cast<char*>(std::malloc(capacity)));
    if (!bytes || !handheld::config::Memory::admits(0)) {
        available = false; bytes.reset(); return bytes;
    }
    size_t actual = capacity;
    if (nvs_get_str(handle, "json", bytes.get(), &actual) != ESP_OK ||
        actual != capacity || bytes.get()[capacity - 1] != '\0') {
        available = false; bytes.reset(); return bytes;
    }
    length = capacity - 1;
    return bytes;
}
#endif
}

bool UserConfig::tryAssign(const UserConfig& source) {
    if (this == &source) return true;
    // Every valid stored record fits this aggregate source-byte allowance.
    // Check counts/bytes before copying a mutable UI candidate's allocations.
    size_t charge = 0;
    if (!configCopyFits(source._settings, charge) || !handheld::config::Memory::admits(charge)) return false;
    try {
        // Compiler-generated scalar/member copying stays the single value map.
        // Verify the SDK's nonthrowing String failures before swapping values.
        UserSettings prepared = source._settings;
        if (!sameConfigStrings(prepared, source._settings) || !handheld::config::Memory::admits(0)) return false;
        std::swap(_settings, prepared);
        copyStateFrom(source);
        return true;
    } catch (const std::bad_alloc&) { return false; }
}

void UserConfig::copyStateFrom(const UserConfig& source) noexcept {
    std::memcpy(_nameIdentity, source._nameIdentity, sizeof _nameIdentity);
    _nameBound = source._nameBound; _namePending = source._namePending;
    _source = source._source; _mirrorPending = source._mirrorPending;
    _nvsMirrorPending = source._nvsMirrorPending; _recoveryRequired = source._recoveryRequired;
}

void UserConfig::swap(UserConfig& other) noexcept {
    // Whole-value moves empty their source first. Pinned WString moves and
    // std::allocator vector moves allocate nothing and release old capacities.
    std::swap(_settings, other._settings);
    std::swap(_nameIdentity, other._nameIdentity);
    std::swap(_nameBound, other._nameBound); std::swap(_namePending, other._namePending);
    std::swap(_source, other._source); std::swap(_mirrorPending, other._mirrorPending);
    std::swap(_nvsMirrorPending, other._nvsMirrorPending);
    std::swap(_recoveryRequired, other._recoveryRequired);
}

void UserConfig::sanitizeSettings(UserSettings& settings) {
    if (settings.radioRegion >= REGION_COUNT) settings.radioRegion = REGION_AMERICAS;
    // Region selects a default; it must not override a supported manual tune,
    // including after the developer controls are locked again.
    if (!loRaFrequencyBand(settings.loraFrequency)) {
        settings.loraFrequency = REGION_FREQ[constrain((int)settings.radioRegion, 0, REGION_COUNT - 1)];
    }
    settings.loraSF = constrain(settings.loraSF, 5, 12);
    settings.loraBW = RadioBandwidth::normalize(settings.loraBW);
    settings.loraCR = constrain(settings.loraCR, 5, 8);
    settings.loraTxPower = constrain(settings.loraTxPower, -9, 22);
    settings.loraPreamble = constrain(settings.loraPreamble, 6L, 65L);
    settings.batteryDisplay = constrain((int)settings.batteryDisplay,
        (int)BATTERY_DISPLAY_PERCENT, (int)BATTERY_DISPLAY_BAR);
    settings.batteryModel = constrain((int)settings.batteryModel,
        (int)BATTERY_MODEL_LIPO, (int)BATTERY_MODEL_LINEAR);
    if (!std::isfinite(settings.chargeThresholdV)) settings.chargeThresholdV = BATTERY_CHARGE_THRESHOLD_DEFAULT;
    if (!std::isfinite(settings.fullBatteryV)) settings.fullBatteryV = BATTERY_FULL_VOLTAGE_DEFAULT;
    settings.chargeThresholdV = constrain(settings.chargeThresholdV,
        BATTERY_CHARGE_THRESHOLD_MIN, BATTERY_CHARGE_THRESHOLD_MAX);
    settings.fullBatteryV = constrain(settings.fullBatteryV,
        BATTERY_FULL_VOLTAGE_MIN, BATTERY_FULL_VOLTAGE_MAX);

    settings.screenDimTimeout = constrain(settings.screenDimTimeout, 5, 3600);
    settings.screenOffTimeout = constrain(settings.screenOffTimeout, 10, 7200);
    if (settings.screenOffTimeout < settings.screenDimTimeout) {
        settings.screenOffTimeout = settings.screenDimTimeout;
    }
    settings.brightness = constrain(settings.brightness, 1, 100);
    settings.keyboardBrightness = keyboardBacklightPercent(keyboardBacklightChoice(settings.keyboardBrightness));
    settings.trackballSpeed = constrain(settings.trackballSpeed, 1, 5);
    settings.touchSensitivity = constrain(settings.touchSensitivity, 1, 5);
    settings.audioVolume = constrain(settings.audioVolume, 0, 100);
    if (settings.timezoneIdx >= 21) settings.timezoneIdx = 6;
    settings.autoIfaceMaxPeers = constrain(settings.autoIfaceMaxPeers, 1, 16);
    settings.announceInterval = handheld::announce::normalizeMinutes(settings.announceInterval);

    size_t retained = 0;
    for (size_t i = 0; i < settings.tcpConnections.size(); ++i) {
        auto& ep = settings.tcpConnections[i];
        ep.host.trim();
        if (ep.host.isEmpty() || ep.port == 0) continue;
        if (retained != i) settings.tcpConnections[retained] = std::move(ep);
        if (++retained >= MAX_TCP_CONNECTIONS) break;
    }
    settings.tcpConnections.resize(retained);
}

bool UserConfig::parseJson(const String& json, bool persisted, bool* unavailable) {
    return parseJson(json.c_str(), json.length(), persisted, unavailable);
}

bool UserConfig::parseJson(const char* json, size_t length, bool persisted, bool* unavailable) {
    if (unavailable) *unavailable = false;
    const auto memoryFailure = [unavailable]() {
        if (unavailable) *unavailable = true;
        return false;
    };
    if (!json || !length || length > StoredLimit) return false;
    Serial.printf("[CONFIG] Parsing config (%u bytes)\n", unsigned(length));

    try {
        handheld::config::JsonAllocator allocator(JsonAllocationLimit);
        JsonDocument doc(&allocator);
        DeserializationError err = deserializeJson(doc, json, length);
        if (err) {
            Serial.printf("[CONFIG] Parse error: %s\n", err.c_str());
            if (err == DeserializationError::NoMemory) return memoryFailure();
            return false;
        }
        // All supported persisted configurations contain the radio frequency.
        // Valid JSON such as null, [] or {} is not a configuration: accepting it
        // would silently replace the user's settings with defaults.
        if (!doc.is<JsonObject>() || !doc["lora_freq"].is<uint32_t>()) return false;
        UserSettings parsed;
        parsed.sdStorageEnabled = _settings.sdStorageEnabled;
        bool nameBound = false, namePending = false;
        uint8_t nameIdentity[16]{};
        if (persisted) {
            if (!doc["name_identity"].isNull()) {
                const char* hex = doc["name_identity"] | "";
                if (strlen(hex) != 32) return false;
                for (size_t i = 0; i < 32; ++i) {
                    const char c = hex[i];
                    const int n = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
                    if (n < 0) return false;
                    nameIdentity[i / 2] |= uint8_t(n << (i % 2 ? 0 : 4));
                }
                nameBound = true;
            }
            if (!doc["name_pending"].isNull() && !doc["name_pending"].is<bool>()) return false;
            namePending = doc["name_pending"] | false;
            if (namePending && !nameBound) return false;
        }

        parsed.radioRegion   = constrain((int)(doc["radio_region"] | 0), 0, REGION_COUNT - 1);
        parsed.loraFrequency = doc["lora_freq"] | (long)LORA_DEFAULT_FREQ;
        // Clamp in the JSON reader's integer type before narrowing to stored fields.
        parsed.loraSF        = constrain(doc["lora_sf"] | (int)LORA_DEFAULT_SF, 5, 12);
        parsed.loraBW        = constrain(doc["lora_bw"] | (long)LORA_DEFAULT_BW, 7800L, 500000L);
        parsed.loraCR        = constrain(doc["lora_cr"] | (int)LORA_DEFAULT_CR, 5, 8);
        parsed.loraTxPower   = constrain(doc["lora_txp"] | (int)LORA_DEFAULT_TX_POWER, -9, 22);
        parsed.loraPreamble  = doc["lora_pre"]  | (long)LORA_DEFAULT_PREAMBLE;
        parsed.loraEnabled   = doc["lora_on"]   | true;

        // WiFi mode — migrate from legacy wifi_enabled bool
        int mode = doc["wifi_mode"] | -1;
        if (mode >= 0) {
            parsed.wifiMode = (RatWiFiMode)constrain(mode, 0, 2);
        } else {
            parsed.wifiMode = (doc["wifi_enabled"] | true) ? RAT_WIFI_AP : RAT_WIFI_OFF;
        }
        int restoreMode = doc["wifi_restore_mode"] | (int)(parsed.wifiMode == RAT_WIFI_OFF ? RAT_WIFI_STA : parsed.wifiMode);
        parsed.wifiRestoreMode = (RatWiFiMode)constrain(restoreMode, 1, 2);
        if (parsed.wifiMode != RAT_WIFI_OFF) parsed.wifiRestoreMode = parsed.wifiMode;
        if (!assignConfigString(parsed.wifiAPSSID, doc["wifi_ap_ssid"]     | "")) return memoryFailure();
        if (!assignConfigString(parsed.wifiAPPassword, doc["wifi_ap_pass"]     | WIFI_AP_PASSWORD)) return memoryFailure();
        parsed.wifiSTASelected = constrain((int)(doc["wifi_sta_selected"] | 0), 0, (int)WIFI_STA_MAX_NETWORKS - 1);

        // Migrate legacy single-network config into the multi-network list.
        JsonArray staArr = doc["wifi_sta_networks"];
        if (staArr) {
            if (!reserveConfigVector(parsed.wifiSTANetworks, std::min(staArr.size(), WIFI_STA_MAX_NETWORKS)))
                return memoryFailure();
            for (JsonObject obj : staArr) {
                if (parsed.wifiSTANetworks.size() >= WIFI_STA_MAX_NETWORKS) break;
                WiFiNetwork n;
                if (!assignConfigString(n.ssid, obj["ssid"] | "")) return memoryFailure();
                if (!assignConfigString(n.password, obj["pass"] | "")) return memoryFailure();
                parsed.wifiSTANetworks.push_back(std::move(n));
            }
        } else {
            WiFiNetwork legacy;
            if (!assignConfigString(legacy.ssid, doc["wifi_sta_ssid"] | "")) return memoryFailure();
            if (!assignConfigString(legacy.password, doc["wifi_sta_pass"] | "")) return memoryFailure();
            if (!legacy.ssid.isEmpty()) {
                if (!reserveConfigVector(parsed.wifiSTANetworks, 1)) return memoryFailure();
                parsed.wifiSTANetworks.push_back(std::move(legacy));
            }
        }

        // AutoInterface (LAN auto-discovery)
        parsed.autoIfaceEnabled  = doc["autoiface_en"]    | false;
        if (!assignConfigString(parsed.autoIfaceGroupId, doc["autoiface_group"] | "reticulum")) return memoryFailure();
        parsed.autoIfaceMaxPeers = constrain(doc["autoiface_max"] | (int)BOARD_DEFAULT_AUTOIFACE_MAX_PEERS, 1, 16);

        // TCP outbound connections
        JsonArray tcpArr = doc["tcp_connections"];
        if (tcpArr) {
            if (!reserveConfigVector(parsed.tcpConnections, std::min(tcpArr.size(), size_t(MAX_TCP_CONNECTIONS))))
                return memoryFailure();
            for (JsonObject obj : tcpArr) {
                if (parsed.tcpConnections.size() >= MAX_TCP_CONNECTIONS) break;
                TCPEndpoint ep;
                const int port = obj["port"] | TCP_DEFAULT_PORT;
                if (port < 1 || port > 65535) continue;
                const char* host = obj["host"] | "";
                // Older LVGL presets copied 15 of the hub's 16 characters.
                // Repair only that exact persisted preset; retain enable state,
                // other ports and custom endpoints. Normal saving persists it.
                if (persisted && port == TCP_DEFAULT_PORT && std::strcmp(host, "rns.ratspeak.or") == 0)
                    host = RATSPEAK_HUB_HOST;
                if (!assignConfigString(ep.host, host)) return memoryFailure();
                ep.port = port;
                ep.autoConnect = obj["auto"] | true;
                if (!ep.host.isEmpty()) parsed.tcpConnections.push_back(std::move(ep));
            }
        }

        parsed.screenDimTimeout = constrain(doc["screen_dim"] | 30, 5, 3600);
        parsed.screenOffTimeout = constrain(doc["screen_off"] | 60, 10, 7200);
        // Brightness: stored as 1-100%. Migrate old 0-255 values.
        int rawBri = doc["brightness"] | BOARD_DEFAULT_BRIGHTNESS;
        if (rawBri > 100) rawBri = std::min(rawBri, 255) * 100 / 255;  // Migrate from PWM to percentage
        parsed.brightness = constrain(rawBri, 1, 100);
        parsed.denseFontMode    = doc["dense_font"] | false;
        parsed.themeLight       = doc["theme_light"] | false;
        parsed.keyboardBrightness = constrain(doc["kb_brightness"] | 0, 0, 100);
        parsed.keyboardAutoOn     = doc["kb_auto_on"] | false;
        parsed.keyboardAutoOff    = doc["kb_auto_off"] | false;
        parsed.trackballSpeed   = constrain(doc["trackball_speed"] | 3, 1, 5);
        parsed.touchSensitivity = constrain(doc["touch_sens"] | 3, 1, 5);
        parsed.bleEnabled       = false;

        // battery settings
        parsed.batteryDisplay = constrain(doc["batt_display"] | (int)BATTERY_DISPLAY_BAR, (int)BATTERY_DISPLAY_PERCENT, (int)BATTERY_DISPLAY_BAR);
        parsed.batteryModel = constrain(doc["batt_model"] | (int)BATTERY_MODEL_LIPO, (int)BATTERY_MODEL_LIPO, (int)BATTERY_MODEL_LINEAR);
        parsed.chargeThresholdV = doc["charge_thresh_v"] | BATTERY_CHARGE_THRESHOLD_DEFAULT;
        parsed.fullBatteryV = doc["full_battery_v"] | BATTERY_FULL_VOLTAGE_DEFAULT;

        parsed.gpsTimeEnabled     = doc["gps_time"]     | true;
    #if LEGACY_GPS_LOC_MIGRATION
        parsed.gpsLocationEnabled = doc["gps_location"].isNull()
            ? (doc["gps_loc"] | false)
            : (doc["gps_location"] | false);
    #else
        parsed.gpsLocationEnabled = doc["gps_location"] | false;
    #endif
        const int timezoneIdx = doc["tz_idx"] | 6;
        parsed.timezoneIdx = timezoneIdx >= 0 && timezoneIdx < 21 ? timezoneIdx : 6;
        parsed.timezoneSet        = doc["tz_set"]       | false;
        parsed.use24HourTime      = doc["time_24h"]     | false;

        parsed.audioEnabled = doc["audio_on"]  | true;
        parsed.audioVolume  = constrain(doc["audio_vol"] | 80, 0, 100);

        if (!assignConfigString(parsed.displayName, doc["display_name"] | "")) return memoryFailure();
        parsed.nameComplete = doc["name_complete"] | !parsed.displayName.isEmpty();
        parsed.sdStorageEnabled = doc["sd_storage"] | parsed.sdStorageEnabled;
        parsed.announceInterval = handheld::announce::normalizeMinutes(doc["announce_int"] | 30);
        parsed.devMode     = doc["dev_mode"]     | false;

        sanitizeSettings(parsed);
        if (!handheld::config::Memory::admits(0)) return memoryFailure();
        // Swapping whole values also releases old String capacities. Arduino's
        // move assignment can otherwise reuse a much larger destination buffer.
        std::swap(_settings, parsed);
        if (persisted) {
            _nameBound = nameBound; _namePending = namePending;
            std::memcpy(_nameIdentity, nameIdentity, sizeof _nameIdentity);
        }
        Serial.println("[CONFIG] Settings loaded");
        return true;
    } catch (const std::bad_alloc&) { return memoryFailure(); }
}

String UserConfig::serializeToJson(bool persisted, size_t limit, bool* unavailable) {
    if (unavailable) *unavailable = false;
    const auto memoryFailure = [unavailable]() {
        if (unavailable) *unavailable = true;
        return String();
    };
    sanitizeSettings(_settings);
    handheld::config::JsonAllocator allocator(JsonAllocationLimit);
    JsonDocument doc(&allocator);

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
    doc["name_complete"] = _settings.nameComplete;
    if (persisted && _nameBound) {
        static constexpr char digits[] = "0123456789abcdef";
        char hex[33]{};
        for (size_t i = 0; i < 16; ++i) { hex[2*i] = digits[_nameIdentity[i] >> 4]; hex[2*i+1] = digits[_nameIdentity[i] & 15]; }
        doc["name_identity"] = hex;
        doc["name_pending"] = _namePending;
    }
    doc["sd_storage"] = _settings.sdStorageEnabled;
    doc["announce_int"] = _settings.announceInterval;
    doc["dev_mode"]     = _settings.devMode;

    String json;
    if (doc.overflowed()) return memoryFailure();
    const size_t length = measureJson(doc);
    if (!length || length > limit) return "";
    if (!handheld::config::Memory::reserveString(json, length)) return memoryFailure();
    if (serializeJson(doc, json) != length) return memoryFailure();
    return json;
}

bool UserConfig::load(FlashStore& flash) {
    _recoveryRequired = false;
    String json;
    const auto source = flash.readRecord(PATH_USER_CONFIG, json, StoredLimit,
                                        handheld::config::Memory::reserveString);
    if (source == FlashStore::RecordSource::Primary || source == FlashStore::RecordSource::Backup) {
        bool unavailable = false;
        if (parseJson(json, true, &unavailable)) {
            _source = source == FlashStore::RecordSource::Primary ? Source::Flash : Source::FlashBackup;
#if USERCONFIG_NVS_BACKUP
            _nvsMirrorPending = !_namePending;
#endif
            return true;
        }
        _source = unavailable ? Source::Unavailable : Source::Invalid;
        _recoveryRequired = true; return false;
    }
    if (source != FlashStore::RecordSource::Absent) {
        _source = source == FlashStore::RecordSource::Invalid ? Source::Invalid : Source::Unavailable;
        _recoveryRequired = true; return false;
    }
#if USERCONFIG_NVS_BACKUP
    bool available = false, present = false;
    size_t length = 0;
    const auto bytes = loadFromNVS(available, present, length);
    if (!available) { _source = Source::Unavailable; _recoveryRequired = true; return false; }
    if (present) {
        bool unavailable = false;
        if (parseJson(bytes.get(), length, true, &unavailable)) { _source = Source::Nvs; return true; }
        _source = unavailable ? Source::Unavailable : Source::Invalid;
        _recoveryRequired = true; return false;
    }
#endif
    _source = Source::Absent;
    return false;
}

bool UserConfig::saveRequired(FlashStore& flash) {
    return saveRequired(flash, serializeToJson());
}

bool UserConfig::saveRequired(FlashStore& flash, const String& json) {
    if (json.isEmpty() || _recoveryRequired || !flash.writeString(PATH_USER_CONFIG, json)) return false;
    _source = Source::Flash;
#if USERCONFIG_NVS_BACKUP
    if (!_namePending) _nvsMirrorPending = !saveToNVS(json);
#endif
    return true;
}

bool UserConfig::save(FlashStore& flash) {
    return !_namePending && saveRequired(flash);
}

bool UserConfig::load(SDStore& sd, FlashStore& flash) {
    if (load(flash)) {
        // A stale/removable mirror must never override a committed flash value.
        _mirrorPending = _settings.sdStorageEnabled;
        return true;
    }
    if (!flash.isReady() || _recoveryRequired) return false;
    String backup;
    if (sd.isReady()) {
        const bool primary = sd.exists(SD_PATH_USER_CONFIG);
        const char* path = primary ? SD_PATH_USER_CONFIG : SD_PATH_USER_CONFIG ".bak";
        if (primary || sd.exists(path)) {
            // The lower SD tier follows the same strict provenance rule. Its
            // File proxy retains the existing per-call shared-SPI protection.
            File file = sd.openFile(path);
            if (!file) { _source = Source::Unavailable; _recoveryRequired = true; return false; }
            const size_t size = file.size();
            if (!size || size > StoredLimit || file.isDirectory()) {
                file.close(); _source = Source::Invalid; _recoveryRequired = true; return false;
            }
            if (!handheld::config::Memory::reserveString(backup, size)) {
                file.close(); _source = Source::Unavailable; _recoveryRequired = true; return false;
            }
            char chunk[512];
            for (size_t offset = 0; offset < size; offset += sizeof chunk) {
                const size_t count = std::min(sizeof chunk, size - offset);
                if (file.readBytes(chunk, count) != count || !backup.concat(chunk, count)) {
                    file.close(); _source = Source::Unavailable; _recoveryRequired = true; return false;
                }
            }
            file.close();
        }
    }
    bool unavailable = false;
    if (!backup.isEmpty() && parseJson(backup, true, &unavailable)) {
        _settings.sdStorageEnabled = true;
        _recoveryRequired = false;
        if (save(flash)) {
            _mirrorPending = true;
            _source = Source::Sd;
            Serial.println("[CONFIG] Restored settings from SD backup");
            return true;
        }
        // The SD record was valid; canonical write/admission failure must not
        // masquerade as fresh absence and permit defaults during recovery.
        _source = Source::Unavailable;
        _recoveryRequired = true;
        return false;
    }
    _recoveryRequired = sd.isReady() &&
        (sd.exists(SD_PATH_USER_CONFIG) || sd.exists(SD_PATH_USER_CONFIG ".bak"));
    if (_recoveryRequired) _source = unavailable ? Source::Unavailable : Source::Invalid;
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
    if (_namePending || _recoveryRequired) return false;
    if (!_settings.sdStorageEnabled) _mirrorPending = false;
    if (!_mirrorPending && !_nvsMirrorPending) return true;
    const String json = serializeToJson();
    if (json.isEmpty()) return false;
#if USERCONFIG_NVS_BACKUP
    if (_nvsMirrorPending) _nvsMirrorPending = !saveToNVS(json);
#endif
    // Optional absent media cannot prevent a safe shutdown: flash is durable.
    if (_mirrorPending && sd.isReady()) {
        bool refused = false;
        bool matches = false;
        {
            const String previous = sd.readString(SD_PATH_USER_CONFIG, handheld::config::Memory::reserveString, &refused);
            matches = previous == json;
        }
        if (refused) return false;
        if (matches) _mirrorPending = false;
        else if (sd.ensureDir(SD_PATH_ROOT) && sd.ensureDir(SD_PATH_CONFIG_DIR) &&
                 sd.writeString(SD_PATH_USER_CONFIG, json)) _mirrorPending = false;
    }
    return !_nvsMirrorPending && (!_mirrorPending || !sd.isReady());
}
