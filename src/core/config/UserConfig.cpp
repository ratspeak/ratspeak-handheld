#include "UserConfig.h"
#include "ConfigJsonWriter.h"
#include "config/AnnounceInterval.h"
#include "config/BoardConfig.h"
#include "radio/RadioFrequency.h"
#include "radio/RadioBandwidth.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#if USERCONFIG_NVS_LEGACY_READ
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

#if USERCONFIG_NVS_LEGACY_READ
// Read-only compatibility for settings written by older firmware.
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
    if (!configCopyFits(source._settings, charge)) return false;
    const auto memoryFailure = [charge]() {
        Serial.printf("[CONFIG] Copy memory unavailable: free=%u largest=%u charge=%u\n",
            unsigned(ESP.getFreeHeap()), unsigned(ESP.getMaxAllocHeap()), unsigned(charge));
        return false;
    };
    if (!handheld::config::Memory::admits(charge)) return memoryFailure();
    try {
        // Compiler-generated scalar/member copying stays the single value map.
        // Verify the SDK's nonthrowing String failures before swapping values.
        UserSettings prepared = source._settings;
        if (!sameConfigStrings(prepared, source._settings) || !handheld::config::Memory::admits(0)) return memoryFailure();
        std::swap(_settings, prepared);
        copyStateFrom(source);
        return true;
    } catch (const std::bad_alloc&) { return memoryFailure(); }
}

void UserConfig::copyStateFrom(const UserConfig& source) noexcept {
    std::memcpy(_nameIdentity, source._nameIdentity, sizeof _nameIdentity);
    _nameBound = source._nameBound; _namePending = source._namePending;
    _source = source._source; _recoveryRequired = source._recoveryRequired;
}

void UserConfig::swap(UserConfig& other) noexcept {
    // Whole-value moves empty their source first. Pinned WString moves and
    // std::allocator vector moves allocate nothing and release old capacities.
    std::swap(_settings, other._settings);
    std::swap(_nameIdentity, other._nameIdentity);
    std::swap(_nameBound, other._nameBound); std::swap(_namePending, other._namePending);
    std::swap(_source, other._source);
    std::swap(_recoveryRequired, other._recoveryRequired);
}

void UserConfig::sanitizeScalars(UserScalars& settings) {
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
}

