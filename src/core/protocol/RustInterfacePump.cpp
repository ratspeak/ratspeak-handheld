
#include "protocol/RustInterfacePump.h"
#include "transport/LoRaInterface.h"
#include "transport/RnsAutoInterface.h"
#include "transport/TCPClientInterface.h"
#include "transport/WiFiInterface.h"
#include <Arduino.h>

namespace {
static_assert(handheld::TxLease::TokenBytes == RS_HANDHELD_TX_LIFETIME_BYTES,
              "Keep the driver token in sync with the Rust ABI");
// "Live" = a TX now would actually reach someone (mirrors hasUsableAnnounceTransport).
inline bool autoLive(RnsAutoInterface* a) { return a && a->isOnline() && a->peerCount() > 0; }
inline bool wifiApLive(WiFiInterface* w) {
    return w && w->isAPActive() && w->getClientCount() > 0;
}
}  // namespace

void RustInterfacePump::begin(rs_handheld_rns_t* ctx, RustClock* clock) {
    if (_changingReceiptHook || _stopping || _draining || !_drainEpoch) return;
    if (_drainEpoch == UINT32_MAX) { stop(); return; }
    ++_drainEpoch;
    _pending = {};
    for (uint8_t id = 0; id <= WIFI_AP_IFACE_ID; ++id) invalidateInterface(id);
    _ctx = ctx;
    _clock = clock;
}

void RustInterfacePump::attachLoRa(LoRaInterface* lora) {
    if (_changingReceiptHook || _stopping) return;
    _changingReceiptHook = true;
    const auto* context = _ctx;
    const auto* clock = _clock;
    const auto generation = _generations[LORA_IFACE_ID];
    auto* previous = _lora;
    _lora = nullptr; // A terminal callback may stop us recursively.
    if (previous) {
        previous->setRawSink(nullptr);
        previous->setTxValidator(nullptr, nullptr);
        previous->setReceiptHook(nullptr, nullptr);
    }
    if (_ctx != context || _clock != clock || _generations[LORA_IFACE_ID] != generation) {
        _receiptHook = nullptr;
        _receiptContext = nullptr;
        _changingReceiptHook = false;
        return;
    }
    invalidateInterface(LORA_IFACE_ID);
    _lora = lora;
    _driverGenerations[LORA_IFACE_ID] = lora ? lora->generation() : 0;
    if (_lora) {
        lora->setReceiptHook(this, receiptHook);
        // A replacement driver can return its previous owner's receipts too.
        // If one stopped us, clear any hook installed by that setter's return.
        if (_lora != lora || _ctx != context || _clock != clock) {
            lora->setRawSink(nullptr);
            lora->setTxValidator(nullptr, nullptr);
            lora->setReceiptHook(nullptr, nullptr);
            _receiptHook = nullptr;
            _receiptContext = nullptr;
            _changingReceiptHook = false;
            return;
        }
        lora->setTxValidator(this, [](void* context, const handheld::TxLease& lease) {
            return static_cast<RustInterfacePump*>(context)->leaseLive(lease);
        });
        lora->setRawSink([this](const uint8_t* data, size_t len) {
            ingest(data, len, LORA_IFACE_ID);
        });
    }
    _changingReceiptHook = false;
}

int RustInterfacePump::attachTcp(TCPClientInterface* tcp) {
    if (!tcp || _tcpCount >= MAX_TCP) return -1;
    uint8_t id = TCP_IFACE_BASE + (uint8_t)_tcpCount;
    invalidateInterface(id);
    _tcp[_tcpCount++] = tcp;
    _driverGenerations[id] = tcp->generation();
    tcp->setRawSink([this, id](const uint8_t* data, size_t len) {
        ingest(data, len, id);
    });
    return id;
}

void RustInterfacePump::attachAuto(RnsAutoInterface* autoIface) {
    if (_auto) _auto->setRawSink(nullptr);
    invalidateInterface(AUTO_IFACE_ID);
    _auto = autoIface;
    _driverGenerations[AUTO_IFACE_ID] = _auto ? _auto->generation() : 0;
    if (_auto) {
        _auto->setRawSink([this](const uint8_t* data, size_t len) {
            ingest(data, len, AUTO_IFACE_ID);
        });
    }
}

