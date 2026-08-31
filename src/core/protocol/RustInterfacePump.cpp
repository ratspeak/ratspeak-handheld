
#include "protocol/RustInterfacePump.h"
#include "transport/LoRaInterface.h"
#include "transport/RnsAutoInterface.h"
#include "transport/TCPClientInterface.h"
#include "transport/WiFiInterface.h"
#include <Arduino.h>

namespace {
// "Live" = a TX now would actually reach someone (mirrors hasUsableAnnounceTransport).
inline bool autoLive(RnsAutoInterface* a) { return a && a->isOnline() && a->peerCount() > 0; }
inline bool wifiApLive(WiFiInterface* w) {
    return w && w->isAPActive() && w->getClientCount() > 0;
}
}  // namespace

void RustInterfacePump::begin(rs_handheld_rns_t* ctx, RustClock* clock) {
    _ctx = ctx;
    _clock = clock;
}

void RustInterfacePump::attachLoRa(LoRaInterface* lora) {
    _lora = lora;
    if (_lora) {
        _lora->setRawSink([this](const uint8_t* data, size_t len) {
            ingest(data, len, LORA_IFACE_ID);
        });
    }
}

int RustInterfacePump::attachTcp(TCPClientInterface* tcp) {
    if (!tcp || _tcpCount >= MAX_TCP) return -1;
    uint8_t id = TCP_IFACE_BASE + (uint8_t)_tcpCount;
    _tcp[_tcpCount++] = tcp;
    tcp->setRawSink([this, id](const uint8_t* data, size_t len) {
        ingest(data, len, id);
    });
    return id;
}

void RustInterfacePump::attachAuto(RnsAutoInterface* autoIface) {
    _auto = autoIface;
    if (_auto) {
        _auto->setRawSink([this](const uint8_t* data, size_t len) {
            ingest(data, len, AUTO_IFACE_ID);
        });
    }
}

void RustInterfacePump::attachWifiAp(WiFiInterface* wifiAp) {
    _wifiAp = wifiAp;
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
    if (_lora) { _lora->setRawSink(nullptr); _lora = nullptr; }
    detachTcpAll();
    if (_auto) { _auto->setRawSink(nullptr); _auto = nullptr; }
    if (_wifiAp) { _wifiAp->setRawSink(nullptr); _wifiAp = nullptr; }
    _sink = nullptr;
    _ctx = nullptr;
    _clock = nullptr;
    _pendingLen = 0;
    _pendingTargets = 0;
}

void RustInterfacePump::detachTcpAll() {
    for (size_t i = 0; i < _tcpCount; i++) {
        if (_tcp[i]) _tcp[i]->setRawSink(nullptr);
        _tcp[i] = nullptr;
    }
    _tcpCount = 0;
    for (uint8_t id = TCP_IFACE_BASE; id < TCP_IFACE_BASE + MAX_TCP; ++id)
        _pendingTargets &= ~(1u << id);
}

void RustInterfacePump::loop() {
    if (!_ctx || !_clock) return;
    // LoRa RX/TX driver pass (raw sink feeds ingest inline). TCP clients are
    // looped by the mains' shared WiFi/TCP section; their sinks land here too.
    if (_lora) _lora->loop();
    // Table expiry + due announce-rebroadcast dispatch, then drain anything the
    // node queued (rebroadcasts, path responses).
    rs_handheld_rns_tick(_ctx, _clock->nowMs());
    drainOutbound();
}

