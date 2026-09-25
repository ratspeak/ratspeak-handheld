#pragma once

#include "protocol/ProtocolBackend.h"
#include "protocol/RustAnnouncePolicy.h"
#include "protocol/RustClock.h"
#include "protocol/RustInterfacePump.h"
#include "protocol/RustKeyMap.h"
#include "protocol/RustLxmfEngine.h"
#include "runtime/ResourceBudget.h"
#include "protocol/RustRatchetStore.h"
#include "protocol/RustLinkManager.h"
#include "protocol/RustResourceEngine.h"
#include "ratspeak_protocol.h"

class FlashStore;
class SDStore;
class IdentityManager;
class MessageStore;
class AnnounceManager;

// Device-side coordinator for the Rust protocol core. C++ owns hardware,
// persistence adapters, scheduling, and UI integration; the FFI owns all
// Reticulum and LXMF wire parsing and construction.
class ProtocolRuntime : public ProtocolBackend, public RustPumpSink {
public:
    ~ProtocolRuntime() override;

    // Boot order: flash + identityMgr +
    // messageStore must be begun first. profile = RS_HANDHELD_PROFILE_* for
    // this board's artifact; nodeHeapCaps = heap_caps_malloc caps for the
    // transport node (SPIRAM on tdeck/tpager SMALL, INTERNAL on cardputer
    // MICRO). Alloc failure -> honest not-ready, never a crash. announceMgr is
    // the contact bridge target for validated inbound announces.
    bool begin(FlashStore* flash, SDStore* sd, IdentityManager* idMgr, MessageStore* store,
               AnnounceManager* announceMgr, int32_t profile, uint32_t nodeHeapCaps);
    void end();
    // Nonblocking message barrier. Lifecycle consumers keep pollReceive running
    // and consume their initial-send results while storage settles. end()
    // requires both incoming and outgoing ownership drained.
    void stopReceive();
    void pollReceive();
    bool receiveDrained() const { return _lxmf.incoming().drained() && _lxmf.drained(); }

    // Retains the context and the caller-owned radio until both message owners
    // and the already-started radio burst settle. No RX, scheduler or metadata
    // retries run here. The caller still polls MessageStore and result consumers.
    void beginMaintenance(LoRaInterface& radio);
    void pollMaintenance();
    bool maintenanceDrained() const;
    bool maintenanceFailed() const;

    RustInterfacePump& pump() { return _pump; }
    bool lifecycleReady() const { return _identityLoaded && _nodeOpen; }
    bool transportStats(rs_handheld_transport_stats_t& out) const;
    static const char* versionString() { return rs_handheld_rns_version(); }

    // The announce contact bridge target; created after begin() in the mains.
    void setAnnounceManager(AnnounceManager* m) { _announceMgr = m; }
    // Seed the cached announce app_data at boot so a path response sent BEFORE our
    // first announce still carries [name, stamp_cost, supported_functionality] —
    // an empty-app_data path response would overwrite our entry on peers (display
    // name lost) and revert Python LXMF to auto_compress=True (bz2 we must reject).
    void seedAnnounceAppData(const uint8_t* appData, size_t len);
    const uint8_t* localDestHash() const { return _destHash; }
    const RustInterfacePump::Counters& pumpCounters() const { return _pump.counters(); }

    // ProtocolBackend
    const char* backendName() const override { return "rust"; }

    void loop() override;
    bool pollRadioBeforeBlockingWork() override;
    // Shutdown / explicit persist: force the peer-ratchet table out (the ring is written
    // synchronously at rotation, so it is never pending here).
    bool persistData() override;

    String identityHash() const override { return _identityHashStr; }
    String identityHashHex() const override { return _identityLoaded ? _identityHashHex : String(); }
    String destinationHashHex() const override { return _destHashHex; }
    String destinationHashStr() const override { return _destHashStr; }

    bool isTransportActive() const override { return _nodeOpen; }
    size_t pathCount() const override;
    size_t linkCount() const override;

    AnnounceResult announce(const uint8_t* appData, size_t len) override;
    unsigned long lastAnnounceTime() const override { return _lastAnnounceMs; }
    uint32_t announceFilterCount() const override;

    handheld::outgoing::Submission lxmfSubmit(const uint8_t dest[16],
        const uint8_t* title, size_t titleLength, const uint8_t* content,
        size_t contentLength, bool preferLink = false) override;
    handheld::outgoing::Poll lxmfPoll(handheld::outgoing::Ticket,
        handheld::outgoing::InitialResult&) const override;
    bool lxmfAcknowledge(handheld::outgoing::Ticket) override;
    bool lxmfCancel(handheld::outgoing::Ticket) override;
    bool lxmfStatus(const handheld::storage::RecordKey&,
        handheld::outgoing::StatusView&) const override;
    uint32_t lxmfStatusRevision() const override;
    void lxmfStopAdmissions() override;
    bool lxmfDrained() const override;
    handheld::storage::Error lxmfDrainError() const override;
    bool lxmfBeginPeerDelete(const uint8_t peer[16]) override;
    void lxmfFinishPeerDelete(const uint8_t peer[16], const handheld::storage::Result&) override;
    int lxmfQueuedCount() const override { return _enginesUp ? _lxmf.queuedCount() : 0; }
    void setMessageCallback(LXMFManager::MessageCallback cb) override { _onMessage = cb; }
    void setStatusCallback(LXMFManager::StatusCallback cb) override { _statusCb = cb; }