void RustInterfacePump::attachWifiAp(WiFiInterface* wifiAp) {
    if (_wifiAp) _wifiAp->setRawSink(nullptr);
    invalidateInterface(WIFI_AP_IFACE_ID);
    _wifiAp = wifiAp;
    _driverGenerations[WIFI_AP_IFACE_ID] = _wifiAp ? _wifiAp->generation() : 0;
    if (_wifiAp) {
        _wifiAp->setRawSink([this](const uint8_t* data, size_t len) {
            ingest(data, len, WIFI_AP_IFACE_ID);
        });
    }
}

bool RustInterfacePump::loraOnline() const { return _lora && _lora->isOnline(); }

uint32_t RustInterfacePump::interfaceBitrate(uint8_t ifaceId) const {
    if (ifaceId == LORA_IFACE_ID && _lora) return _lora->bitrate();
    return 0;
}

int32_t RustInterfacePump::interfaceMode(uint8_t ifaceId) const {
    if (ifaceId == LORA_IFACE_ID) return RS_HANDHELD_IFACE_MODE_ROAMING;
    if (ifaceId >= TCP_IFACE_BASE && ifaceId < TCP_IFACE_BASE + MAX_TCP)
        return RS_HANDHELD_IFACE_MODE_FULL;
    if (ifaceId == AUTO_IFACE_ID) return RS_HANDHELD_IFACE_MODE_FULL;
    if (ifaceId == WIFI_AP_IFACE_ID) return RS_HANDHELD_IFACE_MODE_GATEWAY;
    return RS_HANDHELD_IFACE_MODE_FULL;
}

uint32_t RustInterfacePump::interfaceTxWaitMs(uint8_t ifaceId, uint32_t packets) const {
    return ifaceId == LORA_IFACE_ID && _lora ? _lora->txWaitBudgetMs(packets) : 0;
}

int RustInterfacePump::lastLoraRssi() const { return _lora ? _lora->lastRxRssi() : 0; }

float RustInterfacePump::lastLoraSnr() const { return _lora ? _lora->lastRxSnr() : 0; }

void RustInterfacePump::stop() {
    // Prevent callbacks from admitting more work while old queued receipts are
    // returned to their owner. That owner and its hook still exist here.
    if (_stopping) return;
    _stopping = true;
    // Retire recursive requesters while the Rust context still exists. Driver
    // callbacks below may reenter stop after _ctx has been detached.
    if (_ctx) for (uint8_t id = 0; id <= WIFI_AP_IFACE_ID; ++id)
        rs_handheld_rns_outbound_retire_interface(_ctx, id);
    if (_drainEpoch) _drainEpoch = _drainEpoch == UINT32_MAX ? 0 : _drainEpoch + 1;
    const bool changing = _changingReceiptHook;
    _ctx = nullptr;
    _clock = nullptr;
    _changingReceiptHook = true;
    auto* lora = _lora;
    _lora = nullptr;
    if (lora) {
        lora->setRawSink(nullptr);
        lora->setTxValidator(nullptr, nullptr);
        lora->setReceiptHook(nullptr, nullptr);
    }
    detachTcpAll();
    if (_auto) { _auto->setRawSink(nullptr); _auto = nullptr; }
    if (_wifiAp) { _wifiAp->setRawSink(nullptr); _wifiAp = nullptr; }
    _sink = nullptr;
    _ctx = nullptr;
    _clock = nullptr;
    // An interrupted attachment/hook transition may still be returning the
    // detached driver's remaining queued receipts. Its outer frame clears the
    // callback only after those notifications finish.
    if (!changing) {
        _receiptHook = nullptr;
        _receiptContext = nullptr;
    }
    _changingReceiptHook = changing;
    // A synchronous send callback may stop the owner while its driver still
    // reads this copy. Retire ownership without overwriting the offered bytes.
    _pending.targets = 0;
    _pending.length = 0;
    for (uint8_t id = 0; id <= WIFI_AP_IFACE_ID; ++id) invalidateInterface(id);
    _stopping = false;
}

void RustInterfacePump::detachTcpAll() {
    for (size_t i = 0; i < _tcpCount; i++) {
        if (_tcp[i]) _tcp[i]->setRawSink(nullptr);
        _tcp[i] = nullptr;
    }
    _tcpCount = 0;
    for (uint8_t id = TCP_IFACE_BASE; id < TCP_IFACE_BASE + MAX_TCP; ++id)
        invalidateInterface(id);
}

bool RustInterfacePump::pollRadioBeforeBlockingWork() {
    return !_lora || _lora->pollBeforeBlockingWork();
}

