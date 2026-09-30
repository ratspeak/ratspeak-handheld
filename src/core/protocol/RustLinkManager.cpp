
#include "protocol/RustLinkManager.h"
#include "protocol/RustClock.h"
#include "protocol/RustEntropy.h"
#include "protocol/RustKeyMap.h"
#include "protocol/RustInterfacePump.h"
#include "protocol/RustLinkTiming.h"
#include "protocol/RustLxmfEngine.h"
#include "protocol/RustResourceEngine.h"
#include "protocol/RustWire.h"
#include <Arduino.h>
#include <string.h>

static_assert(RustLinkManager::MAX_LINKS <= RustIncomingDelivery::NoLink,
              "Receipt link-slot sentinel must not overlap a Link owner slot");

namespace {
// Python RNS 1.2.5 Link.py constants.
constexpr unsigned long KEEPALIVE_MS = 360000;             // KEEPALIVE_MAX
constexpr unsigned long KEEPALIVE_MIN_MS = 5000;           // KEEPALIVE_MIN
constexpr unsigned long ESTABLISHMENT_TIMEOUT_PER_HOP_MS = 6000;  // DEFAULT_PER_HOP_TIMEOUT
constexpr unsigned long STALE_GRACE_MS = 5000;             // STALE_GRACE
constexpr double KEEPALIVE_PER_RTT = 360.0 / 1.75;         // KEEPALIVE_MAX / KEEPALIVE_MAX_RTT
constexpr double KEEPALIVE_TIMEOUT_FACTOR = 4.0;
constexpr uint32_t KEEPALIVE_QUEUE_WAIT_MS = 120000;

void secureZero(uint8_t* p, size_t n) {
    volatile uint8_t* v = p;
    while (n--) *v++ = 0;
}

// msgpack float64: 0xCB || 8-byte big-endian IEEE-754. RTT is a double in SECONDS.
size_t packF64(double value, uint8_t* out) {
    out[0] = 0xCB;
    uint64_t bits;
    memcpy(&bits, &value, 8);
    for (int i = 0; i < 8; i++) out[1 + i] = (uint8_t)(bits >> (56 - 8 * i));
    return 9;
}

// msgpack float64 decode; false unless data is the 0xCB form (the only shape umsgpack
// emits for a Python float RTT).
bool unpackF64(const uint8_t* data, size_t len, double* out) {
    if (len != 9 || data[0] != 0xCB) return false;
    uint64_t bits = 0;
    for (int i = 0; i < 8; i++) bits = (bits << 8) | data[1 + i];
    memcpy(out, &bits, 8);
    return true;
}
}  // namespace

void RustLinkManager::begin(const Deps& deps) { _d = deps; }

RustLinkManager::Link* RustLinkManager::findByDest(const uint8_t dest[16]) {
    for (auto& l : _links) {
        if (l.state != State::Free && l.state != State::Closed &&
            memcmp(l.peerDest, dest, 16) == 0)
            return &l;
    }
    return nullptr;
}

RustLinkManager::Link* RustLinkManager::findByLinkId(const uint8_t linkId[16]) {
    for (auto& l : _links) {
        if (l.state != State::Free && l.state != State::Closed &&
            memcmp(l.linkId, linkId, 16) == 0)
            return &l;
    }
    return nullptr;
}

RustLinkManager::Link* RustLinkManager::allocLink() {
    for (auto& l : _links) {
        auto& generation = _generations[&l - _links];
        if ((l.state == State::Free || l.state == State::Closed) && generation != UINT32_MAX) {
            ++generation; return &l;
        }
    }
    // Pool full: reclaim the oldest responder half-open. Every initiator retry
    // arrives under a fresh link_id, so lost handshakes would otherwise pin all
    // slots for the full RespPending timeout (~366s) and deny link-path LXMF.
    // Active/InitRequested/Stale slots carry real sessions — never evicted.
    Link* oldest = nullptr;
    for (auto& l : _links) {
        if (l.state != State::RespPending || _generations[&l - _links] == UINT32_MAX) continue;
        if (!oldest || (long)(oldest->requestMs - l.requestMs) > 0) oldest = &l;
    }
    if (oldest) {
        Serial.println("[RUST-LINK] slot pressure: evicting oldest half-open responder");
        closeLink(*oldest);
        ++_generations[oldest - _links];
        return oldest;
    }
    return nullptr;
}

