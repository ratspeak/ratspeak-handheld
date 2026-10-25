#include "WiFiInterface.h"
#include "WiFiScanSupport.h"
#include "config/Config.h"
#include "config/ConfigMemory.h"
#include "config/NetworkTiming.h"
#include <esp_wifi.h>
#include <cstring>
#include <utility>

namespace {
// The pinned Arduino 2.0.x API exposes its shared scan deadline to subclasses.
// Override it after scanNetworks sets dwell*20, without changing RF dwell time,
// SDK headers, result ownership, or the boot-lifetime scan callback.
struct ScanDeadline : WiFiScanClass {
    static void apply() { _scanTimeout = handheld::network_timing::WifiScanMs; }
};
}

WiFiInterface::WiFiInterface(const char* name)
    : _name(name ? name : "WiFiInterface"), _server(WIFI_AP_PORT)
{
    _txBuffer = (uint8_t*)ps_malloc(TX_BUFFER_SIZE);
    if (!_txBuffer) _txBuffer = (uint8_t*)malloc(TX_BUFFER_SIZE);
}

WiFiInterface::~WiFiInterface() {
    stop();
    if (_txBuffer) { free(_txBuffer); _txBuffer = nullptr; }
}

bool WiFiInterface::prepareAPCredentials(const char* ssid, const char* password, APCredentials& prepared) {
    if (!ssid || !password) return false;
    char generated[32];
    if (!*ssid) {
        snprintf(generated, sizeof(generated), DEVICE_AP_PREFIX "-%04x",
                 unsigned(ESP.getEfuseMac() & 0xFFFF));
        ssid = generated;
    }
    const size_t nameLength = strlen(ssid), passwordLength = strlen(password);
    if (nameLength > 32 || passwordLength > 64) return false;
    APCredentials candidate;
    using handheld::config::Memory;
    if (!Memory::reserveString(candidate.ssid, nameLength) || !candidate.ssid.concat(ssid, nameLength) ||
        (passwordLength && (!Memory::reserveString(candidate.password, passwordLength) ||
                            !candidate.password.concat(password, passwordLength))) || !Memory::admits(0)) return false;
    prepared = std::move(candidate);
    return true;
}

void WiFiInterface::setAPCredentials(APCredentials&& prepared) {
    _apSSID = std::move(prepared.ssid);
    _apPassword = std::move(prepared.password);
    _apConfigured = true;
}

bool WiFiInterface::setAPCredentials(const char* ssid, const char* password) {
    APCredentials prepared;
    if (!prepareAPCredentials(ssid, password, prepared)) return false;
    setAPCredentials(std::move(prepared));
    return true;
}

void WiFiInterface::setSTACredentials(const char* ssid, const char* password) {
    _staSSID = ssid;
    _staPassword = password;
}

bool WiFiInterface::isSTAConnected() const {
    return WiFi.status() == WL_CONNECTED;
}

bool WiFiInterface::startAP() {
    WiFi.persistent(false); // Credentials persist through UserConfig, not SDK NVS.
    if (!_apConfigured && !setAPCredentials("", WIFI_AP_PASSWORD)) return false;

    // AP-only mode — saves ~20KB vs WIFI_AP_STA
    if (!WiFi.mode(WIFI_AP) || !WiFi.softAP(_apSSID.c_str(), _apPassword.c_str())) {
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_OFF);
        return false;
    }

    Serial.printf("[WIFI] AP started: %s @ %s\n",
                  _apSSID.c_str(),
                  WiFi.softAPIP().toString().c_str());

    _server.begin();
    _apActive = true;
    return true;
}

bool WiFiInterface::start() {
    if (_online) return true;
    if (_generation == UINT32_MAX) return false;
    ++_generation;
    if (!_txBuffer || !startAP()) return false;
    _online = true;
    return true;
}

void WiFiInterface::stop() {
    _online = false;
    _apActive = false;
    for (auto& client : _clients) {
        client.stop();
    }
    _clients.clear();
    _clientFrames.clear();
    _server.stop();
    WiFi.softAPdisconnect(true);
}

void WiFiInterface::stopFull() {
    stop();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("[WIFI] Full shutdown");
}

void WiFiInterface::acceptClients() {
    WiFiClient newClient = _server.available();
    if (newClient) {
        if ((int)_clients.size() >= MAX_AP_CLIENTS) {
            newClient.stop();
            Serial.printf("[WIFI] Client rejected (max %d reached)\n", MAX_AP_CLIENTS);
            return;
        }
        newClient.setNoDelay(true);
        newClient.setTimeout(5);
        _clients.push_back(newClient);
        _clientFrames.push_back(FrameState{});
        Serial.printf("[WIFI] Client connected (%d total)\n", (int)_clients.size());
    }
}

void WiFiInterface::readClients() {
    for (int i = _clients.size() - 1; i >= 0; i--) {
        if (!_clients[i].connected()) {
            _clients[i].stop();
            _clients.erase(_clients.begin() + i);
            if (i < (int)_clientFrames.size()) _clientFrames.erase(_clientFrames.begin() + i);
            Serial.printf("[WIFI] Client disconnected (%d total)\n", (int)_clients.size());
            continue;
        }

        if (i >= (int)_clientFrames.size()) _clientFrames.resize(_clients.size());
        int len = readFrame(_clients[i], _clientFrames[i], _rxBuffer, sizeof(_rxBuffer));
        if (len > 0) {
            if (_rawSink) {
                Serial.printf("[WIFI] RX %d bytes from client\n", len);
                _rawSink(_rxBuffer, (size_t)len);
            }
        }
    }
}

