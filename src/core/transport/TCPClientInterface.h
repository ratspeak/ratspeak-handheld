#pragma once

#include "config/Config.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <functional>
#include <atomic>
#include <string>

class TCPClientInterface {
public:
    TCPClientInterface(const char* host, uint16_t port, const char* name);
    ~TCPClientInterface();

    bool start();
    void stop();
    void loop();

    bool isConnected() {
        if (_connectState == CS_CONNECTING) return false;
        return _client.connected();
    }
    bool canDestroy() const { return _connectState != CS_CONNECTING; }
    const String& host() const { return _host; }
    uint16_t port() const { return _port; }

    // Raw-frame seam for the backend pump: HDLC-deframed RX frames go to
    // the sink; TX keeps the Header2 hub-wrap + HDLC framing path unchanged.
    using RawSink = std::function<void(const uint8_t* data, size_t len)>;
    void setRawSink(RawSink sink) { _rawSink = sink; }
    bool sendRaw(const uint8_t* data, size_t len) { return send_outgoing(data, len); }

private:
    bool send_outgoing(const uint8_t* data, size_t len);
    void tryConnect();
    bool sendFrame(const uint8_t* data, size_t len);
    int readFrame();
    static void connectTaskFn(void* arg);
    void waitForConnectTask();

    // WiFiClient::connect() blocks. Run it in a one-shot task and let the
    // main loop claim the result before touching _client again.
    enum ConnectState : uint8_t {
        CS_IDLE       = 0,
        CS_CONNECTING = 1,
        CS_CONNECTED  = 2,
        CS_FAILED     = 3,
    };
    std::atomic<ConnectState> _connectState{CS_IDLE};
    TaskHandle_t _connectTask = nullptr;

    RawSink _rawSink;
    std::string _name;
    std::atomic<bool> _online{false};

    WiFiClient _client;
    String _host;
    uint16_t _port;
    unsigned long _lastAttempt = 0;
    unsigned long _lastRxTime = 0;
#if TCP_SHARED_BUFFERS
    // One static buffer set for all connections — main loop drives them
    // sequentially; saves ~6KB internal heap per extra connection (no PSRAM)
    static uint8_t* _rxBuffer;
    static uint8_t* _txBuffer;
    static uint8_t* _wrapBuffer;
    static bool _buffersAllocated;
#else
    uint8_t* _rxBuffer = nullptr;
    uint8_t* _txBuffer = nullptr;    // PSRAM-allocated send frame buffer
    uint8_t* _wrapBuffer = nullptr;  // PSRAM-allocated packet rewrite buffer
#endif
    static constexpr size_t RX_BUFFER_SIZE = 2048;
    static constexpr size_t TX_BUFFER_SIZE = RX_BUFFER_SIZE * 2 + 2;

    // Exponential reconnect backoff: 1s → ×2 (+0-20% jitter) → 5min cap
    unsigned long _reconnectBackoff = 1000;

    // Hub transport_id for Header2 wrapping (learned from incoming Header2 packets)
    uint8_t _hubTransportId[16] = {};
    bool _hubTransportIdKnown = false;

    // Telemetry counters
    unsigned long _hubRxCount = 0;
    unsigned long _txDropCount = 0;

    // Persistent HDLC frame reassembly state (survives across loop() calls)
    bool _inFrame = false;
    bool _escaped = false;
    size_t _rxPos = 0;

    static constexpr uint8_t FRAME_START = 0x7E;
    static constexpr uint8_t FRAME_ESC   = 0x7D;
    static constexpr uint8_t FRAME_XOR   = 0x20;
    static constexpr unsigned long TCP_KEEPALIVE_TIMEOUT_MS = 300000; // 5 min
    static constexpr unsigned long TCP_LOOP_BUDGET_MS = 25;

public:
    unsigned long lastRxTime() const { return _lastRxTime; }
    unsigned long hubRxCount() const { return _hubRxCount; }
};