void RustLinkManager::closeLink(Link& l) {
    if (_d.lxmf) _d.lxmf->incoming().dropLink(uint8_t(&l - _links), _generations[&l - _links]);
    if (l.state != State::Free && l.state != State::Closed) {
        rs_handheld_rns_link_unregister(_d.ctx, l.linkId);
    }
    secureZero(l.sessionKey, sizeof(l.sessionKey));
    secureZero(l.ephPriv, sizeof(l.ephPriv));
    l.haveKey = false;
    l.keepalivePending = false;
    l.keepaliveInterfaceGeneration = 0;
    l.state = State::Closed;
    if (_request.sink && _request.slot == size_t(&l - _links)) failRequest(RequestError::LinkClosed);
}

void RustLinkManager::failSetup(Link& l) {
    // Full Rust expires failed endpoint Link routes and queues LXMF recovery.
    // Report the failed route to that same outgoing owner instead of starting
    // another handshake against a route already known to have failed.
    rs_handheld_route_t failedRoute = {};
    failedRoute.kind = RS_HANDHELD_ROUTE_DIRECT;
    failedRoute.interface_id = l.iface;
    failedRoute.header_type = l.hasNextHop ? 1 : 0;
    failedRoute.hops = l.hops;
    if (l.hasNextHop) memcpy(failedRoute.next_hop, l.nextHop, 16);
    closeLink(l);
    if (_d.lxmf) _d.lxmf->onLinkSetupFailure(l.peerDest, failedRoute);
}

void RustLinkManager::endAll() {
    // Runtime teardown retires consumer ownership without a reentrant restart.
    _request = {};
    for (auto& l : _links) closeLink(l);
}

bool RustLinkManager::receiptBinding(uint8_t iface, const uint8_t linkId[16], uint8_t& slot,
                                    uint32_t& generation) const {
    for (uint8_t index = 0; index < MAX_LINKS; ++index) {
        const auto& link = _links[index];
        if (!receiptLive(index, _generations[index], iface) || memcmp(link.linkId, linkId, 16)) continue;
        slot = index; generation = _generations[index]; return true;
    }
    return false;
}

bool RustLinkManager::receiptLive(uint8_t slot, uint32_t generation, uint8_t iface) const {
    if (slot >= MAX_LINKS || !generation || _generations[slot] != generation) return false;
    const auto& link = _links[slot];
    return link.haveKey && link.iface == iface && link.state != State::Free && link.state != State::Closed;
}

const uint8_t* RustLinkManager::receiptPeer(uint8_t slot, uint32_t generation) const {
    if (slot >= MAX_LINKS || !receiptLive(slot, generation, _links[slot].iface) || !_links[slot].initiator) return nullptr;
    return _links[slot].peerDest;
}

bool RustLinkManager::frameToDest(uint8_t ifaceId, const uint8_t dest[16], uint8_t headerType,
                                  const uint8_t nextHop[16], uint8_t packetType, uint8_t destType,
                                  uint8_t context, const uint8_t* payload, size_t len,
                                  uint8_t* outHash) {
    uint8_t raw[640];
    size_t rawLen = 0;
    const uint8_t* transportId = headerType == 1 ? nextHop : nullptr;
    if (rs_handheld_rns_packet_build(headerType, packetType, destType, context, transportId, dest,
                                     payload, len, raw, sizeof(raw), &rawLen) != RS_HANDHELD_OK)
        return false;
    if (rawLen && outHash) rs_handheld_rns_packet_hash(raw, rawLen, headerType, outHash);
    return rawLen && _d.pump && _d.pump->sendTo(ifaceId, raw, rawLen);
}

bool RustLinkManager::sendLinkFrame(Link& l, uint8_t context, const uint8_t* payload, size_t len,
                                    uint8_t* outHash) {
    // Link frames address the link_id (LINK dest): HEADER_1 broadcast in BOTH directions —
    // post-establishment traffic rides hub link_tables unmodified (Transport.py:1632-1667).
    // TX deliberately does NOT touch the watchdog baseline (Python last_outbound is not
    // part of the ACTIVE/STALE rule — refreshing it here would mask peer death).
    return frameToDest(l.iface, l.linkId, 0, nullptr, RustWire::PT_DATA, RustWire::DT_LINK, context,
                payload, len, outHash);
}

bool RustLinkManager::linkEstablishing(const uint8_t dest[16]) const {
    for (const auto& link : _links)
        if (link.initiator && link.state == State::InitRequested && !memcmp(link.peerDest, dest, 16)) return true;
    return false;
}

