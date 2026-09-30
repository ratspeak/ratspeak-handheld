#pragma once

#include <stdint.h>
#include "ratspeak_protocol.h"
#include "reticulum/LXMFManager.h"
#include "config/Config.h"
#include "RustIncomingDelivery.h"
#include "OutgoingContract.h"

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

    using Ticket = handheld::outgoing::Ticket;
    using Submission = handheld::outgoing::Submission;
    using InitialResult = handheld::outgoing::InitialResult;
    using StatusView = handheld::outgoing::StatusView;
    bool begin(const Deps& deps);
    Submission submit(const uint8_t dest[16], const uint8_t* title, size_t titleLength,
                      const uint8_t* content, size_t contentLength, bool preferLink = false);
    handheld::outgoing::Poll poll(Ticket, InitialResult&) const;
    bool acknowledge(Ticket);
    bool cancel(Ticket);
    bool status(const handheld::storage::RecordKey&, StatusView&) const;
    uint32_t statusRevision() const { return _statusRevision; }
    void stopAdmissions();
    bool drained() const;
    handheld::storage::Error drainError() const;
    bool beginPeerDelete(const uint8_t peer[16]);
    void finishPeerDelete(const uint8_t peer[16], const handheld::storage::Result&);
    int queuedCount() const;
    void loop();
    void onLinkSetupFailure(const uint8_t peer[16], const rs_handheld_route_t& failedRoute);
    void onResourceOutcome(Ticket, handheld::outgoing::ResourceOutcome);
    handheld::TxOffer offerResource(Ticket, uint8_t iface, const uint8_t* raw, size_t length, uint64_t bornMs);
    uint64_t resourceSendBinding(Ticket, uint8_t iface, const uint8_t linkId[16]) const;
    static bool receiptHook(void*, handheld::TxReceipt, handheld::TxReceiptEvent);

    // Pump local-frame dispatch (ProtocolRuntime routes by packet_type/context).
    void onDataFrame(const rs_handheld_local_frame_t& f, uint8_t ifaceId);
    void onProofFrame(const rs_handheld_local_frame_t& f);
    // Deliver a fully-assembled DIRECT (link/resource) LXMF payload (from the link/resource path).
    // Every admitted persistent message remains Pending until the incoming owner
    // consumes its committed storage result. Callers never independently prove it.
    RustIncomingDelivery::ReceiveResult onDirectPayload(const uint8_t* packed, size_t len,
                                                        const RustIncomingDelivery::ReceiptSeed&);
    RustIncomingDelivery& incoming() { return _incoming; }
    const RustIncomingDelivery& incoming() const { return _incoming; }
private:
    static constexpr uint8_t RowCount = 20, ReceiptBase = 128, ProofAttempts = 3;
    static_assert(ReceiptBase + RowCount * 4 <= UINT8_MAX, "Outgoing receipt namespace overlaps invalid handle");
    enum class Phase : uint8_t { Free, Saving, Query, Ready, Reading, Prepared, AwaitProof, Resource, Grace, Settled };
    enum Flag : uint16_t { Acknowledged = 1, Suppressed = 2, InitialSuppressed = 4,
        PreferLink = 8, ViaLink = 16, Rediscover = 32, Deleted = 64, DeletePending = 128,
        Recovered = 256, Notify = 512, BlockedRecord = 1024, ProofGrace = 2048 };
    struct OutgoingRow {
        double timestamp = 0;
        uint64_t storageSequence = 0, nextAttempt = 0, linkSince = 0, receiptSince = 0, statusRetry = 0;
        uint8_t publicKey[64] = {}, hashes[3][32] = {}, peer[16] = {}, messageId[32] = {};
        rs_handheld_route_t route = {};
        uint32_t generation = 0, identityGeneration = 0, counter = 0, revision = 0;
        uint32_t proofWait = 0;
        uint16_t flags = 0;
        uint8_t storageSlot = UINT8_MAX;
        handheld::storage::Operation storageOperation = handheld::storage::Operation::CreateOutgoing;
        Phase phase = Phase::Free;
        LXMFStatus desired = LXMFStatus::QUEUED, durable = LXMFStatus::QUEUED;
        handheld::storage::Error error = handheld::storage::Error::None, initialError = handheld::storage::Error::None;
        uint8_t initial = 0, proofCount = 0, discoveryCount = 0, receiptMask = 0, queuedReceipts = 0;
    };
    struct BodyWorkspace {
        uint8_t bytes[handheld::storage::Budget::MaxMessageBody] = {};
        handheld::storage::StoredRecordHeader header;
        handheld::TxLease lease;
        uint32_t generations[7] = {};
        uint32_t generation = 0;
        uint16_t length = 0;
        uint8_t slot = UINT8_MAX, targets = 0;
    };
    static_assert(sizeof(OutgoingRow) <= 320, "Review outgoing descriptor budget");
    static_assert(sizeof(BodyWorkspace) <= 4096, "Review outgoing body workspace budget");
    OutgoingRow* row(Ticket);
    const OutgoingRow* row(Ticket) const;
    Ticket allocate();
    void retire(Ticket);
    handheld::storage::RecordKey key(const OutgoingRow&) const;
    handheld::storage::Ticket storageTicket(const OutgoingRow&) const;
    void hold(OutgoingRow&, handheld::storage::Submission, handheld::storage::Operation);
    void releaseBody(Ticket);
    void settleStorage(Ticket);
    void advance(Ticket);
    void attempt(Ticket);
    void preparePacket(Ticket, const uint8_t*, size_t, uint8_t interfaceId, bool broadcast);
    void offerPrepared(Ticket);
    void setStatus(Ticket, LXMFStatus);
    void finishRouteFailure(Ticket);
    bool validatesReceipt(const OutgoingRow&, const rs_handheld_local_frame_t&) const;
    bool buildPacketProof(const uint8_t packetHash[32], uint8_t raw[128], size_t& length);
    void requestUnknownSource(const uint8_t source[16]);
    struct SourceRequest { bool used = false; uint8_t source[16] = {}; unsigned long sentMs = 0; };
    static constexpr size_t SOURCE_REQUEST_SLOTS = 16;
    static constexpr unsigned long SOURCE_REQUEST_THROTTLE_MS = 30000;
    Deps _d;
    OutgoingRow _rows[RowCount];
    BodyWorkspace _body;
    handheld::storage::RecordKey _recoveryCursor;
    uint8_t _deletingPeer[16] = {};
    uint32_t _identityGeneration = 0, _statusRevision = 0;
    uint8_t _cursor = 0;
    bool _accepting = false, _recovering = false, _polling = false, _deleting = false;
    SourceRequest _sourceRequests[SOURCE_REQUEST_SLOTS];
    RustIncomingDelivery _incoming;
};
