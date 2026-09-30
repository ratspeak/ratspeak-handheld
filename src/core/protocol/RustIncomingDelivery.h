#pragma once

#include "reticulum/LXMFManager.h"
#include "transport/TxLease.h"
#include "ratspeak_protocol.h"
#include <cstddef>
#include <cstdint>

class RustClock;
class RustInterfacePump;
class RustLinkManager;

// Protocol-owner receipts share the same durable incoming transaction across
// opportunistic, Link and Resource paths. Bodies live only in storage slots.
class RustIncomingDelivery {
public:
    static constexpr uint8_t RowCount = 4, ReceiptCount = 12, ReceiptsPerRow = 3;
    static constexpr uint8_t NoRow = UINT8_MAX, NoLink = 7;
    static constexpr uint32_t MaxWaitMs = 120000;
    struct IncomingRef {
        uint32_t generation = 0;
        uint8_t slot = UINT8_MAX;
        bool valid() const { return generation && slot < RowCount; }
    };
    enum class ReceiveCode : uint8_t { Rejected, Backpressured, Pending, HandledNonpersistent };
    enum class ReceiveError : uint8_t { None, Invalid, SourceUnknown, Storage, Capacity, Stopped };
    struct ReceiveResult {
        ReceiveCode code = ReceiveCode::Rejected;
        IncomingRef incoming;
        ReceiveError error = ReceiveError::Invalid;
    };
    enum class Kind : uint8_t { Packet, LinkPacket, Resource, Control, ReservedResource, NoProof, PreparedResource, Propagation };
    enum class CommitState : uint8_t { Invalid, Pending, Committed, Failed };
    struct CommitResult {
        handheld::storage::RecordKey key;
        uint8_t messageId[32]{};
        uint32_t revision = 0;
        handheld::storage::Error error = handheld::storage::Error::None;
    };
    // Stack-only validated ingress/proof input. A Resource reservation preserves
    // the ADV birth/deadline; the raw bytes are copied before return.
    struct ReceiptSeed {
        const uint8_t* raw = nullptr;
        size_t length = 0;
        uint64_t bornMs = 0;
        uint32_t maxWaitMs = MaxWaitMs;
        uint32_t interfaceGeneration = 0, linkGeneration = 0;
        handheld::TxReceipt reserved;
        uint8_t interfaceId = UINT8_MAX, linkSlot = NoLink;
        Kind kind = Kind::NoProof;
    };
    struct MessageView {
        const uint8_t* messageId = nullptr;
        const uint8_t* source = nullptr;
        const uint8_t* title = nullptr;
        const uint8_t* content = nullptr;
        size_t titleLength = 0, contentLength = 0;
        double timestamp = 0;
        bool reaction = false;
    };
    struct Deps {
        rs_handheld_rns_t* ctx = nullptr;
        RustClock* clock = nullptr;
        MessageStore* store = nullptr;
        RustInterfacePump* pump = nullptr;
        RustLinkManager* links = nullptr;
        LXMFManager::MessageCallback* onMessage = nullptr;
        const uint8_t* ourDestHash = nullptr;
    };

    bool begin(const Deps&, bool bindReceiptHook = true);
    ReceiveResult accept(const MessageView&, const ReceiptSeed&);
    // Propagation retains a scalar result until explicit release. It emits no
    // transport proof and never treats a disappeared row as a storage commit.
    // There is one propagation consumer per MID; duplicates share the same ref.
    CommitState heldCommit(IncomingRef, CommitResult&) const;
    bool releaseHeld(IncomingRef);
    bool captureSeed(ReceiptSeed&, Kind, uint8_t iface, const uint8_t* raw, size_t length,
                     const uint8_t* linkId = nullptr);
    handheld::TxReceipt reserveResource(uint8_t iface, const uint8_t linkId[16], const uint8_t hash[32]);
    bool resourcePending(uint8_t iface, const uint8_t linkId[16], const uint8_t hash[32]) const;
    bool resourceSeed(handheld::TxReceipt, ReceiptSeed&, const uint8_t* raw, size_t length);
    bool resourceLive(handheld::TxReceipt) const;
    void setResourceDeadline(handheld::TxReceipt, uint32_t waitMs);
    void cancelResource(uint8_t iface, const uint8_t linkId[16], const uint8_t hash[32]);
    void releaseReservation(handheld::TxReceipt);
    bool retainControl(uint8_t iface, const uint8_t linkId[16], const uint8_t* raw, size_t length,
                       handheld::TxReceipt reservation = {});
    void dropPeer(const uint8_t peer[16]);
    void dropLink(uint8_t slot, uint32_t generation);
    void stopAdmissions();
    void poll();
    bool drained() const;
    void detach(); // after pump quiesce and drained(), while this owner still lives
    size_t pendingRows() const;
    size_t retainedReceipts() const;
    static bool receiptHook(void*, handheld::TxReceipt, handheld::TxReceiptEvent);

private:
    static constexpr uint16_t ProofBits = (1u << ReceiptCount) - 1, HeldCommit = 1u << 15;
    enum class Phase : uint8_t { Free, Reserved, AwaitStore, Committed, Reaction, Invisible };
    enum class State : uint8_t { Free, Reserved, Eligible, Queued };
    struct IncomingRow {
        uint64_t ticketSequence = 0;
        uint32_t generation = 0, identityGeneration = 0, visibilityGeneration = 0, committedCounter = 0;
        uint8_t messageId[32] = {}, peer[16] = {};
        uint32_t commitRevision = 0;
        uint8_t ticketSlot = UINT8_MAX;
        Phase phase = Phase::Free;
        uint16_t proofMask = 0;
    };
    struct ReceiptContext {
        uint8_t raw[128] = {};
        uint64_t bornMs = 0;
        uint32_t generation = 0, rowGeneration = 0, interfaceGeneration = 0, linkGeneration = 0, maxWaitMs = 0;
        uint8_t rawLength = 0, rowIndex = NoRow, interfaceId = UINT8_MAX, kindStateLinkSlot = 0;
        Kind kind() const { return Kind(kindStateLinkSlot & 7); }
        State state() const { return State((kindStateLinkSlot >> 3) & 3); }
        uint8_t linkSlot() const { return kindStateLinkSlot >> 5; }
        void set(Kind k, State s, uint8_t link) { kindStateLinkSlot = uint8_t(k) | (uint8_t(s) << 3) | (link << 5); }
        void state(State s) { set(kind(), s, linkSlot()); }
    };
    static_assert(sizeof(IncomingRow) == 80, "Review incoming descriptor budget");
    static_assert(sizeof(ReceiptContext) == 160, "Review proof/control descriptor budget");
    IncomingRow* row(IncomingRef);
    ReceiptContext* receipt(handheld::TxReceipt);
    const ReceiptContext* receipt(handheld::TxReceipt) const;
    IncomingRef allocateRow(const MessageView&);
    handheld::TxReceipt allocateReceipt(const ReceiptSeed&, IncomingRef);
    bool bindingLive(const ReceiptContext&) const;
    bool receiptLive(handheld::TxReceipt) const;
    bool sameResource(const ReceiptContext&, uint8_t iface, const uint8_t linkId[16], const uint8_t hash[32]) const;
    void retireReceipt(handheld::TxReceipt);
    void releaseRow(IncomingRef);
    void drainReceipt(handheld::TxReceipt);
    IncomingRow _rows[RowCount];
    ReceiptContext _receipts[ReceiptCount];
    Deps _d;
    uint32_t _identityGeneration = 0, _visibilityGeneration = 1;
    bool _accepting = false, _polling = false, _bindReceiptHook = true;
};