bool RustLinkManager::ensureLink(const uint8_t dest[16], const uint8_t pubkey[64],
                                 const rs_handheld_route_t& route) {
    if (route.kind != RS_HANDHELD_ROUTE_DIRECT) return false;
    Link* l = findByDest(dest);
    if (l && l->state == State::Active) return true;
    if (l && l->state == State::InitRequested) {
        if (millis() - l->requestMs < l->establishmentTimeoutMs)
            return false;  // still establishing
        failSetup(*l);
        return false; // Let its owner recover the route before another handshake.
    }
    if (l && l->state == State::Stale) {
        closeLink(*l);  // peer went quiet past 2*keepalive; tear down and re-establish
        l = nullptr;
    }
    if (!l) l = allocLink();
    if (!l) return false;  // no free slot

    uint8_t x25519[32], ed25519[32];
    RustEntropy::fill(x25519, sizeof(x25519));
    RustEntropy::fill(ed25519, sizeof(ed25519));
    uint8_t req[RS_HANDHELD_LINK_REQUEST_LEN];
    size_t reqLen = 0;
    if (rs_handheld_rns_link_request_build(x25519, ed25519, 1, 500, req, sizeof(req), &reqLen) !=
        RS_HANDHELD_OK) {
        return false;
    }
    uint8_t linkId[16];
    if (rs_handheld_rns_link_id(dest, req, reqLen, linkId) != RS_HANDHELD_OK) return false;

    *l = Link{};
    l->state = State::InitRequested;
    l->initiator = true;
    memcpy(l->peerDest, dest, 16);
    memcpy(l->linkId, linkId, 16);
    memcpy(l->pubkey, pubkey, 64);
    memcpy(l->ephPriv, x25519, 32);
    l->iface = route.interface_id;
    l->hops = route.hops;
    l->hasNextHop = route.header_type == 1;
    if (l->hasNextHop) memcpy(l->nextHop, route.next_hop, 16);
    const uint32_t firstHopBitrate = _d.pump ? _d.pump->interfaceBitrate(l->iface) : 0;
    l->establishmentTimeoutMs =
        RustLinkTiming::establishmentTimeoutMs(l->hops, firstHopBitrate) +
        (_d.pump ? _d.pump->interfaceTxWaitMs(l->iface, 2) : 0);
    l->requestMs = millis();
    l->lastInboundMs = l->requestMs;
    if (rs_handheld_rns_link_register(_d.ctx, linkId) != RS_HANDHELD_OK) {
        secureZero(x25519, sizeof(x25519));
        secureZero(ed25519, sizeof(ed25519));
        closeLink(*l);
        return false;
    }

    // LINKREQUEST -> the destination (SINGLE), packet_type LINKREQUEST, context None.
    const bool accepted = frameToDest(l->iface, dest, route.header_type, route.next_hop, RustWire::PT_LINKREQUEST,
                RustWire::DT_SINGLE, RustWire::CTX_NONE, req, reqLen);
    secureZero(x25519, sizeof(x25519));
    secureZero(ed25519, sizeof(ed25519));
    if (!accepted) { closeLink(*l); return false; }
    Serial.printf("[RUST-LINK] initiator LINKREQUEST sent (bitrate=%lu timeout=%lums)\n",
                  (unsigned long)firstHopBitrate,
                  (unsigned long)l->establishmentTimeoutMs);
    return false;
}

void RustLinkManager::onLrProof(Link& l, const rs_handheld_local_frame_t& f) {
    uint8_t responderPub[32];
    int32_t valid = 0;
    if (rs_handheld_rns_link_proof_validate(l.pubkey, l.linkId, f.payload, f.payload_len,
                                            responderPub, &valid) != RS_HANDHELD_OK ||
        !valid) {
        Serial.println("[RUST-LINK] LRPROOF invalid");
        return;
    }
    if (rs_handheld_rns_link_derive(l.ephPriv, responderPub, l.linkId, l.sessionKey) !=
        RS_HANDHELD_OK) {
        return;
    }
    l.haveKey = true;
    secureZero(l.ephPriv, sizeof(l.ephPriv));
    // LRRTT activation (handshake msg 3): encrypted msgpack-f64 RTT in seconds.
    l.rttSecs = (millis() - l.requestMs) / 1000.0;
    uint8_t rttMp[9];
    packF64(l.rttSecs, rttMp);
    uint8_t iv[16], enc[64];
    RustEntropy::fill(iv, sizeof(iv));
    size_t encLen = 0;
    if (rs_handheld_rns_link_encrypt(l.sessionKey, rttMp, sizeof(rttMp), iv, enc, sizeof(enc),
                                     &encLen) != RS_HANDHELD_OK) {
        return;
    }
    if (!sendLinkFrame(l, RustWire::CTX_LRRTT, enc, encLen)) { closeLink(l); return; }
    l.state = State::Active;
    l.lastInboundMs = millis();  // activation + last_proof baseline (Link.py:434-438)
    updateKeepalive(l, l.rttSecs);
    Serial.println("[RUST-LINK] initiator link ACTIVE (LRRTT sent)");
}

