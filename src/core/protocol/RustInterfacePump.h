#pragma once

#include <stddef.h>
#include <stdint.h>
#include "ratspeak_protocol.h"
#include "protocol/RustClock.h"

class LoRaInterface;
class TCPClientInterface;
class RnsAutoInterface;
class WiFiInterface;

// Sink for packets the Rust node surfaces to the C++ delivery engines: validated
// announces (-> AnnounceManager/KeyMap) and local frames (-> LXMF/link/resource).
// ProtocolRuntime implements it; the pump forwards without knowing the engine types.
class RustPumpSink {
public:
    virtual ~RustPumpSink() = default;
    virtual void onAnnounceEvent(const rs_handheld_announce_event_t& ev, uint8_t ifaceId) = 0;
    virtual void onLocalFrame(const rs_handheld_local_frame_t& f, uint8_t ifaceId) = 0;
    // An inbound path request for our own dest: re-announce as a PATH_RESPONSE (throttled).
    virtual void onOwnPathRequest(uint8_t ifaceId, const uint8_t tag[16], size_t tagLen) = 0;
};

// Raw-frame pump between the C++ interface drivers and the Rust transport node
//. Interface ids: 0 = LoRa, 1..4 = TCP clients (registry order, stable per
// attach cycle), 5 = AutoInterface, 6 = WiFi-AP server. Rust owns route and
// transport policy; this class performs only the requested hardware I/O:
// sendTo() targets one route/link interface, sendAll() broadcasts when Rust has
// no path, and transport-originated forwards use their explicit origin/reason.
// Calls are serialized by the protocol owner task; the FFI has no internal locks.
class RustInterfacePump {
public:
    static constexpr uint8_t LORA_IFACE_ID = 0;
    static constexpr uint8_t TCP_IFACE_BASE = 1;
    static constexpr size_t MAX_TCP = 4;
    static constexpr uint8_t AUTO_IFACE_ID = 5;
    static constexpr uint8_t WIFI_AP_IFACE_ID = 6;

    struct Counters {
        uint32_t rxFrames = 0;
        uint32_t rxAccepted = 0;
        uint32_t rxDuplicates = 0;
        uint32_t rxAnnounces = 0;      // validated announces seen (event filled)
        uint32_t rxLocal = 0;          // local-delivery frames handed to the engines
        uint32_t rxPathReqSelf = 0;    // path requests for our own dest (-> re-announce)
        uint32_t rxAnnounceIgnored = 0;  // freshness-rejected announce; no contact side effect
        uint32_t rxDropped = 0;
        uint32_t rxErrors = 0;         // malformed / FFI-rejected frames
        uint32_t txFrames = 0;
        uint32_t txDropped = 0;        // no live interface to send on
    };

    void begin(rs_handheld_rns_t* ctx, RustClock* clock);
    // Quiesce before the FFI context is destroyed: clears every driver raw sink
    // and nulls _ctx so no late RX/TX can reach a freed node.
    void stop();
    bool started() const { return _ctx != nullptr; }

    void setSink(RustPumpSink* sink) { _sink = sink; }

    void attachLoRa(LoRaInterface* lora);
    int attachTcp(TCPClientInterface* tcp);  // returns interface id, -1 if full
    void detachTcpAll();
    void attachAuto(RnsAutoInterface* autoIface);  // driver lifecycle stays main-owned
    void attachWifiAp(WiFiInterface* wifiAp);

    bool loraOnline() const;
    // Authoritative first-hop bitrate for a registered interface, or zero when unavailable.
    uint32_t interfaceBitrate(uint8_t ifaceId) const;
    uint32_t interfaceTxWaitMs(uint8_t ifaceId, uint32_t packets) const;
    int lastLoraRssi() const;    // last RX RSSI from the LoRa driver (0 if no LoRa)
    float lastLoraSnr() const;

    // One pass: LoRa driver loop (TCP loops stay in the mains), tick, TX drain.
    void loop();

    // Hardware-only TX primitives. Engines select one with rs_handheld_rns_route();
    // announces and genuinely pathless endpoint traffic use sendAll().
    bool sendTo(uint8_t ifaceId, const uint8_t* data, size_t len);
    bool sendAll(const uint8_t* data, size_t len);

    const Counters& counters() const { return _counters; }

private:
    void ingest(const uint8_t* data, size_t len, uint8_t ifaceId);
    void drainOutbound();
    void transmit(const uint8_t* data, size_t len, uint8_t originIface, int32_t reason);
    bool sendAllExcept(const uint8_t* data, size_t len, uint8_t excludedIface);
    int32_t interfaceMode(uint8_t ifaceId) const;

    rs_handheld_rns_t* _ctx = nullptr;
    RustClock* _clock = nullptr;
    RustPumpSink* _sink = nullptr;
    LoRaInterface* _lora = nullptr;
    TCPClientInterface* _tcp[MAX_TCP] = {};
    size_t _tcpCount = 0;
    RnsAutoInterface* _auto = nullptr;
    WiFiInterface* _wifiAp = nullptr;
    Counters _counters;

    // Static scratch (loopTask-only): keeps the large POD out-params off the task stack.
    rs_handheld_announce_event_t _event;
    rs_handheld_local_frame_t _local;
    uint8_t _pathRequestTag[16] = {};
    uint8_t _txBuf[500];
    size_t _pendingLen = 0;
    uint8_t _pendingTargets = 0;
    unsigned long _pendingSince = 0;
};
