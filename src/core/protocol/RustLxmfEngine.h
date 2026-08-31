#pragma once

#include <stdint.h>
#include <deque>
#include <map>
#include <string>
#include <vector>
#include "ratspeak_protocol.h"
#include "reticulum/LXMFManager.h"
#include "config/Config.h"

class MessageStore;
class RustClock;
class RustKeyMap;
class RustInterfacePump;
class RustLinkManager;
class RustResourceEngine;

// LXMF delivery engine: opportunistic messages and delivery proofs, with
// link/Resource fallback for larger payloads. Discovery and proof retries have
// separate bounded budgets. Access is serialized by the protocol owner task.
class RustLxmfEngine {
public:
    struct Deps {
        rs_handheld_rns_t* ctx = nullptr;
        RustClock* clock = nullptr;
        RustKeyMap* keymap = nullptr;
        MessageStore* store = nullptr;
        RustInterfacePump* pump = nullptr;
        RustLinkManager* links = nullptr;
        RustResourceEngine* resources = nullptr;
        LXMFManager::MessageCallback* onMessage = nullptr;
        LXMFManager::StatusCallback* statusCb = nullptr;
        const uint8_t* ourDestHash = nullptr;  // 16 bytes, our lxmf.delivery dest
    };

    void begin(const Deps& deps);
    bool send(const uint8_t dest[16], const char* content, const char* title, bool preferLink);
    // Boot restore: re-queue store-persisted QUEUED/SENDING messages (already saved —
    // no new store record). Micro parity: LXMFManager::begin does the same.
    void restorePending(const std::vector<LXMFMessage>& pending);
    int queuedCount() const { return (int)_outQueue.size(); }
    void loop();

    // Pump local-frame dispatch (ProtocolRuntime routes by packet_type/context).
    void onDataFrame(const rs_handheld_local_frame_t& f, uint8_t ifaceId);
    void onProofFrame(const rs_handheld_local_frame_t& f);
    // Deliver a fully-assembled DIRECT (link/resource) LXMF payload (from the link/resource path).
    // True means validation and storage succeeded. Previously stored duplicates and
    // reactions return true even though they are not stored, so PROVE_ALL retries heal.
    bool onDirectPayload(const uint8_t* packed, size_t len);
    // Resource-transfer outcome (the resource proof is the delivery ack): flips the tracked
    // resource-sent message DELIVERED / FAILED.
    void onResourceOutcome(const uint8_t peerDest[16], bool delivered);
    // Conversation deletion fence: purge every queued/in-flight write for this peer without
    // emitting status updates into the conversation that is about to be removed.
    void dropPeer(const uint8_t peerDest[16]);

private:
    struct OutMsg {
        uint8_t dest[16] = {};
        std::string content;
        std::string title;
        double timestamp = 0;
        uint8_t messageId[32] = {};
        bool haveId = false;
        bool preferLink = false;
        bool viaLink = false;  // downgraded once: skip the opportunistic rebuild on later passes
        uint32_t savedCounter = 0;
        int retries = 0;             // path-discovery attempts (10s x7)
        uint8_t proofAttempts = 0;   // proof-timeout requeues (cap 3) — independent budget
        unsigned long lastRetryMs = 0;
        unsigned long linkStartMs = 0;  // first link-establishment attempt; bounds the wait
    };
    struct Pending {
        uint8_t hash[32] = {};
        uint8_t pubkey[64] = {};
        uint8_t dest[16] = {};
        std::string content;  // retained for requeue (store tail may have scrolled past)
        std::string title;
        double timestamp = 0;
        uint32_t savedCounter = 0;
        unsigned long createdMs = 0;
        unsigned long jitterMs = 0;  // per-pending retry jitter (de-syncs concurrent retries)
        uint32_t interfaceWaitMs = 0; // accepted radio queue/pacing allowance
        uint8_t attempts = 0;
        bool preferLink = false;  // a link-packet pending: requeue re-takes the link path
    };
    struct ResPending {  // single outbound resource transfer (engine serializes)
        bool active = false;
        uint8_t dest[16] = {};
        double timestamp = 0;
        uint32_t savedCounter = 0;
    };

    bool attempt(OutMsg& msg);
    // Link/resource fallback for oversize / LoRa-gated / link-preferred messages. Returns true if
    // the link/resource path handled the message; *done = true when resolved (dequeue), false while
    // the link is still establishing (retry next pass). Returns false if unavailable (caller FAILs).
    bool deliverViaLinkOrResource(OutMsg& msg, const uint8_t pub[64], bool* done);
    bool deliverLocal(const uint8_t src[16], const uint8_t pub[64], const rs_handheld_lxmf_message_t& m,
                      double localTs);
    void emitProof(const uint8_t packetHash[32], uint8_t ifaceId);
    void markStatus(const std::string& peerHex, double ts, uint32_t counter, LXMFStatus st);
    void requeueOrFail(Pending& p);
    void requestUnknownSource(const uint8_t source[16]);

    struct SourceRequest {
        bool used = false;
        uint8_t source[16] = {};
        unsigned long sentMs = 0;
    };
    static constexpr size_t SOURCE_REQUEST_SLOTS = 16;
    static constexpr unsigned long SOURCE_REQUEST_THROTTLE_MS = 30000;

    Deps _d;
    std::deque<OutMsg> _outQueue;
    std::map<std::string, Pending> _pending;  // full-hash-hex -> pending proof
    ResPending _resPending;
    SourceRequest _sourceRequests[SOURCE_REQUEST_SLOTS];
};