void RustLinkManager::updateKeepalive(Link& l, double rttSecs) {
    // Link.py:844-846: keepalive = clamp(rtt * (360/1.75), 5, 360)s; stale = 2*keepalive.
    double ka = rttSecs * KEEPALIVE_PER_RTT * 1000.0;
    if (ka < (double)KEEPALIVE_MIN_MS) ka = (double)KEEPALIVE_MIN_MS;
    if (ka > (double)KEEPALIVE_MS) ka = (double)KEEPALIVE_MS;
    l.keepaliveMs = (uint32_t)ka;
    l.staleTimeMs = l.keepaliveMs * 2;
}

void RustLinkManager::sendTeardown(Link& l) {
    // Python Link.__teardown_packet: LINKCLOSE context, payload = link.encrypt(link_id).
    if (!l.haveKey) return;
    uint8_t iv[16], enc[80];
    RustEntropy::fill(iv, sizeof(iv));
    size_t encLen = 0;
    if (rs_handheld_rns_link_encrypt(l.sessionKey, l.linkId, 16, iv, enc, sizeof(enc), &encLen) ==
        RS_HANDHELD_OK) {
        sendLinkFrame(l, RustWire::CTX_LINKCLOSE, enc, encLen);
    }
}

void RustLinkManager::queueKeepalive(Link& l) {
    if (l.keepalivePending) return; // duplicate requests never renew a held echo
    l.keepalivePending = true;
    l.keepaliveAttempted = true;
    l.keepaliveBornMs = _d.clock ? _d.clock->nowMs() : uint64_t(millis());
    l.keepaliveInterfaceGeneration = _d.pump ? _d.pump->interfaceGeneration(l.iface) : 0;
}

void RustLinkManager::retryKeepalive(Link& l) {
    if (!l.keepalivePending || !l.haveKey) return;
    const uint64_t now = _d.clock ? _d.clock->nowMs() : uint64_t(millis());
    if (now < l.keepaliveBornMs || now - l.keepaliveBornMs >= KEEPALIVE_QUEUE_WAIT_MS) {
        l.keepalivePending = false;
        Serial.println("[RUST-LINK] pending keepalive expired before admission");
        return;
    }
    if (!_d.pump) return;
    const uint32_t generation = _d.pump->interfaceGeneration(l.iface);
    if (l.keepaliveInterfaceGeneration && generation != l.keepaliveInterfaceGeneration) {
        l.keepalivePending = false;
        Serial.println("[RUST-LINK] pending keepalive lost its interface");
        return;
    }
    if (!generation) return;
    l.keepaliveInterfaceGeneration = generation;
    const uint8_t payload = l.initiator ? 0xFF : 0xFE;
    uint8_t raw[20]; size_t length = 0;
    if (rs_handheld_rns_packet_build(0, RustWire::PT_DATA, RustWire::DT_LINK,
        RustWire::CTX_KEEPALIVE, nullptr, l.linkId, &payload, 1, raw, sizeof(raw),
        &length) != RS_HANDHELD_OK || !length) return;
    handheld::TxLease lease;
    if (!_d.pump->captureLeaseAt(l.iface, raw, length, l.keepaliveBornMs,
                                KEEPALIVE_QUEUE_WAIT_MS, lease)) return;
    const uint32_t ownerGeneration = _generations[&l - _links];
    const uint64_t born = l.keepaliveBornMs;
    const bool admitted = _d.pump->sendLeased(raw, length, lease);
    if (_generations[&l - _links] != ownerGeneration || !l.keepalivePending ||
        l.keepaliveBornMs != born || l.state == State::Free || l.state == State::Closed ||
        !admitted) return;
    l.keepalivePending = false;
    if (l.initiator) l.lastKeepaliveSentMs = millis();
}