bool WiFiInterface::sendToClients(const uint8_t* data, size_t len) {
    bool accepted = false;
    for (auto& client : _clients) {
        if (client.connected()) {
            accepted |= sendFrame(client, data, len);
        }
    }
    return accepted;
}

void WiFiInterface::loop() {
    if (!_online) return;
    acceptClients();
    readClients();
}

bool WiFiInterface::startAsyncScan() {
    const wifi_mode_t mode = WiFi.getMode();
    const wifi_mode_t scanMode = mode == WIFI_OFF ? WIFI_STA :
        mode == WIFI_AP ? WIFI_AP_STA : mode;
    if (!WiFi.mode(scanMode)) {
        Serial.printf("[WIFI] Scan mode startup failed (mode=%d heap=%lu largest=%lu)\n",
                      (int)mode, (unsigned long)ESP.getFreeHeap(),
                      (unsigned long)ESP.getMaxAllocHeap());
        return false;
    }
    WiFi.scanDelete();
    const int result = WiFi.scanNetworks(true, false, false, 300, 0);
    const bool started = result == WIFI_SCAN_RUNNING || result >= 0;
    if (started) ScanDeadline::apply();
    Serial.printf("[WIFI] Async scan %s (status=%d error=%d mode=%d heap=%lu largest=%lu)\n",
                  started ? "started" : "rejected", result, (int)handheld_wifi_scan_error(), (int)scanMode,
                  (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMaxAllocHeap());
    return started;
}

std::vector<WiFiInterface::ScanResult> WiFiInterface::getScanResults(int maxResults) {
    std::vector<ScanResult> results;
    int n = WiFi.scanComplete();
    if (n <= 0) return results;

    // Bound our copy independently of the SDK-owned scan list. Keep the
    // strongest unique names without first allocating one row per AP.
    const size_t capacity = static_cast<size_t>(std::max(0, std::min(maxResults, 15)));
    results.reserve(capacity);
    for (int i = 0; i < n && capacity; ++i) {
        const auto* raw = static_cast<const wifi_ap_record_t*>(WiFi.getScanInfoByIndex(i));
        if (!raw) throw std::bad_alloc();
        String ssid;
        const size_t length = strnlen(reinterpret_cast<const char*>(raw->ssid), sizeof(raw->ssid));
        if (!ssid.reserve(length) || !ssid.concat(reinterpret_cast<const char*>(raw->ssid), length))
            throw std::bad_alloc();
        if (ssid.isEmpty()) continue;
        const int rssi = raw->rssi;
        const bool encrypted = raw->authmode != WIFI_AUTH_OPEN;
        auto same = std::find_if(results.begin(), results.end(),
            [&](const ScanResult& row) { return row.ssid == ssid; });
        if (same != results.end()) {
            if (rssi > same->rssi) { same->rssi = rssi; same->encrypted = encrypted; }
        } else if (results.size() < capacity) {
            results.push_back({std::move(ssid), rssi, encrypted});
        } else {
            auto weakest = std::min_element(results.begin(), results.end(),
                [](const ScanResult& a, const ScanResult& b) { return a.rssi < b.rssi; });
            if (rssi > weakest->rssi) *weakest = {std::move(ssid), rssi, encrypted};
        }
    }
    std::sort(results.begin(), results.end(),
              [](const ScanResult& a, const ScanResult& b) { return a.rssi > b.rssi; });
    Serial.printf("[WIFI] Async scan: %d networks found\n", (int)results.size());
    return results;
}

// HDLC-like framing: [0x7E] [escaped data] [0x7E]
// Buffered: builds full frame then sends in a single write() call
bool WiFiInterface::sendFrame(WiFiClient& client, const uint8_t* data, size_t len) {
    if (!_txBuffer || !data || len == 0) return false;
    if (len * 2 + 2 > TX_BUFFER_SIZE) return false;
    size_t pos = 0;
    _txBuffer[pos++] = FRAME_START;
    for (size_t i = 0; i < len && pos < TX_BUFFER_SIZE - 2; i++) {
        if (data[i] == FRAME_START || data[i] == FRAME_ESC) {
            _txBuffer[pos++] = FRAME_ESC;
            if (pos < TX_BUFFER_SIZE - 1) _txBuffer[pos++] = data[i] ^ FRAME_XOR;
        } else {
            _txBuffer[pos++] = data[i];
        }
    }
    _txBuffer[pos++] = FRAME_START;
    return client.write(_txBuffer, pos) == pos;
}

int WiFiInterface::readFrame(WiFiClient& client, FrameState& state, uint8_t* buffer, size_t maxLen) {
    if (!client.available()) return 0;

    while (client.available() && state.pos < maxLen) {
        uint8_t b = client.read();

        if (b == FRAME_START) {
            if (state.inFrame && state.pos > 0) {
                int len = state.pos;
                state.pos = 0;
                state.escaped = false;
                return len;  // End of frame
            }
            state.inFrame = true;
            state.escaped = false;
            state.pos = 0;
            continue;
        }

        if (!state.inFrame) continue;

        if (b == FRAME_ESC) {
            state.escaped = true;
            continue;
        }

        if (state.escaped) {
            buffer[state.pos++] = b ^ FRAME_XOR;
            state.escaped = false;
        } else {
            buffer[state.pos++] = b;
        }
    }

    if (state.pos >= maxLen) {
        state.inFrame = false;
        state.escaped = false;
        state.pos = 0;
    }

    return 0;  // Incomplete frame
}