void RustInterfacePump::loop() {
    if (!_ctx || !_clock) return;
    const auto* context = _ctx;
    const auto* clock = _clock;
    const auto generation = _generations[LORA_IFACE_ID];
    // LoRa RX/TX driver pass (raw sink feeds ingest inline). TCP clients are
    // looped by the mains' shared WiFi/TCP section; their sinks land here too.
    if (_lora) _lora->loop();
    if (!_ctx || !_clock || _ctx != context || _clock != clock ||
        _generations[LORA_IFACE_ID] != generation) return;
    // Table expiry + due announce-rebroadcast dispatch, then drain anything the
    // node queued (rebroadcasts, path responses).
    if (!syncInterfaceFacts()) return;
    rs_handheld_rns_tick(_ctx, _clock->nowMs());
    drainOutbound();
}

void RustInterfacePump::ingest(const uint8_t* data, size_t len, uint8_t ifaceId) {
    if (!_ctx || !_clock || !data || len == 0) return;
    if (!syncInterfaceFacts()) { ++_counters.rxErrors; return; }
    _counters.rxFrames++;
    int32_t action = RS_HANDHELD_INGEST_DROPPED;
    rs_handheld_status_t st = rs_handheld_rns_packet_ingest_with_mode(
        _ctx, data, len, ifaceId, interfaceMode(ifaceId), _clock->nowMs(), &action, &_event,
        &_local);
    if (st != RS_HANDHELD_OK) {
        _counters.rxErrors++;
        return;
    }
    switch (action) {
        case RS_HANDHELD_INGEST_DUPLICATE:
            _counters.rxDuplicates++;
            break;
        case RS_HANDHELD_INGEST_DROPPED:
            _counters.rxDropped++;
            break;
        case RS_HANDHELD_INGEST_LOCAL_FRAME:
            _counters.rxAccepted++;
            _counters.rxLocal++;
            // Endpoint delivery (LXMF data/proof, link handshake, resource) -> engines.
            if (_sink) _sink->onLocalFrame(_local, ifaceId);
            break;
        case RS_HANDHELD_INGEST_PATH_REQUEST_SELF: {
            _counters.rxAccepted++;
            _counters.rxPathReqSelf++;
            // A peer's cached path to us expired; re-announce our dest (throttled in ProtocolRuntime).
            size_t tagLen; uint32_t endpoint = 0;
            if (rs_handheld_rns_take_own_path_request(_ctx, _pathRequestTag, &tagLen, &endpoint) ==
                    RS_HANDHELD_OK &&
                tagLen > 0 && tagLen <= sizeof(_pathRequestTag)) {
                if (_sink) _sink->onEndpointPathRequest(ifaceId, _pathRequestTag, tagLen, endpoint);
            } else {
                _counters.rxErrors++;
            }
            break;
        }
        case RS_HANDHELD_INGEST_LEARNED_ANNOUNCE:
        case RS_HANDHELD_INGEST_SCHEDULED_ANNOUNCE:
            _counters.rxAccepted++;
            _counters.rxAnnounces++;
            // Contact learning + KeyMap continuity is the C++ AnnounceManager's job:
            // OK from ingest means "signature + binding valid", not "safe to learn".
            if (_sink) _sink->onAnnounceEvent(_event, ifaceId);
            break;
        case RS_HANDHELD_INGEST_ANNOUNCE_OTHER:
            // Valid announce for another aspect (lxst.telephony, NomadNet, …):
            // path learned, but NOT surfaced as a contact. _event is not filled.
            _counters.rxAccepted++;
            _counters.rxAnnounces++;
            break;
        case RS_HANDHELD_INGEST_ANNOUNCE_PROPAGATION:
            _counters.rxAccepted++;
            _counters.rxAnnounces++;
            if (_sink) _sink->onPropagationAnnounce(_event, ifaceId);
            break;
        case RS_HANDHELD_INGEST_ANNOUNCE_VOICE:
            _counters.rxAccepted++;
            _counters.rxAnnounces++;
            if (_sink) _sink->onVoiceAnnounce(_event, ifaceId);
            break;
        case RS_HANDHELD_INGEST_ANNOUNCE_RRC:
            _counters.rxAccepted++;
            _counters.rxAnnounces++;
            if (_sink) _sink->onRrcAnnounce(_event, ifaceId);
            break;
        case RS_HANDHELD_INGEST_ANNOUNCE_IGNORED:
            // Signature/binding may be valid, but freshness rejected it. Never surface the stale
            // event to KeyMap/contact/peer-ratchet policy and never count it as accepted.
            _counters.rxAnnounceIgnored++;
            _counters.rxDropped++;
            break;
        default:
            _counters.rxAccepted++;
            break;
    }
    // Ingest may queue rebroadcasts/path responses — flush them promptly.
    drainOutbound();
}

