#include "TCPClientInterface.h"
#include "config/Config.h"

#include <WiFi.h>
#include <algorithm>

#if TCP_SHARED_BUFFERS
uint8_t* TCPClientInterface::_rxBuffer = nullptr;
uint8_t* TCPClientInterface::_txBuffer = nullptr;
uint8_t* TCPClientInterface::_wrapBuffer = nullptr;
bool TCPClientInterface::_buffersAllocated = false;
#endif

TCPClientInterface::TCPClientInterface(const char* host, uint16_t port, const char* name)
    : _name(name ? name : "TCPClient"), _host(host), _port(port)
{
#if TCP_SHARED_BUFFERS
    // Allocate shared buffers once — all TCP connections share them
    if (!_buffersAllocated) {
        _rxBuffer = (uint8_t*)malloc(RX_BUFFER_SIZE);
        _txBuffer = (uint8_t*)malloc(TX_BUFFER_SIZE);
        _wrapBuffer = (uint8_t*)malloc(RX_BUFFER_SIZE);
        if (!_rxBuffer || !_txBuffer || !_wrapBuffer) {
            Serial.println("[TCP] FATAL: buffer allocation failed — interface disabled");
            _online = false;
        }
        _buffersAllocated = true;
    }
#else
    _rxBuffer = (uint8_t*)ps_malloc(RX_BUFFER_SIZE);
    if (!_rxBuffer) _rxBuffer = (uint8_t*)malloc(RX_BUFFER_SIZE);
    _txBuffer = (uint8_t*)ps_malloc(TX_BUFFER_SIZE);
    if (!_txBuffer) _txBuffer = (uint8_t*)malloc(TX_BUFFER_SIZE);
    _wrapBuffer = (uint8_t*)ps_malloc(RX_BUFFER_SIZE);
    if (!_wrapBuffer) _wrapBuffer = (uint8_t*)malloc(RX_BUFFER_SIZE);
#endif
}

TCPClientInterface::~TCPClientInterface() {
    stop();
    if (_connectState == CS_CONNECTING) {
        Serial.printf("[TCP] Waiting for connect task before destroy: %s:%d\n",
                      _host.c_str(), _port);
        waitForConnectTask();
    }
    if (_client.connected()) _client.stop();
#if !TCP_SHARED_BUFFERS
    if (_rxBuffer) { free(_rxBuffer); _rxBuffer = nullptr; }
    if (_txBuffer) { free(_txBuffer); _txBuffer = nullptr; }
    if (_wrapBuffer) { free(_wrapBuffer); _wrapBuffer = nullptr; }
#endif
    // Shared buffers persist for the lifetime of the device
}

bool TCPClientInterface::start() {
    _online = true;
    tryConnect();
    return true;
}

void TCPClientInterface::stop() {
    _online = false;
    if (_connectState == CS_CONNECTING) {
        // The retiring owner polls canDestroy(); never block its timers while
        // the SDK connect helper owns the socket.
        Serial.printf("[TCP] Stop deferred while connect task exits for %s:%d\n",
                      _host.c_str(), _port);
        return;
    } else {
        _connectTask = nullptr;
    }
    if (_client.connected()) {
        _client.stop();
        Serial.printf("[TCP] Disconnected from %s:%d\n", _host.c_str(), _port);
    }
}

void TCPClientInterface::waitForConnectTask() {
    while (_connectState == CS_CONNECTING) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    _connectTask = nullptr;
}

void TCPClientInterface::connectTaskFn(void* arg) {
    auto* self = static_cast<TCPClientInterface*>(arg);
    bool ok = self->_client.connect(self->_host.c_str(), self->_port, TCP_CONNECT_TIMEOUT_MS);
    if (ok && !self->_online) {
        self->_client.stop();
        ok = false;
    }
    self->_connectState = ok ? CS_CONNECTED : CS_FAILED;
    vTaskDelete(nullptr);
}

