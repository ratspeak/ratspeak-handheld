#pragma once

#include <WiFi.h>
#include <WiFiServer.h>
#include <WiFiClient.h>
#include <vector>
#include <functional>
#include <string>

class WiFiInterface {
public:
    WiFiInterface(const char* name = "WiFiInterface");
    ~WiFiInterface();

    bool start();
    void stop();
    void loop();

    // Full WiFi shutdown (AP + STA + radio off)
    void stopFull();

    // AP config
    struct APCredentials { String ssid, password; };
    // Stage all allocation before an existing AP is stopped. Empty SSID chooses
    // the generated name; empty password deliberately selects an open AP.
    static bool prepareAPCredentials(const char* ssid, const char* password, APCredentials& prepared);
    void setAPCredentials(APCredentials&& prepared);
    bool setAPCredentials(const char* ssid, const char* password);
    String getAPSSID() const { return _apSSID; }
    int getClientCount() const { return _clients.size(); }
    bool isAPActive() const { return _apActive; }
    uint32_t generation() const { return _generation; }

    // STA config (optional)
    void setSTACredentials(const char* ssid, const char* password);
    bool isSTAConnected() const;

    // Scan driver helpers; WiFiConnection owns admission, deadlines and retirement.
    struct ScanResult {
        String ssid;
        int rssi;
        bool encrypted;
    };
    // False when mode initialization or SDK scan admission fails.
    static bool startAsyncScan();
    // Reads at most 15 unique rows from the bounded SDK snapshot. May throw on
    // allocation failure; WiFiConnection handles it and retires SDK storage.
    static std::vector<ScanResult> getScanResults(int maxResults = 15);

    // Raw-frame seam for the backend pump (TCPClientInterface precedent):
    // HDLC-deframed RX frames go to the sink; TX reuses the framing + client
    // fan-out unchanged.
    using RawSink = std::function<void(const uint8_t* data, size_t len)>;
    void setRawSink(RawSink sink) { _rawSink = sink; }
    bool sendRaw(const uint8_t* data, size_t len) { return sendToClients(data, len); }

private:
    bool startAP();
    void acceptClients();
    void readClients();
    bool sendToClients(const uint8_t* data, size_t len);

    // HDLC-like framing for TCP stream
    bool sendFrame(WiFiClient& client, const uint8_t* data, size_t len);
    struct FrameState {
        bool inFrame = false;
        bool escaped = false;
        size_t pos = 0;
    };
    int readFrame(WiFiClient& client, FrameState& state, uint8_t* buffer, size_t maxLen);

    RawSink _rawSink;
    std::string _name;
    bool _online = false;
    uint32_t _generation = 0;

    String _apSSID;
    String _apPassword;
    String _staSSID;
    String _staPassword;
    bool _apActive = false, _apConfigured = false;

    WiFiServer _server;
    std::vector<WiFiClient> _clients;
    std::vector<FrameState> _clientFrames;
    uint8_t _rxBuffer[600];
    uint8_t* _txBuffer = nullptr;
    static constexpr size_t TX_BUFFER_SIZE = 1202;  // worst-case: 600*2 + 2 delimiters

    static constexpr int MAX_AP_CLIENTS = 4;

    static constexpr uint8_t FRAME_START = 0x7E;
    static constexpr uint8_t FRAME_ESC   = 0x7D;
    static constexpr uint8_t FRAME_XOR   = 0x20;
};
