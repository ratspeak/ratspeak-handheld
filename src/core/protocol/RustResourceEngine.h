#pragma once

#include <stdint.h>
#include <functional>
#include "ratspeak_protocol.h"

class RustClock;
class RustInterfacePump;
class RustLxmfEngine;

// One outbound + one inbound Reticulum Resource transfer. Rust owns codecs and crypto;
// this owner schedules interface I/O, retries, cancellation and durable delivery.
// Sender: advertise -> serve part requests -> await delivery proof. Receiver:
// accept -> request/ingest parts (window grow/shrink) -> assemble -> proof.
// ADV/REQ/ICL/RCL are link-encrypted. Parts and delivery proofs are raw.
// All calls belong to the protocol service owner.
class RustResourceEngine {
public:
    struct Deps {
        rs_handheld_rns_t* ctx = nullptr;
        RustClock* clock = nullptr;
        RustInterfacePump* pump = nullptr;
        RustLxmfEngine* lxmf = nullptr;
    };
    // Called with (peerDest16, delivered) when an outbound resource resolves: true on a valid
    // delivery proof, false on cancellation/timeout — LXMF flips DELIVERED / FAILED.
    using OutcomeCb = std::function<void(const uint8_t*, bool)>;

    void begin(const Deps& deps) { _d = deps; }
    void setOutcomeCallback(OutcomeCb cb) { _outcome = cb; }

    // SENDER: start an outbound resource of `data` over the ACTIVE link (peerDest/linkId/key).
    // Returns false if a resource is already in flight or the ADV could not be built.
    bool startSend(const uint8_t peerDest[16], const uint8_t linkId[16], const uint8_t key[64],
                   uint8_t ifaceId, const uint8_t* data, size_t len);
    bool sending() const { return _out.active; }
    bool receiving() const { return _in.active; }
    uint8_t activeTransfers() const { return (uint8_t)((_out.active ? 1 : 0) + (_in.active ? 1 : 0)); }

    // Link-frame dispatch from the link manager (RESOURCE_ADV/REQ/PRF/parts).
    void onLinkFrame(const uint8_t peerDest[16], const uint8_t linkId[16], const uint8_t key[64],
                     uint8_t ifaceId, const rs_handheld_local_frame_t& f);
    void dropPeer(const uint8_t peerDest[16]);
    void loop();
    void endAll();

private:
    struct Out {
        bool active = false;
        uint8_t peerDest[16] = {};
        uint8_t linkId[16] = {};
        uint8_t key[64] = {};
        uint8_t resourceHash[32] = {};
        uint8_t iface = UINT8_MAX;
        uint32_t numParts = 0;
        unsigned long startMs = 0;
        unsigned long lastActivityMs = 0;  // reset on each served request (Python idle timeout)
        // ADV-resend (Resource.py:573-592): re-advertise up to MAX_ADV_RETRIES if no REQ arrives.
        uint8_t adv[RS_HANDHELD_RESOURCE_ADV_MAX] = {};
        size_t advLen = 0;
        unsigned long lastAdvMs = 0;
        uint8_t advRetriesLeft = 0;
        bool reqReceived = false;       // a REQ arrived -> stop re-advertising
        uint8_t pendingParts = 0;       // retained indices, not copies of ciphertext
    };
    struct In {
        bool active = false;
        uint8_t peerDest[16] = {};
        uint8_t linkId[16] = {};
        uint8_t key[64] = {};
        uint8_t resourceHash[32] = {};
        uint8_t iface = UINT8_MAX;
        uint32_t numParts = 0;
        unsigned long lastReqMs = 0;
        unsigned long lastPartMs = 0;   // progress marker (shrink fires on NO progress only)
        uint32_t partsRemaining = 0;    // missing parts named in the accepted request
        unsigned long startMs = 0;
        uint8_t reqRetriesLeft = 0;     // bounded part re-requests (Resource.py:613-628 MAX_RETRIES)
        bool requestPending = false;
    };

    bool frameLinkEncrypted(uint8_t ifaceId, const uint8_t linkId[16], const uint8_t key[64],
                            uint8_t packetType, uint8_t context, const uint8_t* plaintext,
                            size_t len, bool retain = false);
    bool frameRawPart(uint8_t ifaceId, const uint8_t linkId[16], const uint8_t* part, size_t len);
    // Resource delivery proof: PT_PROOF/RESOURCE_PRF, PLAINTEXT on the link (Packet.py:196 —
    // "Resource proofs are not encrypted"; same as packet proofs over links, Packet.py:199).
    void frameRawProof(uint8_t ifaceId, const uint8_t linkId[16], const uint8_t* proof, size_t len);
    void sendRequest();
    void servePendingParts();
    void cancelOutbound();
    void closeInbound(bool cancel);
    uint32_t waitMs(uint8_t iface, uint32_t packets, uint32_t minimum) const;
    bool retainControl(uint8_t iface, const uint8_t* raw, size_t len);
    void closeOutbound(bool delivered, bool notify = true);

    // Terminal frames survive transfer cleanup and temporary interface backpressure.
    // Four short packets bound memory even if several peers repeatedly advertise while busy.
    struct Control {
        uint8_t raw[128] = {};
        size_t len = 0;
        uint8_t iface = UINT8_MAX;
        unsigned long queuedMs = 0;
    };
    Control _control[4];

    Deps _d;
    Out _out;
    In _in;
    OutcomeCb _outcome;
};
