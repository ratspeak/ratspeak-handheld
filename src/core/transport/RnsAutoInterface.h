#pragma once

#include <Arduino.h>
#include <functional>
#include <string>

// IPv6 link-local discovery and per-peer UDP data on lwIP BSD sockets.
// Rust supplies the wire derivations; RustAutoWire.h holds timing constants.
//
// Deliberate scope (single STA netif, Python-first contract):
// - beacon cadence 1.6 s fixed (rsReticulum's 8 s background cadence skipped)
// - no multi-NIC dedup deque (unreachable with one netif)
// - data accepted only from beacon-verified peers (Python py:592-594; auto.rs is
//   laxer — the stricter validator behavior wins)
// - inbound datagrams >500 B dropped with a stat: the lite wire budget is 500 while
//   auto HW_MTU is 1196; standard RNS traffic is <=500 and we signal link MTU 500,
//   so no interop impact at defaults (documented limitation)
//
// Threading: protocol owner task only (including pump transmit).
// No ISR work, no blocking calls (all sockets O_NONBLOCK, RX capped 4/socket/pass).
class RnsAutoInterface {
public:
    using RawSink = std::function<void(const uint8_t* data, size_t len)>;
    static constexpr size_t MAX_PEERS_CAP = 16;

    struct Stats {
        uint32_t rxFrames = 0;       // data datagrams delivered to the sink
        uint32_t rxOversize = 0;     // >500 B (lite wire budget) dropped
        uint32_t rxUnknownPeer = 0;  // data from a non-peered source dropped
        uint32_t rxBeacons = 0;      // verified discovery tokens
        uint32_t rxBadBeacons = 0;   // token mismatch (wrong group / bad hash)
        uint32_t txFrames = 0;       // sendRaw fan-outs with >=1 live peer
        uint32_t txBeacons = 0;
        uint32_t peersDropped = 0;   // table full
    };

    // llRaw = the STA link-local address as RAW 16 bytes (never a formatted string:
    // Arduino IPv6Address::toString is expanded-form and would break beacon-hash
    // interop — the Rust FFI formats RFC-5952 from the raw bytes).
    bool start(const char* groupId, uint8_t maxPeers, const uint8_t llRaw[16],
               uint32_t scopeId);
    void stop();
    void loop();
    void notifyLinkChange(const uint8_t llRaw[16], uint32_t scopeId);

    bool isOnline() const { return _online; }
    size_t peerCount() const;
    const std::string& multicastAddress() const { return _mcastAddrStr; }

    // Raw-frame seam for RustInterfacePump (same shape as TCPClientInterface).
    void setRawSink(RawSink sink) { _rawSink = sink; }
    // True when at least one peer datagram was accepted by the socket.
    bool sendRaw(const uint8_t* data, size_t len);

    const Stats& stats() const { return _stats; }

private:
    struct Peer {
        uint8_t ip[16];
        unsigned long lastHeardMs;
        unsigned long lastReverseMs;
        bool used;
    };

    bool openSockets();
    void closeSockets();
    bool computeSelf(const uint8_t llRaw[16]);
    void sendBeacon();
    void pollDiscovery(int fd);
    void pollData();
    void peerJobs(unsigned long now);
    Peer* findPeer(const uint8_t ip[16]);
    void upsertPeer(const uint8_t ip[16], unsigned long now);
    bool isSelf(const uint8_t ip[16]) const;

    int _discSock = -1;
    int _unicastSock = -1;
    int _dataSock = -1;
    bool _online = false;
    bool _rxEnabled = false;  // discovery/data RX gated until our first beacon TX
    bool _mcastLoopOk = false;
    bool _oversizeLogged = false;
    uint8_t _maxPeers = 8;
    uint32_t _scopeId = 0;

    std::string _groupId;
    std::string _mcastAddrStr;
    uint8_t _mcastGroup[16] = {};
    uint8_t _selfLl[16] = {};
    char _selfLlStr[46] = {};
    uint8_t _selfToken[32] = {};
    // Prior link-locals (SLAAC rotation): late multicast echoes of our old beacons
    // must stay self-classified or we'd peer with our former address.
    uint8_t _prevLl[2][16] = {};
    int _prevLlCount = 0;

    Peer _peers[MAX_PEERS_CAP] = {};
    unsigned long _nextBeaconMs = 0;
    unsigned long _nextPeerJobMs = 0;
    unsigned long _lastEchoMs = 0;
    unsigned long _lastAnyRxMs = 0;
    bool _carrierOk = false;

    RawSink _rawSink;
    Stats _stats;
    static uint8_t _rxBuf[1300];  // HW_MTU 1196 + slack; oversize measured, then dropped
};