void RustLinkManager::onLinkRequest(const rs_handheld_local_frame_t& f, uint8_t ifaceId) {
    if (!_d.ourDestHash) return;
    // Responder: a peer opened a link to our delivery dest.
    uint8_t initX[32];
    if (rs_handheld_rns_link_request_parse(f.payload, f.payload_len, initX, nullptr, nullptr,
                                           nullptr) != RS_HANDHELD_OK) {
        return;
    }
    uint8_t linkId[16];
    if (rs_handheld_rns_link_id(_d.ourDestHash, f.payload, f.payload_len, linkId) != RS_HANDHELD_OK) {
        return;
    }
    Link* l = allocLink();
    if (!l) return;
    *l = Link{};
    uint8_t respX[32];
    RustEntropy::fill(respX, sizeof(respX));
    uint8_t proof[RS_HANDHELD_LINK_PROOF_LEN];
    size_t proofLen = 0;
    if (rs_handheld_rns_link_proof_build(_d.ctx, respX, linkId, 1, 500, proof, sizeof(proof),
                                         &proofLen) != RS_HANDHELD_OK) {
        secureZero(respX, sizeof(respX));
        return;
    }
    if (rs_handheld_rns_link_derive(respX, initX, linkId, l->sessionKey) != RS_HANDHELD_OK) {
        secureZero(respX, sizeof(respX));
        secureZero(l->sessionKey, sizeof(l->sessionKey));  // drop partial derive output
        return;
    }
    secureZero(respX, sizeof(respX));
    l->state = State::RespPending;
    l->initiator = false;
    l->iface = ifaceId;
    l->haveKey = true;
    memcpy(l->linkId, linkId, 16);
    l->hops = f.hops ? f.hops : 1;  // responder establishment timeout scales per Link.py:207
    l->requestMs = millis();
    l->lastInboundMs = l->requestMs;
    memcpy(l->peerDest, _d.ourDestHash, 16);  // responder routing placeholder, never an authenticated source
    if (rs_handheld_rns_link_register(_d.ctx, linkId) != RS_HANDHELD_OK) {
        closeLink(*l);
        return;
    }
    // LRPROOF is a PROOF packet (context Lrproof) addressed to the link_id.
    if (!frameToDest(l->iface, linkId, 0, nullptr, RustWire::PT_PROOF, RustWire::DT_LINK,
                     RustWire::CTX_LRPROOF, proof, proofLen)) { closeLink(*l); return; }
    Serial.println("[RUST-LINK] responder LRPROOF sent; awaiting LRRTT");
}

void RustLinkManager::onLinkData(Link& l, const rs_handheld_local_frame_t& f) {
    if (!l.haveKey) return;
    // Link.py:974: the INITIATOR ignores an inbound 0xFF keepalive entirely (no liveness
    // credit, no echo) — only the peer's 0xFE responses count.
    if (f.context == RustWire::CTX_KEEPALIVE && l.initiator && f.payload_len == 1 &&
        f.payload[0] == 0xFF) {
        return;
    }
    l.lastInboundMs = millis();
    if (l.initiator) l.keepalivePending = false; // peer activity makes an unsent request unnecessary
    if (l.state == State::Stale) l.state = State::Active;  // any inbound revives (Link.py:983-984)

    if (f.context == RustWire::CTX_LRRTT) {
        if (l.initiator) return;  // Link.py:1057: only the responder consumes LRRTT
        // Responder activation: decrypt the initiator's RTT and take
        // max(measured, received) for the keepalive interval (Link.py:534-541).
        uint8_t pt[32];
        size_t ptLen = 0;
        if (rs_handheld_rns_link_decrypt(l.sessionKey, f.payload, f.payload_len, pt, sizeof(pt),
                                         &ptLen) == RS_HANDHELD_OK) {
            double received = 0;
            double measured = (millis() - l.requestMs) / 1000.0;
            if (unpackF64(pt, ptLen, &received) && received > measured) measured = received;
            l.rttSecs = measured;
            l.state = State::Active;
            updateKeepalive(l, l.rttSecs);
            Serial.println("[RUST-LINK] responder link ACTIVE (LRRTT received)");
        }
        return;
    }
    if (f.context == RustWire::CTX_KEEPALIVE) {
        // Responder echoes 0xFE to a 0xFF request, plaintext (Link.py:1149-1153,
        // Packet.py:205-208); an inbound 0xFE is liveness credit only.
        if (!l.initiator && f.payload_len == 1 && f.payload[0] == 0xFF) {
            queueKeepalive(l);
            retryKeepalive(l);
        }
        return;
    }
    if (f.context == RustWire::CTX_LINKCLOSE) {
        // Peer teardown (Python Link.teardown: encrypted link_id). Close only on an
        // authenticated close that names this link.
        uint8_t pt[32];
        size_t ptLen = 0;
        if (rs_handheld_rns_link_decrypt(l.sessionKey, f.payload, f.payload_len, pt, sizeof(pt),
                                         &ptLen) == RS_HANDHELD_OK &&
            ptLen == 16 && memcmp(pt, l.linkId, 16) == 0) {
            Serial.println("[RUST-LINK] peer closed link");
            closeLink(l);
        }
        return;
    }
    if (f.context == RustWire::CTX_RESOURCE_ADV || f.context == RustWire::CTX_RESOURCE_REQ ||
        f.context == RustWire::CTX_RESOURCE_ICL || f.context == RustWire::CTX_RESOURCE_RCL ||
        f.context == RustWire::CTX_RESOURCE_PRF || f.context == RustWire::CTX_RESOURCE) {
        if (_d.resources) _d.resources->onLinkFrame(l.peerDest, l.linkId, l.sessionKey, l.iface, f);
        return;
    }
    if (f.context == RustWire::CTX_RESPONSE && l.initiator && l.state == State::Active) {
        uint8_t pt[RS_HANDHELD_LINK_MDU]; size_t length = 0;
        if (rs_handheld_rns_link_decrypt(l.sessionKey, f.payload, f.payload_len,
                pt, sizeof pt, &length) == RS_HANDHELD_OK)
            completeResponse(l.iface, l.linkId, pt, length);
        return;
    }
    if (f.context == RustWire::CTX_NONE) {
        // Link-delivered LXMF (DIRECT): decrypt -> full packed message -> LXMF engine.
        uint8_t pt[RS_HANDHELD_LINK_MDU + 32];
        size_t ptLen = 0;
        if (rs_handheld_rns_link_decrypt(l.sessionKey, f.payload, f.payload_len, pt, sizeof(pt),
                                         &ptLen) == RS_HANDHELD_OK &&
            _d.lxmf) {
            RustIncomingDelivery::ReceiptSeed seed;
            uint8_t raw[128]; size_t rawLen = 0;
            if (!l.initiator) {
                uint8_t proof[RS_HANDHELD_PROOF_MAX]; size_t proofLen = 0;
                if (rs_handheld_rns_proof_build(_d.ctx, f.packet_hash, 0, proof,
                    sizeof(proof), &proofLen) != RS_HANDHELD_OK ||
                    rs_handheld_rns_packet_build(0, RustWire::PT_PROOF, RustWire::DT_LINK,
                    RustWire::CTX_NONE, nullptr, l.linkId, proof, proofLen, raw, sizeof(raw),
                    &rawLen) != RS_HANDHELD_OK || !rawLen) return;
            }
            // Initiator-role packet proofs require a discarded ephemeral signing
            // key; preserve the existing no-proof policy while still storing safely.
            if (!_d.lxmf->incoming().captureSeed(seed,
                l.initiator ? RustIncomingDelivery::Kind::NoProof : RustIncomingDelivery::Kind::LinkPacket,
                l.iface, raw, rawLen, l.linkId)) return;
            _d.lxmf->onDirectPayload(pt, ptLen, seed);
        }
    }
}