void TCPClientInterface::tryConnect() {
    if (_connectState == CS_CONNECTING) return;
    _lastAttempt = millis();
    _connectState = CS_CONNECTING;
    Serial.printf("[TCP] Connecting to %s:%d (async)...\n", _host.c_str(), _port);
    BaseType_t ok = xTaskCreate(connectTaskFn, "tcpconn", 4096, this, 1, &_connectTask);
    if (ok != pdPASS) {
        _connectState = CS_IDLE;
        _connectTask = nullptr;
        _reconnectBackoff = std::min(_reconnectBackoff * 2, (unsigned long)300000);
        Serial.printf("[TCP] Failed to spawn connect task for %s:%d\n", _host.c_str(), _port);
    }
}

void TCPClientInterface::loop() {
    if (!_online) return;

    if (_connectState == CS_CONNECTED) {
        _connectState = CS_IDLE;
        _connectTask = nullptr;
        // Reset HDLC frame state and hub discovery for new connection
        _inFrame = false;
        _escaped = false;
        _rxPos = 0;
        _hubTransportIdKnown = false;
        _lastRxTime = millis();
        _reconnectBackoff = 1000;  // Reset backoff on success

        // Set TCP write timeout to prevent blocking on half-open sockets
        _client.setTimeout(5);  // 5 ms write/read timeout
        _client.setNoDelay(true);  // Disable Nagle — send immediately

        Serial.printf("[TCP] Connected to %s:%d\n", _host.c_str(), _port);
    } else if (_connectState == CS_FAILED) {
        _connectState = CS_IDLE;
        _connectTask = nullptr;
        // Exponential backoff: 1s → 2s → 4s → ... → 5min max, with jitter
        _reconnectBackoff = std::min(_reconnectBackoff * 2, (unsigned long)300000);
        _reconnectBackoff += random(_reconnectBackoff / 5);  // +0-20% jitter
        Serial.printf("[TCP] Failed to connect to %s:%d (next retry in %lus)\n",
                      _host.c_str(), _port, _reconnectBackoff / 1000);
    }

    // WiFiClient is not safe to use from the loop while connectTaskFn owns it.
    if (_connectState == CS_CONNECTING) return;

    // Auto-reconnect with exponential backoff (only if WiFi is connected)
    if (!_client.connected()) {
        if (WiFi.status() != WL_CONNECTED) return;
        if (millis() - _lastAttempt >= _reconnectBackoff) {
            tryConnect();
        }
        return;
    }

    // Keepalive: if no RX for 5 minutes, force reconnect (NAT timeout detection)
    if (_lastRxTime > 0 && millis() - _lastRxTime >= TCP_KEEPALIVE_TIMEOUT_MS) {
        Serial.printf("[TCP] No RX for %lus, forcing reconnect to %s:%d\n",
                      (millis() - _lastRxTime) / 1000, _host.c_str(), _port);
        _client.stop();
        _inFrame = false;
        _escaped = false;
        _rxPos = 0;
        return;  // Will reconnect on next loop iteration
    }

    // Drain incoming frames per loop (up to 15, time-boxed)
    unsigned long tcpStart = millis();
    for (int i = 0; i < 15 && _client.available() && (millis() - tcpStart < TCP_LOOP_BUDGET_MS); i++) {
        unsigned long rxStart = millis();
        int len = readFrame();
        if (len > 0) {
            _lastRxTime = millis();
            _hubRxCount++;

            // Learn hub transport_id from incoming Header2 packets (once per connection)
            if (len >= 35) {
                uint8_t flags = _rxBuffer[0];
                uint8_t header_type = (flags >> 6) & 0x01;
                if (header_type == 1 && !_hubTransportIdKnown) {
                    memcpy(_hubTransportId, _rxBuffer + 2, 16);
                    _hubTransportIdKnown = true;
                    char hex[33];
                    for (int j = 0; j < 16; j++) snprintf(hex + j*2, sizeof(hex) - j*2, "%02x", _hubTransportId[j]);
                    Serial.printf("[TCP] Learned hub transport_id: %.8s\n", hex);
                }
            }

            Serial.printf("[TCP] RX %d bytes from %s:%d (%lums)\n",
                          len, _host.c_str(), _port, millis() - rxStart);
            if (_rawSink) {
                _rawSink(_rxBuffer, (size_t)len);
            }
        } else {
            break;  // Incomplete frame, wait for more data
        }
    }
}

