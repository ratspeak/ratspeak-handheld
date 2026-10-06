#include "WiFiInterface.h"
#include "config/Config.h"
#include "config/NetworkTiming.h"

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
    _apPassword = WIFI_AP_PASSWORD;
    _txBuffer = (uint8_t*)ps_malloc(TX_BUFFER_SIZE);
    if (!_txBuffer) _txBuffer = (uint8_t*)malloc(TX_BUFFER_SIZE);
}

WiFiInterface::~WiFiInterface() {
    stop();
    if (_txBuffer) { free(_txBuffer); _txBuffer = nullptr; }
}

void WiFiInterface::setAPCredentials(const char* ssid, const char* password) {
    _apSSID = ssid;
    _apPassword = password;
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
    // Generate SSID from chip ID if not set
    if (_apSSID.isEmpty()) {
        uint32_t chip = ESP.getEfuseMac() & 0xFFFF;
        char ssid[32];
        snprintf(ssid, sizeof(ssid), DEVICE_AP_PREFIX "-%04x", chip);
        _apSSID = ssid;
    }

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

std::vector<WiFiInterface::ScanResult> WiFiInterface::scanNetworks(int maxResults) {
    std::vector<ScanResult> results;
    wifi_mode_t prevMode = WiFi.getMode();
    bool wasConnected = (WiFi.status() == WL_CONNECTED);
    String prevSSID, prevPass;

    Serial.printf("[WIFI] Scan: prevMode=%d connected=%d\n", (int)prevMode, wasConnected);

    // Ensure we're in a mode that supports scanning
    if (prevMode == WIFI_OFF || prevMode == WIFI_AP) {
        WiFi.mode(WIFI_AP_STA);
    }
    // Disconnect from any active STA connection to free the radio for scanning
    WiFi.disconnect(false);
    delay(300);

    // Delete any previous scan results
    WiFi.scanDelete();

    Serial.println("[WIFI] Starting network scan...");

    // Synchronous scan — blocks until complete (typically 2-5 seconds)
    int n = WiFi.scanNetworks(false, false, false, 300, 0);

    Serial.printf("[WIFI] Scan result: %d\n", n);

    // If synchronous returned -2 (still running), poll for it
    if (n == WIFI_SCAN_RUNNING) {
        unsigned long t0 = millis();
        while (n == WIFI_SCAN_RUNNING && millis() - t0 < 15000) {
            delay(200);
            n = WiFi.scanComplete();
        }
        Serial.printf("[WIFI] Scan poll result: %d\n", n);
    }

    if (n >= 0) results = getScanResults(maxResults);
    WiFi.scanDelete();

    // Restore previous WiFi mode
    if (prevMode == WIFI_OFF) {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
    } else if (prevMode == WIFI_AP) {
        WiFi.mode(WIFI_AP);
    }
    // Note: if was STA/AP_STA, the reconnect will happen via main loop's STA handler
    return results;
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
    Serial.printf("[WIFI] Async scan %s (status=%d mode=%d heap=%lu largest=%lu)\n",
                  started ? "started" : "rejected", result, (int)scanMode,
                  (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMaxAllocHeap());
    return started;
}

bool WiFiInterface::isScanComplete() {
    int result = WiFi.scanComplete();
    return result != WIFI_SCAN_RUNNING;
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
        String ssid = WiFi.SSID(i);
        if (ssid.isEmpty()) continue;
        const int rssi = WiFi.RSSI(i);
        const bool encrypted = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        auto same = std::find_if(results.begin(), results.end(),
            [&](const ScanResult& row) { return row.ssid == ssid; });
        if (same != results.end()) {
            if (rssi > same->rssi) { same->rssi = rssi; same->encrypted = encrypted; }
        } else if (results.size() < capacity) {
            results.push_back({ssid, rssi, encrypted});
        } else {
            auto weakest = std::min_element(results.begin(), results.end(),
                [](const ScanResult& a, const ScanResult& b) { return a.rssi < b.rssi; });
            if (rssi > weakest->rssi) *weakest = {ssid, rssi, encrypted};
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