void UserConfig::sanitizeSettings(UserSettings& settings) {
    sanitizeScalars(settings);
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

        // Absent fields migrate to OFF/AUTO/AUTO. Invalid new policy values
        // refuse the whole candidate instead of silently changing delivery mode.
        if ((!doc["prop_on"].isNull() && !doc["prop_on"].is<bool>()) ||
            (!doc["prop_select"].isNull() && !doc["prop_select"].is<uint8_t>()) ||
            (!doc["prop_delivery"].isNull() && !doc["prop_delivery"].is<uint8_t>()) ||
            (!doc["prop_node"].isNull() && !doc["prop_node"].is<const char*>())) return false;
        parsed.voice.enabled=doc["voice_on"] | false;
        parsed.voice.contactsOnly=doc["voice_contacts"] | true;
        const int voiceVolume=doc["voice_volume"] | 70;
        const int voiceRoute=doc["voice_route"] | 0;
        if(voiceVolume<0 || voiceVolume>100 || voiceRoute<0 || voiceRoute>2) return false;
        if(!doc["voice_max_messages"].isNull() && !doc["voice_max_messages"].is<uint8_t>()) return false;
        const unsigned voiceMax=doc["voice_max_messages"] | 2u;
        if(voiceMax<1 || voiceMax>50) return false;
        parsed.voice.maxMessages=uint8_t(voiceMax);
        parsed.voice.volume=static_cast<uint8_t>(voiceVolume);
        parsed.voice.route=static_cast<handheld::voice::Route>(voiceRoute);
        parsed.propagation.enabled = doc["prop_on"] | false;
        parsed.propagation.selection = static_cast<handheld::propagation::Selection>(doc["prop_select"] | 0);
        parsed.propagation.delivery = static_cast<handheld::propagation::Delivery>(doc["prop_delivery"] | 0);
        const JsonString node = doc["prop_node"].as<JsonString>();
        if (!parsed.propagation.valid() ||
            !parsed.propagation.setManual(node.c_str(), node.size())) return false;

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

void UserConfig::writeJson(handheld::config::JsonWriter& writer, bool persisted, const UserScalars& scalars) const {
    writer.object();
    writer.member("radio_region", scalars.radioRegion);
    writer.member("lora_freq", scalars.loraFrequency);
    writer.member("lora_sf", scalars.loraSF);
    writer.member("lora_bw", scalars.loraBW);
    writer.member("lora_cr", scalars.loraCR);
    writer.member("lora_txp", scalars.loraTxPower);
    writer.member("lora_pre", scalars.loraPreamble);
    writer.member("lora_on", scalars.loraEnabled);

    writer.member("wifi_mode", (int)scalars.wifiMode);
    writer.member("wifi_restore_mode", (int)scalars.wifiRestoreMode);
    writer.member("wifi_ap_ssid", _settings.wifiAPSSID);
    writer.member("wifi_ap_pass", _settings.wifiAPPassword);
    writer.member("wifi_sta_selected", (int)constrain((int)scalars.wifiSTASelected, 0, (int)WIFI_STA_MAX_NETWORKS - 1));
    writer.array("wifi_sta_networks");
    for (size_t slot = 0; slot < WIFI_STA_MAX_NETWORKS; slot++) {
        writer.object();
        if (slot < _settings.wifiSTANetworks.size()) {
            writer.member("ssid", _settings.wifiSTANetworks[slot].ssid);
            writer.member("pass", _settings.wifiSTANetworks[slot].password);
        } else {
            writer.member("ssid", "");
            writer.member("pass", "");
        }
        writer.endObject();
    }

    writer.endArray();

    writer.member("autoiface_en", scalars.autoIfaceEnabled);
    writer.member("autoiface_group", _settings.autoIfaceGroupId);
    writer.member("autoiface_max", scalars.autoIfaceMaxPeers);

    writer.array("tcp_connections");
    for (auto& ep : _settings.tcpConnections) {
        writer.object();
        writer.member("host", ep.host);
        writer.member("port", ep.port);
        writer.member("auto", ep.autoConnect);
        writer.endObject();
    }

    writer.endArray();

    writer.member("screen_dim", scalars.screenDimTimeout);
    writer.member("screen_off", scalars.screenOffTimeout);
    writer.member("brightness", scalars.brightness);
    writer.member("dense_font", scalars.denseFontMode);
    writer.member("theme_light", scalars.themeLight);
    writer.member("kb_brightness", scalars.keyboardBrightness);
    writer.member("kb_auto_on", scalars.keyboardAutoOn);
    writer.member("kb_auto_off", scalars.keyboardAutoOff);
    writer.member("trackball_speed", scalars.trackballSpeed);
    writer.member("touch_sens", scalars.touchSensitivity);
    writer.member("ble_enabled", false);

    // battery settings
    writer.member("batt_display", scalars.batteryDisplay);
    writer.member("batt_model", scalars.batteryModel);
    writer.member("charge_thresh_v", scalars.chargeThresholdV);
    writer.member("full_battery_v", scalars.fullBatteryV);

    writer.member("gps_time", scalars.gpsTimeEnabled);
    writer.member("gps_location", scalars.gpsLocationEnabled);
    writer.member("tz_idx", scalars.timezoneIdx);
    writer.member("tz_set", scalars.timezoneSet);
    writer.member("time_24h", scalars.use24HourTime);

    writer.member("audio_on", scalars.audioEnabled);
    writer.member("audio_vol", scalars.audioVolume);

    writer.member("display_name", _settings.displayName);
    writer.member("name_complete", scalars.nameComplete);
    if (persisted && _nameBound) {
        static constexpr char digits[] = "0123456789abcdef";
        char hex[33]{};
        for (size_t i = 0; i < 16; ++i) { hex[2*i] = digits[_nameIdentity[i] >> 4]; hex[2*i+1] = digits[_nameIdentity[i] & 15]; }
        writer.member("name_identity", hex);
        writer.member("name_pending", _namePending);
    }
    writer.member("sd_storage", scalars.sdStorageEnabled);
    writer.member("announce_int", scalars.announceInterval);
    writer.member("dev_mode", scalars.devMode);
    writer.member("voice_on", scalars.voice.enabled);
    writer.member("voice_contacts", scalars.voice.contactsOnly);
    writer.member("voice_volume", scalars.voice.volume);
    writer.member("voice_max_messages", scalars.voice.maxMessages);
    writer.member("voice_route", uint8_t(scalars.voice.route));
    writer.member("prop_on", scalars.propagation.enabled);
    writer.member("prop_select", static_cast<uint8_t>(scalars.propagation.selection));
    writer.member("prop_delivery", static_cast<uint8_t>(scalars.propagation.delivery));
    char propagationNode[33]{};
    scalars.propagation.manualHex(propagationNode);
    writer.member("prop_node", propagationNode);
    writer.endObject();
}

UserConfig::SaveResult UserConfig::prepareJson(bool persisted, size_t limit, size_t& length) {
    sanitizeSettings(_settings);
    return measureJson(persisted, limit, _settings, length);
}

UserConfig::SaveResult UserConfig::measureJson(bool persisted, size_t limit, const UserScalars& scalars,
                                               size_t& length) const {
    length = 0;
    if (!scalars.propagation.valid() || scalars.voice.volume > 100 ||
        uint8_t(scalars.voice.route) > 2 || scalars.voice.maxMessages < 1 ||
        scalars.voice.maxMessages > 50) return SaveResult::Invalid;
    if (!handheld::config::Memory::admits(0)) return SaveResult::NoMemory;
    handheld::config::JsonWriter measured(limit);
    writeJson(measured, persisted, scalars);
    if (measured.unavailable() || !handheld::config::Memory::admits(0)) return SaveResult::NoMemory;
    if (!measured.ok() || !measured.size()) return SaveResult::Invalid;
    length = measured.size();
    return SaveResult::Complete;
}

String UserConfig::serializeToJson(bool persisted, size_t limit, bool* unavailable) {
    if (unavailable) *unavailable = false;
    const auto memoryFailure = [unavailable]() {
        if (unavailable) *unavailable = true;
        Serial.printf("[CONFIG] Encode memory unavailable: free=%u largest=%u\n",
            unsigned(ESP.getFreeHeap()), unsigned(ESP.getMaxAllocHeap()));
        return String();
    };
    size_t length = 0;
    const auto prepared = prepareJson(persisted, limit, length);
    if (prepared == SaveResult::NoMemory) return memoryFailure();
    if (prepared != SaveResult::Complete) return "";
    String json;
    if (!handheld::config::Memory::reserveString(json, length)) return memoryFailure();
    handheld::config::JsonWriter output(length, &json);
    writeJson(output, persisted, _settings);
    if (output.unavailable()) return memoryFailure();
    if (!output.ok() || output.size() != length || json.length() != length) return "";
    return json;
}

// The caller sanitizes and measures first. This synchronous source borrows
// those fixed values through write and readback; neither emission mutates
// settings or retains a complete encoded record.
class UserConfig::JsonSource final : public handheld::storage::AtomicSource {
public:
    JsonSource(const UserConfig& config, const UserScalars& scalars, size_t length)
        : _config(config), _scalars(scalars), _length(length) {}
    size_t length() const override { return _length; }
    bool unavailable() const { return _unavailable; }
    bool emit(handheld::storage::ByteSink& sink) const override {
        if (!handheld::config::Memory::admits(0)) { _unavailable = true; return false; }
        handheld::config::JsonWriter writer(_length, sink);
        _config.writeJson(writer, true, _scalars);
        _unavailable = _unavailable || writer.unavailable() || !handheld::config::Memory::admits(0);
        return !_unavailable && writer.ok() && writer.size() == _length;
    }
private:
    const UserConfig& _config;
    const UserScalars& _scalars;
    size_t _length;
    mutable bool _unavailable = false;
};

bool UserConfig::load(FlashStore& flash) {
    _recoveryRequired = false;
    String json;
    const auto source = flash.readRecord(PATH_USER_CONFIG, json, StoredLimit,
                                        handheld::config::Memory::reserveString);
    if (source == FlashStore::RecordSource::Primary || source == FlashStore::RecordSource::Backup) {
        bool unavailable = false;
        if (parseJson(json, true, &unavailable)) {
            _source = source == FlashStore::RecordSource::Primary ? Source::Flash : Source::FlashBackup;
            return true;
        }
        _source = unavailable ? Source::Unavailable : Source::Invalid;
        _recoveryRequired = true; return false;
    }
    if (source != FlashStore::RecordSource::Absent) {
        _source = source == FlashStore::RecordSource::Invalid ? Source::Invalid : Source::Unavailable;
        _recoveryRequired = true; return false;
    }
#if USERCONFIG_NVS_LEGACY_READ
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
    size_t length = 0;
    const auto prepared = prepareJson(true, StoredLimit, length);
    if (prepared == SaveResult::NoMemory) saveMemoryFailure("prepare", length);
    return prepared == SaveResult::Complete && savePrepared(flash, length) == SaveResult::Complete;
}

UserConfig::SaveResult UserConfig::saveMemoryFailure(const char* phase, size_t length) {
    Serial.printf("[CONFIG] %s memory unavailable: free=%u largest=%u bytes=%u\n",
        phase, unsigned(ESP.getFreeHeap()), unsigned(ESP.getMaxAllocHeap()), unsigned(length));
    return SaveResult::NoMemory;
}

UserConfig::SaveResult UserConfig::savePrepared(FlashStore& flash, size_t length, const UserScalars& scalars) {
    if (!length || length > StoredLimit || _recoveryRequired) return SaveResult::Invalid;
    if (!handheld::config::Memory::admits(0)) return saveMemoryFailure("write", length);
    const JsonSource source(*this, scalars, length);
    const auto result = flash.writeAtomic(PATH_USER_CONFIG, source);
    if (source.unavailable()) return saveMemoryFailure("write", length);
    if (result != handheld::storage::Error::None) return SaveResult::StorageError;
    _source = Source::Flash;
    return SaveResult::Complete;
}

bool UserConfig::save(FlashStore& flash) {
    return !_namePending && saveRequired(flash);
}

bool UserConfig::load(SDStore& sd, FlashStore& flash) {
    // Legacy settings must never override a committed flash value.
    if (load(flash)) return true;
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
            _source = Source::Sd;
            Serial.println("[CONFIG] Imported legacy SD settings");
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

bool UserConfig::save(SDStore&, FlashStore& flash) {
    return save(flash);
}