void RustLinkManager::onLocalFrame(const rs_handheld_local_frame_t& f, uint8_t ifaceId) {
    // LINKREQUEST to our dest -> responder handshake.
    if (f.packet_type == RustWire::PT_LINKREQUEST) {
        onLinkRequest(f, ifaceId);
        return;
    }
    // LRPROOF (PROOF + Lrproof) matches an initiator link by link_id (the frame dest).
    if (f.packet_type == RustWire::PT_PROOF && f.context == RustWire::CTX_LRPROOF) {
        Link* l = findByLinkId(f.destination_hash);
        if (l && l->iface == ifaceId && l->initiator && l->state == State::InitRequested)
            onLrProof(*l, f);
        return;
    }
    // Resource delivery proof rides a PROOF packet (Python Packet.py:196) addressed to the link.
    if (f.packet_type == RustWire::PT_PROOF && f.context == RustWire::CTX_RESOURCE_PRF) {
        Link* l = findByLinkId(f.destination_hash);
        if (l && l->iface == ifaceId) onLinkData(*l, f);
        return;
    }
    // Link DATA (LRRTT / keepalive / resource / link-LXMF) routed by link_id.
    if (f.packet_type == RustWire::PT_DATA) {
        Link* l = findByLinkId(f.destination_hash);
        if (l && l->iface == ifaceId) onLinkData(*l, f);
    }
}

bool RustLinkManager::sendLinkData(const uint8_t dest[16], const uint8_t* plaintext, size_t len,
                                   uint8_t* outHash) {
    uint8_t raw[500]; size_t rawLength = 0;
    if (!buildLinkDataPacket(dest, plaintext, len, raw, sizeof(raw), rawLength)) return false;
    if (outHash) rs_handheld_rns_packet_hash(raw, rawLength, 0, outHash);
    return _d.pump && _d.pump->sendTo(activeLinkIface(dest), raw, rawLength);
}

bool RustLinkManager::buildLinkDataPacket(const uint8_t dest[16], const uint8_t* plaintext, size_t len,
    uint8_t* raw, size_t capacity, size_t& rawLength) {
    return buildLinkPacket(dest, RustWire::CTX_NONE, plaintext, len, raw, capacity, rawLength);
}