bool TCPClientInterface::send_outgoing(const uint8_t* rawData, size_t rawLen) {
    struct View { const uint8_t* p; size_t n; const uint8_t* data() const { return p; } size_t size() const { return n; } };
    View data{rawData, rawLen};
    if (!rawData || rawLen == 0) return false;
    if (!_online) {
        Serial.printf("[TCP] TX BLOCKED (offline) %d bytes to %s:%d\n", (int)data.size(), _host.c_str(), _port);
        return false;
    }
    if (_connectState == CS_CONNECTING) {
        Serial.printf("[TCP] TX BLOCKED (connecting) %d bytes to %s:%d\n", (int)data.size(), _host.c_str(), _port);
        return false;
    }
    if (!_client.connected()) {
        Serial.printf("[TCP] TX BLOCKED (disconnected) %d bytes to %s:%d\n", (int)data.size(), _host.c_str(), _port);
        return false;
    }

    // Wrap Header1 non-announce packets as Header2 for TCP transport
    // (mirrors Rust actor.rs:653-678 — hub drops raw Header1 data packets).
    // Link packets are the exception: LRPROOF/LRRTT/link DATA are addressed
    // by link_id and must stay Header1 so the hub's link_table can route them
    // back along the link request path.
    if (_hubTransportIdKnown && data.size() >= 19) {
        uint8_t flags = data.data()[0];
        uint8_t header_type = (flags >> 6) & 0x01;
        uint8_t destination_type = (flags >> 2) & 0x03;
        uint8_t packet_type = flags & 0x03;
        bool link_packet = destination_type == 0x03;

        // Diagnostic: identify packet types going through TCP
        static const char* pt_names[] = {"DATA", "ANNOUNCE", "LINKREQ", "PROOF"};
        Serial.printf("[TCP-DIAG] send: %d bytes ht=%d dt=%d pt=%s(%d) to %s:%d\n",
            (int)data.size(), header_type, destination_type,
            (packet_type < 4) ? pt_names[packet_type] : "?", packet_type,
            _host.c_str(), _port);
        if (packet_type == 0x03) {
            Serial.printf("[TCP-DIAG] *** PROOF packet being sent via TCP! ***\n");
        }

        if (packet_type != 0x01 && !link_packet) {  // Not ANNOUNCE or LINK traffic
            if (header_type == 0) {
                // Header1 → wrap as Header2 (handles hops==1, hops==0, unknown path)
                uint8_t new_flags = flags | 0x50;  // Set Header2 (bit 6) + Transport (bit 4)

                // Build Header2 packet: flags(1) + hops(1) + transport_id(16) + original[2:]
                size_t new_len = data.size() + 16;
                if (new_len > RX_BUFFER_SIZE) {
                    Serial.printf("[TCP] H1->H2 wrap too large (%d bytes), dropping\n", (int)new_len);
                    _txDropCount++;
                    return false;
                }
                if (!_wrapBuffer) return false;
                _wrapBuffer[0] = new_flags;
                _wrapBuffer[1] = data.data()[1];  // hops
                memcpy(_wrapBuffer + 2, _hubTransportId, 16);  // transport_id
                memcpy(_wrapBuffer + 18, data.data() + 2, data.size() - 2);  // dest_hash + context + payload

                Serial.printf("[TCP] TX %d->%d bytes (H1->H2 wrap) to %s:%d\n",
                              (int)data.size(), (int)new_len, _host.c_str(), _port);
                return sendFrame(_wrapBuffer, new_len);
            }
            else if (data.size() >= 35 && memcmp(data.data() + 2, _hubTransportId, 16) != 0) {
                // Header2 with wrong transport_id → fix it
                // Transport::outbound() may have used _received_from=destination_hash
                if (!_wrapBuffer) return false;
                memcpy(_wrapBuffer, data.data(), data.size());
                memcpy(_wrapBuffer + 2, _hubTransportId, 16);

                Serial.printf("[TCP] TX %d bytes (H2 transport_id fixed) to %s:%d\n",
                              (int)data.size(), _host.c_str(), _port);
                return sendFrame(_wrapBuffer, data.size());
            }
        }
    }

    if (!_hubTransportIdKnown) {
        bool accepted = sendFrame(data.data(), data.size());
        Serial.printf("[TCP] TX %d bytes (hub ID pending) to %s:%d\n",
                      (int)data.size(), _host.c_str(), _port);
        return accepted;
    } else {
        // Passthrough: announces, correct Header2
        bool accepted = sendFrame(data.data(), data.size());
        Serial.printf("[TCP] TX %d bytes (passthrough) to %s:%d\n", (int)data.size(), _host.c_str(), _port);
        return accepted;
    }
}