    String publicKeyHex() const override { return _publicKeyHex; }

    // Honesty driver: true once identity loaded + node open + engines wired.
    bool protocolReady() const override { return _enginesUp; }

    uint8_t activeResourceTransfers() const override {
        return _enginesUp ? _resources.activeTransfers() : 0;
    }
    const char* deliveryBackendDetail() const override {
        return _enginesUp ? "opportunistic+link+resource"
                          : (lifecycleReady() ? "transport-only" : "not started");
    }

    // RustPumpSink
    void onAnnounceEvent(const rs_handheld_announce_event_t& ev, uint8_t ifaceId) override;
    void onLocalFrame(const rs_handheld_local_frame_t& f, uint8_t ifaceId) override;
    void onOwnPathRequest(uint8_t ifaceId, const uint8_t tag[16], size_t tagLen) override;

private:
    bool loadOrCreateIdentity(IdentityManager* idMgr);
    bool openTransport(int32_t profile, uint32_t nodeHeapCaps);
    void seedDedup(MessageStore* store);
    bool startEngines(FlashStore* flash, SDStore* sd, MessageStore* store,
                      AnnounceManager* announceMgr);
    // Construction persists wire ordering before exposing signed bytes. Sent here means built;
    // admission is separate so a refused path response retains the exact packet and lifetime.
    AnnounceResult buildAnnouncePacket(const uint8_t* appData, size_t len, uint8_t context,
                                      uint8_t* raw, size_t capacity, size_t& rawLen);
    AnnounceResult emitAnnounce(const uint8_t* appData, size_t len);
    void pollPathResponses();
    void promotePathResponse(uint8_t ifaceId, uint64_t now);

    rs_handheld_rns_t* _ctx = nullptr;
    uint8_t* _nodeBuf = nullptr;
    size_t _nodeBufLen = 0;
    int32_t _profile = RS_HANDHELD_PROFILE_SMALL;
    bool _identityLoaded = false;
    bool _nodeOpen = false;
    bool _enginesUp = false;
    enum AnnounceTiming : uint8_t { NormalPending = 2 };
    uint8_t _announceTiming = 0; // Uses the existing alignment gap after lifecycle flags.
    LoRaInterface* _maintenanceRadio = nullptr;

    uint8_t _identityHash[16] = {};
    uint8_t _destHash[16] = {};
    uint8_t _publicKey[64] = {};
    String _identityHashStr = "unknown";  // xxxx:xxxx:xxxx (micro diag format)
    String _identityHashHex;
    String _destHashHex = "unknown";
    String _destHashStr = "unknown";
    String _publicKeyHex;
    unsigned long _lastAnnounceMs = 0;

    // One owner per physical interface prevents a blocked radio/socket from holding up
    // another requester. Same-interface bursts share its forthcoming broadcast response.
    // Duplicate-tag filtering stays in Rust; the five-second interval is pacing, not loss.
    static constexpr uint32_t PATH_REQUEST_GRACE_MS = 400;
    static constexpr uint32_t PATH_RESPONSE_INTERVAL_MS = 5000;
    static constexpr uint32_t PATH_RESPONSE_MAX_AGE_MS = 30000;
    static constexpr uint32_t PATH_RESPONSE_RETRY_MS = 50;
    static constexpr size_t PATH_RESPONSE_INTERFACES = RustInterfacePump::WIFI_AP_IFACE_ID + 1;
    struct PathResponse {
        uint8_t raw[500] = {};
        handheld::TxLease lease;
        uint64_t bornMs = 0;
        uint64_t readyMs = 0;
        uint64_t packetBornMs = 0;
        uint64_t lastSentMs = 0;
        uint32_t generation = 0;
        uint16_t rawLen = 0;
        uint8_t tag[16] = {};
        uint8_t tagLen = 0;
        bool pending = false;
        bool hasLastSent = false;
        bool replay = false;
        // A fresh request cannot be satisfied by already-seen replay bytes. Retain
        // one coalesced followup, without a second packet buffer or renewable age.
        uint64_t followupBornMs = 0;
        uint8_t followupTag[16] = {};
        uint8_t followupTagLen = 0;
    } _pathResponses[PATH_RESPONSE_INTERFACES];
    static_assert(sizeof(_pathResponses) <= handheld::ResourceBudget::PathResponses,
                  "Review per-interface path-response retention budget");
    uint32_t _normalAnnouncePendingUntil = 0;
    size_t _normalAnnouncePendingLen = 0;
    // Current name/capability bytes, seeded before ingress and refreshed after
    // committed settings changes. Shared by new path responses and nonempty
    // deferred normal announces; already admitted signed packets are immutable.
    static constexpr size_t APP_DATA_MAX = 256;
    uint8_t _lastAppData[APP_DATA_MAX] = {};
    size_t _lastAppDataLen = 0;

    RustClock _clock;
    RustInterfacePump _pump;
    RustKeyMap _keymap;
    RustRatchetStore _ratchets;
    RustLinkManager _links;
    RustResourceEngine _resources;
    RustLxmfEngine _lxmf;
    AnnounceManager* _announceMgr = nullptr;
    LXMFManager::MessageCallback _onMessage;
    LXMFManager::StatusCallback _statusCb;
};