bool RustLinkManager::buildLinkPacket(const uint8_t dest[16], uint8_t context,
    const uint8_t* plaintext, size_t len, uint8_t* raw, size_t capacity, size_t& rawLength) {
    rawLength = 0;
    Link* l = findByDest(dest);
    if (!l || l->state != State::Active || !l->haveKey) return false;
    uint8_t iv[16];
    RustEntropy::fill(iv, sizeof(iv));
    uint8_t enc[RS_HANDHELD_LINK_MDU + 64];
    size_t encLen = 0;
    if (rs_handheld_rns_link_encrypt(l->sessionKey, plaintext, len, iv, enc, sizeof(enc), &encLen) !=
        RS_HANDHELD_OK) {
        return false;
    }
    return rs_handheld_rns_packet_build(0, RustWire::PT_DATA, RustWire::DT_LINK, context,
        nullptr, l->linkId, enc, encLen, raw, capacity, &rawLength) == RS_HANDHELD_OK && rawLength;
}

bool RustLinkManager::identify(Link& link) {
    if (link.state != State::Active || !link.initiator || !_d.pump) return false;
    if (link.identified) return true;
    uint8_t plaintext[128], raw[500]; size_t length = 0;
    if (rs_handheld_rns_link_identify(_d.ctx, link.linkId, plaintext) != RS_HANDHELD_OK ||
        !buildLinkPacket(link.peerDest, RustWire::CTX_LINKIDENTIFY, plaintext, sizeof plaintext,
                         raw, sizeof raw, length)) return false;
    const auto generation = _generations[&link - _links];
    if (!_d.pump->sendTo(link.iface, raw, length) || generation != _generations[&link - _links] ||
        link.state != State::Active) return false;
    link.identified = true;
    return true;
}

bool RustLinkManager::startGet(const uint8_t dest[16], uint8_t operation, const uint8_t* transientId,
                              uint16_t limitBytes, RequestSink& sink, uint32_t timeoutMs) {
    if (_request.sink || _requestSequence == UINT32_MAX || !timeoutMs || !_d.pump) return false;
    Link* link = findByDest(dest);
    if (!link || !identify(*link)) return false;
    uint8_t plaintext[96], raw[500], hash[32]; size_t length = 0, rawLength = 0;
    const uint64_t now = _d.clock ? _d.clock->nowMs() : uint64_t(millis());
    const auto epoch = RustClock::epochSecs();
    if (rs_handheld_lxmf_get_request(epoch ? double(epoch) : double(now) / 1000.0, operation,
        transientId, limitBytes, plaintext, sizeof plaintext, &length) != RS_HANDHELD_OK ||
        !buildLinkPacket(dest, RustWire::CTX_REQUEST, plaintext, length, raw, sizeof raw, rawLength) ||
        rs_handheld_rns_packet_hash(raw, rawLength, 0, hash) != RS_HANDHELD_OK) return false;
    handheld::TxLease lease;
    if (!_d.pump->captureLeaseAt(link->iface, raw, rawLength, now, timeoutMs, lease)) return false;
    const auto sequence = ++_requestSequence;
    _request = {};
    _request.sink = &sink; _request.sequence = sequence;
    _request.bornMs = now; _request.waitMs = timeoutMs;
    _request.slot = uint8_t(link - _links); _request.iface = link->iface;
    _request.linkGeneration = _generations[_request.slot];
    _request.interfaceGeneration = _d.pump->interfaceGeneration(link->iface);
    memcpy(_request.id, hash, 16); // packet requests use the transmitted packet hash
    const bool admitted = _d.pump->sendLeased(raw, rawLength, lease);
    if (!admitted && _request.sequence == sequence) _request = {};
    return admitted;
}

bool RustLinkManager::requestLive() const {
    if (!_request.sink || !_d.pump || !receiptLive(_request.slot, _request.linkGeneration, _request.iface) ||
        !_request.interfaceGeneration || _d.pump->interfaceGeneration(_request.iface) != _request.interfaceGeneration)
        return false;
    const auto now = _d.clock ? _d.clock->nowMs() : uint64_t(millis());
    return now >= _request.bornMs && now - _request.bornMs < _request.waitMs;
}

void RustLinkManager::cancelRequest(RequestSink& sink) {
    if (_request.sink == &sink) _request = {};
}

void RustLinkManager::failRequest(RequestError error) {
    auto* sink = _request.sink;
    _request = {};
    if (sink) sink->onLinkRequestFailed(error);
}

bool RustLinkManager::responseId(uint8_t iface, const uint8_t linkId[16], uint8_t out[16]) const {
    if (!requestLive() || _request.iface != iface || memcmp(_links[_request.slot].linkId, linkId, 16)) return false;
    memcpy(out, _request.id, 16);
    return true;
}