void RustInterfacePump::invalidateInterface(uint8_t id) {
    if (id > WIFI_AP_IFACE_ID) return;
    // Zero is an exhausted generation; never wrap and authorize ancient work.
    if (_generations[id]) _generations[id] = _generations[id] == UINT32_MAX ? 0 : _generations[id] + 1;
    const auto previousTargets = _pending.targets;
    _pending.targets &= ~(1u << id);
    if (previousTargets && !_pending.targets) ++_counters.txDropped;
    if (_ctx) rs_handheld_rns_outbound_retire_interface(_ctx, id);
}

uint8_t RustInterfacePump::liveTargets() const {
    uint8_t targets = 0;
    if (loraOnline()) targets |= 1u << LORA_IFACE_ID;
    for (size_t i = 0; i < _tcpCount; ++i)
        if (_tcp[i] && _tcp[i]->isConnected()) targets |= 1u << (TCP_IFACE_BASE + i);
    if (autoLive(_auto)) targets |= 1u << AUTO_IFACE_ID;
    if (wifiApLive(_wifiAp)) targets |= 1u << WIFI_AP_IFACE_ID;
    return targets;
}

uint32_t RustInterfacePump::syncGeneration(uint8_t id) {
    if (id > WIFI_AP_IFACE_ID) return 0;
    uint32_t driver = 0;
    if (id == LORA_IFACE_ID && _lora) driver = _lora->generation();
    else if (id >= TCP_IFACE_BASE && id < TCP_IFACE_BASE + _tcpCount && _tcp[id - TCP_IFACE_BASE])
        driver = _tcp[id - TCP_IFACE_BASE]->generation();
    else if (id == AUTO_IFACE_ID && _auto) driver = _auto->generation();
    else if (id == WIFI_AP_IFACE_ID && _wifiAp) driver = _wifiAp->generation();
    if (_driverGenerations[id] != driver) {
        _driverGenerations[id] = driver;
        invalidateInterface(id);
    }
    return driver ? _generations[id] : 0;
}

bool RustInterfacePump::syncInterfaceFacts() {
    if (!_ctx || !_clock) return false;
    const auto live = liveTargets();
    for (uint8_t id = 0; id <= WIFI_AP_IFACE_ID; ++id) {
        const auto generation = syncGeneration(id);
        if (rs_handheld_rns_update_interface(_ctx, id, generation, interfaceMode(id),
                interfaceBitrate(id), generation && (live & (1u << id)) ? 1 : 0) != RS_HANDHELD_OK)
            return false;
    }
    return true;
}

uint32_t RustInterfacePump::interfaceGeneration(uint8_t id) {
    if (!_ctx || !_clock || id > WIFI_AP_IFACE_ID || !(liveTargets() & (1u << id))) return 0;
    return syncGeneration(id);
}

bool RustInterfacePump::receiptHook(void* context, handheld::TxReceipt receipt,
                                   handheld::TxReceiptEvent event) {
    auto* self = static_cast<RustInterfacePump*>(context);
    return self->_receiptHook && self->_receiptHook(self->_receiptContext, receipt, event);
}

void RustInterfacePump::setReceiptHook(handheld::TxReceiptHook hook, void* context) {
    if (_changingReceiptHook || _stopping || !_ctx || !_clock) return;
    const auto* previousContext = _ctx;
    const auto* previousClock = _clock;
    const auto generation = _generations[LORA_IFACE_ID];
    _changingReceiptHook = true;
    if (_lora) _lora->setReceiptHook(nullptr, nullptr);
    if (_ctx == previousContext && _clock == previousClock && _generations[LORA_IFACE_ID] == generation) {
        _receiptHook = hook;
        _receiptContext = context;
        if (_lora) _lora->setReceiptHook(this, receiptHook);
    } else {
        _receiptHook = nullptr;
        _receiptContext = nullptr;
    }
    _changingReceiptHook = false;
}

bool RustInterfacePump::captureLease(uint8_t id, const uint8_t* data, size_t len,
                                      handheld::TxLease& lease) {
    lease = {};
    if (!_ctx || !_clock || !data || !len || id > WIFI_AP_IFACE_ID ||
        !(liveTargets() & (1u << id))) return false;
    lease.generation = syncGeneration(id);
    lease.interfaceId = id;
    if (!lease.generation || rs_handheld_rns_capture_outbound_lifetime(
            _ctx, data, len, _clock->nowMs(), lease.token) != RS_HANDHELD_OK) {
        lease = {};
        return false;
    }
    return true;
}

