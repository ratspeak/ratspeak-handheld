#pragma once

#include <stdint.h>
#include "ratspeak_protocol.h"

class RustClock;
class RustKeyMap;
class RustInterfacePump;
class RustLxmfEngine;
class RustResourceEngine;

// Reticulum LINK state machine, both roles, over the FFI link primitives.
// INITIATOR: opens a link to send an oversize/link-preferred message. RESPONDER:
// accepts inbound links from fleet peers (large inbound delivery). LRRTT
// activation (handshake msg 3), session-key storage + zeroize. Keepalive/stale
// per Python Link.py: interval = clamp(rtt*(360/1.75), 5, 360)s from the measured
// RTT at activation; watchdog baseline = last INBOUND (max of inbound/proof/
// activation — never outbound); initiator sends 0xFF, responder echoes 0xFE;
// STALE at 2*interval, LINKCLOSE teardown after rtt*4+5s grace. Inbound single
// link-packets (CTX_NONE) are proved back to the sender with the identity-signed
// explicit proof (responder role only — Link.py:277-287). Access is serialized by the protocol owner task.
class RustLinkManager {
public:
    static constexpr size_t MAX_LINKS = 4;

    struct Deps {
        rs_handheld_rns_t* ctx = nullptr;
        RustClock* clock = nullptr;
        RustKeyMap* keymap = nullptr;
        RustInterfacePump* pump = nullptr;
        RustLxmfEngine* lxmf = nullptr;
        RustResourceEngine* resources = nullptr;
        const uint8_t* ourDestHash = nullptr;  // 16 bytes
    };

    enum class State : uint8_t { Free, InitRequested, Active, RespPending, Stale, Closed };

    ~RustLinkManager() { endAll(); }  // defense-in-depth: keys never outlive the manager

    void begin(const Deps& deps);
    void loop();

    // INITIATOR: ensure an ACTIVE link on Rust's selected route. The chosen
    // interface is bound for the complete lifetime of the link.
    bool ensureLink(const uint8_t dest[16], const uint8_t pubkey[64],
                    const rs_handheld_route_t& route);

    // Encrypt+frame+TX one link data packet (context None) over the ACTIVE link to `dest`.
    // Returns false if there is no active link or the interface refuses the packet.
    // When `outHash` is non-null it receives the
    // sent packet's 32-byte hash (the LXMF engine tracks it for the delivery-proof receipt).
    bool sendLinkData(const uint8_t dest[16], const uint8_t* plaintext, size_t len,
                      uint8_t* outHash = nullptr);
    // The outgoing owner installs the hash and checked authorization before a
    // possible inline Started/proof callback. Refusal releases its workspace.
    bool buildLinkDataPacket(const uint8_t dest[16], const uint8_t* plaintext, size_t len,
                             uint8_t* raw, size_t capacity, size_t& rawLength);

    // Link session key for `dest` (64 bytes), or nullptr if no active link — used by the
    // resource engine (advertise_build / assemble take the link key).
    const uint8_t* activeLinkKey(const uint8_t dest[16]) const;
    // link_id for `dest`, or nullptr if no active link (resource frames address the link).
    const uint8_t* activeLinkId(const uint8_t dest[16]) const;
    uint8_t activeLinkIface(const uint8_t dest[16]) const;
    bool linkActive(const uint8_t dest[16]) const;
    // A false ensureLink may be local backpressure. Only an admitted handshake
    // owns a network timeout; callers must not age a full pool as peer failure.
    bool linkEstablishing(const uint8_t dest[16]) const;

    enum class RequestError : uint8_t { Timeout, LinkClosed, Unsupported, Invalid };
    struct RequestSink {
        virtual ~RequestSink() = default;
        // Encoded MessagePack value; borrow ends on return. The request has
        // already retired, so the callback may admit its next operation.
        virtual void onLinkResponse(const uint8_t* value, size_t length) = 0;
        virtual void onLinkRequestFailed(RequestError) = 0;
    };
    // One inbox control request across the existing Link pool. No packet or
    // message body is retained. Caller owns retries after local refusal.
    bool startGet(const uint8_t dest[16], uint8_t operation, const uint8_t* transientId,
                  uint16_t limitBytes, RequestSink& sink, uint32_t timeoutMs);
    bool requestPending() const { return _request.sink != nullptr; }
    void cancelRequest(RequestSink& sink);
    bool responseId(uint8_t iface, const uint8_t linkId[16], uint8_t out[16]) const;
    bool completeResponse(uint8_t iface, const uint8_t linkId[16], const uint8_t* data, size_t length);
    void failResponse(uint8_t iface, const uint8_t linkId[16], const uint8_t requestId[16], RequestError error);
    bool receiptBinding(uint8_t iface, const uint8_t linkId[16], uint8_t& slot, uint32_t& generation) const;
    bool receiptLive(uint8_t slot, uint32_t generation, uint8_t iface) const;
    const uint8_t* receiptPeer(uint8_t slot, uint32_t generation) const;