void RustInterfacePump::ingest(const uint8_t* data, size_t len, uint8_t ifaceId) {
    if (!_ctx || !_clock || !data || len == 0) return;
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
            size_t tagLen;
            if (rs_handheld_rns_take_own_path_request_tag(_ctx, _pathRequestTag, &tagLen) ==
                    RS_HANDHELD_OK &&
                tagLen > 0 && tagLen <= sizeof(_pathRequestTag)) {
                if (_sink) _sink->onOwnPathRequest(ifaceId, _pathRequestTag, tagLen);
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
            // Valid announce for a non-lxmf.delivery aspect (lxst.telephony, lxmf.propagation, …):
            // path learned, but NOT surfaced as a contact. _event is not filled.
            _counters.rxAccepted++;
            _counters.rxAnnounces++;
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

bool RustInterfacePump::sendTo(uint8_t ifaceId, const uint8_t* data, size_t len) {
    if (!data || len == 0) return false;
    bool sent = false;
    if (ifaceId == LORA_IFACE_ID && _lora && _lora->isOnline()) {
        sent = _lora->sendRaw(data, len);
    }
    for (size_t i = 0; i < _tcpCount; i++) {
        if (ifaceId == TCP_IFACE_BASE + (uint8_t)i && _tcp[i] && _tcp[i]->isConnected()) {
            sent = _tcp[i]->sendRaw(data, len);
            break;
        }
    }
    if (ifaceId == AUTO_IFACE_ID && autoLive(_auto)) {
        sent = _auto->sendRaw(data, len);
    }
    if (ifaceId == WIFI_AP_IFACE_ID && wifiApLive(_wifiAp)) {
        sent = _wifiAp->sendRaw(data, len);
    }
    if (sent) _counters.txFrames++;
    else _counters.txDropped++;
    return sent;
}

bool RustInterfacePump::sendAll(const uint8_t* data, size_t len) {
    return sendAllExcept(data, len, UINT8_MAX);
}

bool RustInterfacePump::sendAllExcept(const uint8_t* data, size_t len, uint8_t excludedIface) {
    if (!data || len == 0) return false;
    bool sent = false;
    if (_lora && _lora->isOnline() && excludedIface != LORA_IFACE_ID) {
        sent |= _lora->sendRaw(data, len);
    }
    for (size_t i = 0; i < _tcpCount; i++) {
        const uint8_t id = TCP_IFACE_BASE + (uint8_t)i;
        if (!_tcp[i] || !_tcp[i]->isConnected() || excludedIface == id) continue;
        sent |= _tcp[i]->sendRaw(data, len);
    }
    if (autoLive(_auto) && excludedIface != AUTO_IFACE_ID) {
        sent |= _auto->sendRaw(data, len);
    }
    if (wifiApLive(_wifiAp) && excludedIface != WIFI_AP_IFACE_ID) {
        sent |= _wifiAp->sendRaw(data, len);
    }
    if (sent) _counters.txFrames++;
    else _counters.txDropped++;
    return sent;
}

void RustInterfacePump::drainOutbound() {
    if (!_ctx) return;
    for (int guard = 0; guard < 32; guard++) {
        if (_pendingTargets) {
            for (uint8_t id = 0; id <= WIFI_AP_IFACE_ID; ++id) {
                if ((_pendingTargets & (1u << id)) && sendTo(id, _txBuf, _pendingLen))
                    _pendingTargets &= ~(1u << id);
            }
            // Retain one popped Rust packet until every selected interface accepts it.
            // Successful interfaces are never sent the same packet again while LoRa is busy.
            if (_pendingTargets && millis() - _pendingSince <=
                    interfaceTxWaitMs(LORA_IFACE_ID, 2) + 40000) return;
            _pendingTargets = 0;
            _pendingLen = 0;
        }
        size_t outLen = 0;
        uint8_t originIface = 0;
        int32_t reason = 0;
        rs_handheld_status_t st = rs_handheld_rns_poll_outbound(
            _ctx, _txBuf, sizeof(_txBuf), &outLen, &originIface, &reason);
        if (st != RS_HANDHELD_OK || outLen == 0) break;
        transmit(_txBuf, outLen, originIface, reason);
    }
}

void RustInterfacePump::transmit(const uint8_t* data, size_t len,
                                 uint8_t originIface, int32_t reason) {
    // Rust supplies both provenance and reason. A path response returns only on
    // the requester's interface; an endpoint-originated path request fans out;
    // relay/rebroadcast traffic is sent everywhere except where it arrived.
    uint8_t targets = 0;
    if (loraOnline()) targets |= 1u << LORA_IFACE_ID;
    for (size_t i = 0; i < _tcpCount; ++i)
        if (_tcp[i] && _tcp[i]->isConnected()) targets |= 1u << (TCP_IFACE_BASE + i);
    if (autoLive(_auto)) targets |= 1u << AUTO_IFACE_ID;
    if (wifiApLive(_wifiAp)) targets |= 1u << WIFI_AP_IFACE_ID;
    if (reason == RS_HANDHELD_TX_PATH_RESPONSE)
        targets &= originIface <= WIFI_AP_IFACE_ID ? (1u << originIface) : 0;
    else if (reason != RS_HANDHELD_TX_PATH_REQUEST && originIface <= WIFI_AP_IFACE_ID)
        targets &= ~(1u << originIface);
    for (uint8_t id = 0; id <= WIFI_AP_IFACE_ID; ++id) {
        if ((targets & (1u << id)) && !sendTo(id, data, len)) _pendingTargets |= 1u << id;
    }
    _pendingLen = len;
    _pendingSince = millis();
}