// HDLC-like framing: [0x7E] [escaped data] [0x7E]
// Buffered write — single syscall instead of per-byte writes
bool TCPClientInterface::sendFrame(const uint8_t* data, size_t len) {
    if (!_txBuffer || !data || len == 0) return false;
    // Worst case: every byte escapes (2x) + 2 delimiters
    size_t maxFrameLen = len * 2 + 2;
    if (maxFrameLen > TX_BUFFER_SIZE) {
        Serial.printf("[TCP] TX frame too large (%d bytes), dropping\n", (int)len);
        _txDropCount++;
        return false;
    }
    size_t pos = 0;
    _txBuffer[pos++] = FRAME_START;
    for (size_t i = 0; i < len && pos < TX_BUFFER_SIZE - 2; i++) {
        if (data[i] == FRAME_START || data[i] == FRAME_ESC) {
            _txBuffer[pos++] = FRAME_ESC;
            _txBuffer[pos++] = data[i] ^ FRAME_XOR;
        } else {
            _txBuffer[pos++] = data[i];
        }
    }
    _txBuffer[pos++] = FRAME_START;
    const size_t written = _client.write(_txBuffer, pos);
    if (written != pos) {
        _txDropCount++;
        Serial.printf("[TCP] TX short write (%d/%d bytes)\n", (int)written, (int)pos);
        return false;
    }
    // No flush() — TCP_NODELAY sends immediately without Nagle delay
    return true;
}

int TCPClientInterface::readFrame() {
    if (!_client.available()) return 0;

    // Uses persistent member state: _inFrame, _escaped, _rxPos
    // This allows frames split across TCP segments to be reassembled correctly
    int bytesRead = 0;
    constexpr int MAX_BYTES_PER_CALL = 1024;

    while (_client.available() && _rxPos < RX_BUFFER_SIZE && bytesRead < MAX_BYTES_PER_CALL) {
        uint8_t b = _client.read();
        bytesRead++;

        if (b == FRAME_START) {
            if (_inFrame && _rxPos > 0) {
                // End of frame — return length, caller reads from _rxBuffer
                size_t frameLen = _rxPos;
                _inFrame = false;
                _escaped = false;
                _rxPos = 0;
                return frameLen;
            }
            _inFrame = true;
            _rxPos = 0;
            _escaped = false;
            continue;
        }

        if (!_inFrame) continue;

        if (b == FRAME_ESC) {
            _escaped = true;
            continue;
        }

        if (_escaped) {
            _rxBuffer[_rxPos++] = b ^ FRAME_XOR;
            _escaped = false;
        } else {
            _rxBuffer[_rxPos++] = b;
        }
    }

    // Buffer overflow protection
    if (_rxPos >= RX_BUFFER_SIZE) {
        Serial.printf("[TCP] Frame too large (%d bytes), dropping\n", (int)_rxPos);
        _inFrame = false;
        _escaped = false;
        _rxPos = 0;
    }

    return 0;  // Incomplete frame — state preserved for next call
}