bool RustLinkManager::completeResponse(uint8_t iface, const uint8_t linkId[16], const uint8_t* data, size_t length) {
    uint8_t expected[16]; size_t offset = 0, valueLength = 0;
    if (!responseId(iface, linkId, expected) ||
        rs_handheld_lxmf_response_view(data, length, expected, &offset, &valueLength) != RS_HANDHELD_OK) return false;
    auto* sink = _request.sink;
    _request = {};
    sink->onLinkResponse(data + offset, valueLength);
    return true;
}

void RustLinkManager::failResponse(uint8_t iface, const uint8_t linkId[16], const uint8_t requestId[16], RequestError error) {
    uint8_t expected[16];
    if (responseId(iface, linkId, expected) && !memcmp(expected, requestId, 16)) failRequest(error);
}

const uint8_t* RustLinkManager::activeLinkKey(const uint8_t dest[16]) const {
    for (const auto& l : _links) {
        if (l.state == State::Active && l.haveKey && memcmp(l.peerDest, dest, 16) == 0)
            return l.sessionKey;
    }
    return nullptr;
}

const uint8_t* RustLinkManager::activeLinkId(const uint8_t dest[16]) const {
    for (const auto& l : _links) {
        if (l.state == State::Active && memcmp(l.peerDest, dest, 16) == 0) return l.linkId;
    }
    return nullptr;
}

uint8_t RustLinkManager::activeLinkIface(const uint8_t dest[16]) const {
    for (const auto& l : _links) {
        if (l.state == State::Active && memcmp(l.peerDest, dest, 16) == 0) return l.iface;
    }
    return UINT8_MAX;
}

bool RustLinkManager::linkActive(const uint8_t dest[16]) const {
    return activeLinkId(dest) != nullptr;
}

size_t RustLinkManager::activeCount() const {
    size_t n = 0;
    for (const auto& l : _links)
        if (l.state == State::Active) n++;
    return n;
}

void RustLinkManager::loop() {
    if (_request.sink && !requestLive()) failRequest(RequestError::Timeout);
    unsigned long now = millis();
    for (auto& l : _links) {
        if (l.state == State::Active || l.state == State::Stale || l.state == State::RespPending)
            retryKeepalive(l);
        if (l.state == State::Active) {
            // Python watchdog (Link.py:786-810): baseline is last INBOUND/proof/activation.
            if (now - l.lastInboundMs >= l.keepaliveMs) {
                // Only the initiator schedules keepalives, on the two-condition rule
                // (quiet inbound AND quiet since our last 0xFF — Link.py:792-794).
                const uint64_t clockNow = _d.clock ? _d.clock->nowMs() : uint64_t(now);
                if (l.initiator && !l.keepalivePending &&
                    now - l.lastKeepaliveSentMs >= l.keepaliveMs &&
                    (!l.keepaliveAttempted || (clockNow >= l.keepaliveBornMs &&
                     clockNow - l.keepaliveBornMs >= l.keepaliveMs))) {
                    queueKeepalive(l);
                    retryKeepalive(l);
                }
                if (now - l.lastInboundMs >= l.staleTimeMs) {
                    l.state = State::Stale;
                    l.staleSinceMs = now;
                    Serial.println("[RUST-LINK] link stale");
                }
            }
        } else if (l.state == State::Stale) {
            // Grace = rtt*4 + 5s (Link.py:797); any inbound flips back to Active. Then
            // LINKCLOSE teardown (encrypted link_id) + close, reason TIMEOUT. Note:
            // Python's WATCHDOG_MAX_SLEEP(5) caps its effective grace at ~5s; we honor
            // the nominal value — more lenient on high-RTT (LoRa) links, wire-neutral.
            unsigned long grace =
                (unsigned long)(l.rttSecs * KEEPALIVE_TIMEOUT_FACTOR * 1000.0) + STALE_GRACE_MS;
            if (now - l.staleSinceMs >= grace) {
                sendTeardown(l);
                Serial.println("[RUST-LINK] link stale; closing");
                closeLink(l);
            }
        } else if (l.state == State::RespPending) {
            // Responder awaiting LRRTT: Link.py:207 establishment timeout
            // (per-hop x max(1,hops) + KEEPALIVE), not the stale sweep.
            unsigned long timeout =
                ESTABLISHMENT_TIMEOUT_PER_HOP_MS * (l.hops < 1 ? 1 : l.hops) + KEEPALIVE_MS;
            if (now - l.requestMs > timeout) {
                Serial.println("[RUST-LINK] responder link never activated; closing");
                closeLink(l);
            }
        } else if (l.state == State::InitRequested) {
            if (now - l.requestMs > l.establishmentTimeoutMs) {
                Serial.println("[RUST-LINK] link establishment timed out");
                failSetup(l);
            }
        }
    }
}