bool RustInterfacePump::captureLeaseAt(uint8_t id, const uint8_t* data, size_t len,
                                      uint64_t bornMs, uint32_t maxWaitMs,
                                      handheld::TxLease& lease) {
    lease = {};
    const uint32_t generation = interfaceGeneration(id);
    if (!generation || !data || !len) return false;
    if (rs_handheld_rns_capture_outbound_lifetime_at(_ctx, data, len, _clock->nowMs(),
            bornMs, maxWaitMs, lease.token) != RS_HANDHELD_OK) return false;
    lease.interfaceId = id;
    lease.generation = generation;
    return true;
}

bool RustInterfacePump::leaseLive(const handheld::TxLease& lease) {
    const uint8_t id = lease.interfaceId;
    if (!_ctx || !_clock || !lease.generation || id > WIFI_AP_IFACE_ID ||
        lease.generation != syncGeneration(id) || !(liveTargets() & (1u << id))) return false;
    if (lease.receipt().valid() &&
        !receiptHook(this, lease.receipt(), handheld::TxReceiptEvent::Validate)) return false;
    const uint64_t now = _clock->nowMs();
    int32_t live = 0;
    return rs_handheld_rns_outbound_lifetime_is_live(_ctx, lease.token, id, now, &live) ==
               RS_HANDHELD_OK && live != 0;
}

bool RustInterfacePump::sendLeased(const uint8_t* data, size_t len, const handheld::TxLease& lease) {
    if (lease.receipt().valid()) {
        const auto offer = offerReceipt(data, len, lease);
        return offer == handheld::TxOffer::Started || offer == handheld::TxOffer::Queued;
    }
    const bool sent = writeLeased(data, len, lease);
    if (sent) ++_counters.txFrames;
    else ++_counters.txDropped;
    return sent;
}

handheld::TxOffer RustInterfacePump::offerReceipt(const uint8_t* data, size_t len,
                                                const handheld::TxLease& lease) {
    using handheld::TxOffer;
    if (_changingReceiptHook) return TxOffer::Blocked;
    if (!data || !len || len > 500 || !lease.receipt().valid() || !leaseLive(lease)) {
        ++_counters.txDropped;
        receiptHook(this, lease.receipt(), handheld::TxReceiptEvent::Dropped);
        return TxOffer::Rejected;
    }
    if (lease.interfaceId == LORA_IFACE_ID) {
        const auto offer = _lora->offerLeased(data, len, lease);
        if (offer == TxOffer::Queued || offer == TxOffer::Started) ++_counters.txFrames;
        else if (offer == TxOffer::Rejected) ++_counters.txDropped;
        return offer;
    }
    if (writeLeased(data, len, lease)) {
        ++_counters.txFrames;
        receiptHook(this, lease.receipt(), handheld::TxReceiptEvent::Started);
        return TxOffer::Started;
    }
    // A failed socket write may retire its connection. Capacity alone retains
    // ownership with the producer; a lost session terminates this receipt.
    if (!leaseLive(lease)) {
        ++_counters.txDropped;
        receiptHook(this, lease.receipt(), handheld::TxReceiptEvent::Dropped);
        return TxOffer::Rejected;
    }
    return TxOffer::Blocked;
}

bool RustInterfacePump::sendRetainedTo(uint8_t id, const uint8_t* data, size_t len,
                                      const handheld::TxLease& retained) {
    handheld::TxLease lease = retained;
    lease.interfaceId = id;
    lease.generation = _ctx && _clock ? syncGeneration(id) : 0;
    return sendLeased(data, len, lease);
}

bool RustInterfacePump::writeLeased(const uint8_t* data, size_t len, const handheld::TxLease& lease) {
    if (!data || !len || len > 500 || !leaseLive(lease)) return false;
    const uint8_t id = lease.interfaceId;
    bool sent = false;
    if (id == LORA_IFACE_ID) sent = _lora->sendLeased(data, len, lease);
    else if (id >= TCP_IFACE_BASE && id < TCP_IFACE_BASE + _tcpCount)
        sent = _tcp[id - TCP_IFACE_BASE]->sendRaw(data, len);
    else if (id == AUTO_IFACE_ID) sent = _auto->sendRaw(data, len);
    else if (id == WIFI_AP_IFACE_ID) sent = _wifiAp->sendRaw(data, len);
    return sent;
}