    // Pump local-frame dispatch (ProtocolRuntime routes Link-typed / LINKREQUEST frames here).
    void onLocalFrame(const rs_handheld_local_frame_t& f, uint8_t ifaceId);

    size_t activeCount() const;
    void endAll();  // zeroize + free all links (teardown)

private:
    struct Link {
        State state = State::Free;
        uint8_t peerDest[16] = {};   // recipient dest (initiator); responder has no authenticated peer binding
        uint8_t linkId[16] = {};
        uint8_t pubkey[64] = {};     // peer identity public key (for proof validate)
        uint8_t ephPriv[32] = {};    // our ephemeral x25519 private
        uint8_t sessionKey[64] = {};
        bool haveKey = false;
        bool initiator = false;
        bool identified = false;
        uint8_t iface = UINT8_MAX;
        uint8_t hops = 1;
        bool hasNextHop = false;
        uint8_t nextHop[16] = {};
        unsigned long requestMs = 0;
        uint32_t establishmentTimeoutMs = 12000;
        // Watchdog baseline: refreshed on inbound, proof-echo and activation ONLY
        // (Link.py:788-790 — TX must never mask peer death).
        unsigned long lastInboundMs = 0;
        unsigned long lastKeepaliveSentMs = 0;
        // One coalesced FF/FE control owned by this watchdog until driver
        // admission. Refusal preserves its birth and interface incarnation.
        uint64_t keepaliveBornMs = 0;
        uint32_t keepaliveInterfaceGeneration = 0;
        bool keepalivePending = false;
        bool keepaliveAttempted = false;
        unsigned long staleSinceMs = 0;
        uint32_t keepaliveMs = 360000;  // KEEPALIVE default until RTT is measured
        uint32_t staleTimeMs = 720000;  // STALE_FACTOR(2) * keepalive
        double rttSecs = 0;
    };

    Link* findByDest(const uint8_t dest[16]);
    Link* findByLinkId(const uint8_t linkId[16]);
    Link* allocLink();
    void closeLink(Link& l);
    void failSetup(Link& l);
    bool sendLinkFrame(Link& l, uint8_t context, const uint8_t* payload, size_t len,
                       uint8_t* outHash = nullptr);
    bool frameToDest(uint8_t ifaceId, const uint8_t dest[16], uint8_t headerType,
                     const uint8_t nextHop[16],
                     uint8_t packetType, uint8_t destType, uint8_t context, const uint8_t* payload,
                     size_t len, uint8_t* outHash = nullptr);
    void onLrProof(Link& l, const rs_handheld_local_frame_t& f);
    void onLinkRequest(const rs_handheld_local_frame_t& f, uint8_t ifaceId);
    void onLinkData(Link& l, const rs_handheld_local_frame_t& f);
    void updateKeepalive(Link& l, double rttSecs);
    void queueKeepalive(Link& l);
    void retryKeepalive(Link& l);
    void sendTeardown(Link& l);
    bool identify(Link&);
    bool requestLive() const;
    void failRequest(RequestError);
    bool buildLinkPacket(const uint8_t dest[16], uint8_t context, const uint8_t* plaintext,
                         size_t len, uint8_t* raw, size_t capacity, size_t& rawLength);

    struct Request {
        RequestSink* sink = nullptr;
        uint64_t bornMs = 0;
        uint32_t waitMs = 0, sequence = 0, linkGeneration = 0, interfaceGeneration = 0;
        uint8_t id[16]{};
        uint8_t slot = UINT8_MAX, iface = UINT8_MAX;
    };
    Request _request;
    uint32_t _requestSequence = 0;

    Deps _d;
    Link _links[MAX_LINKS];
    uint32_t _generations[MAX_LINKS] = {}; // retained across close/reset; exhausted slots retire
};