bool RustInterfacePump::sendTo(uint8_t id, const uint8_t* data, size_t len) {
    handheld::TxLease lease;
    if (!captureLease(id, data, len, lease)) { ++_counters.txDropped; return false; }
    return sendLeased(data, len, lease);
}

bool RustInterfacePump::sendAll(const uint8_t* data, size_t len) {
    return sendAllExcept(data, len, UINT8_MAX);
}

bool RustInterfacePump::sendAllExcept(const uint8_t* data, size_t len, uint8_t excludedIface) {
    if (!_ctx || !_clock || !data || !len) return false;
    handheld::TxLease lease;
    if (rs_handheld_rns_capture_outbound_lifetime(_ctx, data, len, _clock->nowMs(), lease.token) !=
            RS_HANDHELD_OK) return false;
    const uint8_t targets = liveTargets() &
        (excludedIface <= WIFI_AP_IFACE_ID ? ~(1u << excludedIface) : UINT8_MAX);
    bool sent = false;
    for (uint8_t id = 0; id <= WIFI_AP_IFACE_ID; ++id) {
        if (id == excludedIface || !(targets & (1u << id))) continue;
        lease.interfaceId = id;
        lease.generation = syncGeneration(id);
        sent |= writeLeased(data, len, lease);
    }
    if (sent) ++_counters.txFrames;
    else ++_counters.txDropped;
    return sent;
}

void RustInterfacePump::drainOutbound() {
    if (!_ctx || !_clock || _draining || _changingReceiptHook || !_drainEpoch) return;
    _draining = true;
    struct Guard { bool& active; ~Guard() { active = false; } } guard{_draining};
    auto* const context = _ctx;
    const auto epoch = _drainEpoch;
    const auto alive = [&] { return _ctx == context && _clock && _drainEpoch == epoch; };
    uint8_t blocked = 0;
    uint64_t after = 0;
    bool heldServiced = false;
    for (int attempt = 0; attempt < 32; ++attempt) {
        PendingFrame* frame = &_pending;
        bool queued = false;
        if (!_pending.targets || heldServiced) {
            const bool transfer = !_pending.targets;
            // Observe every driver incarnation before binding any new queue row.
            // Initial attachment records its driver generation before RX hooks.
            const uint8_t live = liveTargets();
            for (uint8_t id = 0; id <= WIFI_AP_IFACE_ID; ++id) {
                const auto generation = syncGeneration(id);
                _peek.generations[id] = live & (1u << id) ? generation : 0;
            }
            if (!alive()) return;
            frame = transfer ? &_pending : &_peek;
            size_t length = 0;
            const auto status = rs_handheld_rns_outbound_select(context,
                transfer ? 0 : after, transfer ? 0 : blocked, _peek.generations,
                transfer ? 1 : 0, frame->data, sizeof(frame->data), &length,
                &_peekIdentity, &frame->targets, frame->generations, frame->token);
            if (status != RS_HANDHELD_OK || !length) return;
            frame->length = length;
            queued = !transfer;
            if (queued) after = _peekIdentity;
            else heldServiced = false;
        }
        const uint64_t identity = _peekIdentity;
        handheld::TxLease lease;
        memcpy(lease.token, frame->token, sizeof(lease.token));
        for (uint8_t id = 0; id <= WIFI_AP_IFACE_ID; ++id) {
            const uint8_t bit = 1u << id;
            if (!(frame->targets & bit) || (blocked & bit)) continue;
            lease.interfaceId = id;
            lease.generation = frame->generations[id];
            bool completed = !leaseLive(lease);
            if (!alive()) return;
            if (!completed) {
                completed = sendLeased(frame->data, frame->length, lease);
                if (!alive()) return; // callback may have freed the old context
                if (!completed) completed = !leaseLive(lease);
                if (!alive()) return;
            }
            if (completed) {
                frame->targets &= ~bit;
                if (queued) rs_handheld_rns_outbound_ack(context, identity, bit);
            } else {
                blocked |= bit; // preserve per-interface FIFO while others advance
            }
        }
        if (!queued) heldServiced = _pending.targets != 0;
    }
}


bool RustInterfacePump::admitsVoice(uint8_t iface, size_t bytes, uint32_t interval, uint8_t hops) const {
    if (!interfaceOnline(iface)) return false;
    return iface != LORA_IFACE_ID || (_lora && _lora->admitsVoice(bytes, interval, hops));
}
