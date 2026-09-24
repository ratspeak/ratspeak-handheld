
#include "protocol/RustResourceEngine.h"
#include "protocol/RustClock.h"
#include "protocol/RustEntropy.h"
#include "protocol/RustInterfacePump.h"
#include "protocol/RustLxmfEngine.h"
#include "protocol/RustWire.h"
#include <Arduino.h>
#include <string.h>

namespace {
// Network-only timing floors. Radio allowances also cover the bounded TX queue and pacing.
constexpr unsigned long SENDER_IDLE_TIMEOUT_MS = 40000;   // ~Resource.py sender grace x4
constexpr unsigned long RECEIVER_NO_PROGRESS_MS = 8000;   // shrink + re-request threshold
constexpr unsigned long RECEIVER_BASE_TIMEOUT_MS = 30000; // + 15s per advertised part
constexpr unsigned long RECEIVER_PER_PART_TIMEOUT_MS = 15000;
// ARQ budgets (Resource.py:130-131): sender re-advertises up to MAX_ADV_RETRIES if no REQ;
// receiver re-requests up to MAX_PART_RETRIES then cancels. Re-sends are identical frames
// (wire-neutral: a same-resource re-ADV preserves progress; a re-REQ re-serves parts).
constexpr uint8_t MAX_ADV_RETRIES = 4;
constexpr uint8_t MAX_PART_RETRIES = 16;
constexpr unsigned long ADV_RETRY_MS = 8000;  // re-advertise cadence (4x fits the 40s idle cap)
}  // namespace

bool RustResourceEngine::frameLinkEncrypted(uint8_t ifaceId, const uint8_t linkId[16],
                                            const uint8_t key[64], uint8_t packetType, uint8_t context,
                                            const uint8_t* plaintext, size_t len, bool retain, handheld::TxReceipt reservation) {
    uint8_t iv[16];
    RustEntropy::fill(iv, sizeof(iv));
    uint8_t enc[RS_HANDHELD_LINK_MDU + 64];
    size_t encLen = 0;
    if (rs_handheld_rns_link_encrypt(key, plaintext, len, iv, enc, sizeof(enc), &encLen) !=
        RS_HANDHELD_OK) {
        return false;
    }
    uint8_t raw[640];
    size_t rawLen = 0;
    rs_handheld_rns_packet_build(0, packetType, RustWire::DT_LINK, context, nullptr, linkId, enc,
                                 encLen, raw, sizeof(raw), &rawLen);
    if (!rawLen || !_d.pump) return false;
    if (retain) return _d.lxmf && _d.lxmf->incoming().retainControl(ifaceId, linkId, raw, rawLen, reservation);
    if (context == RustWire::CTX_RESOURCE_ADV && _out.active && _d.lxmf) {
        if (_d.lxmf->resourceSendBinding(_out.ticket, ifaceId, linkId) != _out.linkBinding) return false;
        const auto result = _d.lxmf->offerResource(_out.ticket, ifaceId, raw, rawLen, _d.clock->nowMs());
        return result == handheld::TxOffer::Queued || result == handheld::TxOffer::Started;
    }
    return _d.pump->sendTo(ifaceId, raw, rawLen);
}

bool RustResourceEngine::frameRawPart(uint8_t ifaceId, const uint8_t linkId[16],
                                      const uint8_t* part, size_t len) {
    // Parts ride RESOURCE-context packets UNENCRYPTED at the packet layer (the blob is already
    // one Token ciphertext).
    uint8_t raw[640];
    size_t rawLen = 0;
    rs_handheld_rns_packet_build(0, RustWire::PT_DATA, RustWire::DT_LINK, RustWire::CTX_RESOURCE,
                                 nullptr, linkId, part, len, raw, sizeof(raw), &rawLen);
    if (!rawLen || !_d.lxmf || !_out.active ||
        _d.lxmf->resourceSendBinding(_out.ticket, ifaceId, linkId) != _out.linkBinding) return false;
    const auto result = _d.lxmf->offerResource(_out.ticket, ifaceId, raw, rawLen, _d.clock->nowMs());
    return result == handheld::TxOffer::Queued || result == handheld::TxOffer::Started;
}

bool RustResourceEngine::buildRawProof(const uint8_t linkId[16], const uint8_t* proof, size_t len,
                                       uint8_t raw[128], size_t& rawLen) {
    return rs_handheld_rns_packet_build(0, RustWire::PT_PROOF, RustWire::DT_LINK,
        RustWire::CTX_RESOURCE_PRF, nullptr, linkId, proof, len, raw, 128, &rawLen) == RS_HANDHELD_OK && rawLen;
}

uint32_t RustResourceEngine::waitMs(uint8_t iface, uint32_t packets, uint32_t minimum) const {
    const uint32_t radio = _d.pump ? _d.pump->interfaceTxWaitMs(iface, packets) : 0;
    return radio + minimum;
}


bool RustResourceEngine::startSend(handheld::outgoing::Ticket ticket, const uint8_t peerDest[16], const uint8_t linkId[16],
                                   const uint8_t key[64], uint8_t ifaceId,
                                   const uint8_t* data, size_t len) {
    if (_out.active || !ticket.valid() || !_d.lxmf) return false;
    const auto binding = _d.lxmf->resourceSendBinding(ticket, ifaceId, linkId);
    if (!binding) return false;
    uint8_t adv[RS_HANDHELD_RESOURCE_ADV_MAX];
    size_t advLen = 0;
    uint32_t numParts = 0;
    uint8_t resHash[32];
    rs_handheld_status_t st = RS_HANDHELD_ERR_RETRY;
    for (int tries = 0; tries < 4 && st == RS_HANDHELD_ERR_RETRY; tries++) {
        uint8_t rh[4], iv[16];
        RustEntropy::fill(rh, sizeof(rh));  // fresh map-hash entropy each retry
        RustEntropy::fill(iv, sizeof(iv));
        st = rs_handheld_rns_resource_advertise_build(_d.ctx, key, data, len, rh, iv, adv,
                                                      sizeof(adv), &advLen, &numParts, resHash);
    }
    if (st != RS_HANDHELD_OK) {
        Serial.printf("[RUST-RES] advertise_build failed (%d)\n", (int)st);
        return false;
    }
    _out.active = true;
    _out.linkBinding = binding;
    _out.ticket = ticket;
    memcpy(_out.peerDest, peerDest, 16);
    memcpy(_out.linkId, linkId, 16);
    memcpy(_out.key, key, 64);
    memcpy(_out.resourceHash, resHash, 32);
    _out.iface = ifaceId;
    _out.numParts = numParts;
    _out.startMs = millis();
    _out.lastActivityMs = _out.startMs;
    // Retain the ADV plaintext so the watchdog can re-advertise if no REQ arrives.
    memcpy(_out.adv, adv, advLen);
    _out.advLen = advLen;
    _out.lastAdvMs = _out.startMs;
    _out.advRetriesLeft = MAX_ADV_RETRIES;
    _out.reqReceived = false;
    if (!frameLinkEncrypted(ifaceId, linkId, key, RustWire::PT_DATA, RustWire::CTX_RESOURCE_ADV,
                            adv, advLen)) {
        closeOutbound(false, false);
        return false;
    }
    Serial.printf("[RUST-RES] sender ADV sent (%u parts)\n", (unsigned)numParts);
    return true;
}

void RustResourceEngine::closeOutbound(bool delivered, bool notify) {
    const auto ticket = _out.ticket;
    rs_handheld_rns_resource_outbound_close(_d.ctx);
    _out = Out{};
    if (notify && _outcome) _outcome(ticket, delivered);
}

void RustResourceEngine::cancelSend(handheld::outgoing::Ticket ticket) {
    if (!_out.active || _out.ticket != ticket) return;
    // Closing local application ownership never depends on a remote response.
    // The independent terminal-control pool owns any admitted cancellation.
    frameLinkEncrypted(_out.iface, _out.linkId, _out.key, RustWire::PT_DATA,
        RustWire::CTX_RESOURCE_ICL, _out.resourceHash, 32, true);
    closeOutbound(false);
}

void RustResourceEngine::cancelOutbound() {
    if (!frameLinkEncrypted(_out.iface, _out.linkId, _out.key, RustWire::PT_DATA,
                            RustWire::CTX_RESOURCE_ICL, _out.resourceHash, 32, true)) return;
    closeOutbound(false);
}

void RustResourceEngine::closeInbound(bool cancel, bool keepReceipt) {
    if (cancel && !frameLinkEncrypted(_in.iface, _in.linkId, _in.key, RustWire::PT_DATA,
            RustWire::CTX_RESOURCE_RCL, _in.resourceHash, 32, true, _in.receipt)) {
        _in.cancelPending = true; return;
    }
    if (!cancel && !keepReceipt && _d.lxmf) _d.lxmf->incoming().releaseReservation(_in.receipt);
    rs_handheld_rns_resource_inbound_close(_d.ctx);
    _in = In{};
}

void RustResourceEngine::sendRequest() {
    uint8_t req[RS_HANDHELD_RESOURCE_REQUEST_MAX];
    size_t reqLen = 0;
    if (rs_handheld_rns_resource_request_build(_d.ctx, req, sizeof(req), &reqLen) != RS_HANDHELD_OK) {
        return;
    }
    _in.requestPending = !frameLinkEncrypted(_in.iface, _in.linkId, _in.key, RustWire::PT_DATA,
                                            RustWire::CTX_RESOURCE_REQ, req, reqLen);
    if (_in.requestPending) return;
    _in.lastReqMs = millis();
    _in.partsRemaining = reqLen > 33 ? (reqLen - 33) / 4 : 0;
}

void RustResourceEngine::servePendingParts() {
    for (uint32_t i = 0; i < _out.numParts; ++i) {
        if (!(_out.pendingParts & (1u << i))) continue;
        uint8_t part[RS_HANDHELD_RESOURCE_SDU];
        size_t len = 0;
        if (rs_handheld_rns_resource_part_emit(_d.ctx, i, part, sizeof(part), &len) != RS_HANDHELD_OK)
            return;
        if (!frameRawPart(_out.iface, _out.linkId, part, len)) return;
        _out.pendingParts &= ~(1u << i);
        _out.lastActivityMs = millis();
    }
}

void RustResourceEngine::onLinkFrame(const uint8_t peerDest[16], const uint8_t linkId[16],
                                     const uint8_t key[64], uint8_t ifaceId,
                                     const rs_handheld_local_frame_t& f) {
    if (f.context == RustWire::CTX_RESOURCE_ADV) {
        // RECEIVER: accept the advertised transfer (fail-closed subset).
        // A full peer's ADV may have a much larger hashmap than our outbound ADV.
        uint8_t adv[RS_HANDHELD_LINK_MDU + 16];
        size_t advLen = 0;
        if (rs_handheld_rns_link_decrypt(key, f.payload, f.payload_len, adv, sizeof(adv), &advLen) !=
            RS_HANDHELD_OK) {
            return;
        }
        uint32_t numParts = 0, transferSize = 0, dataSize = 0;
        uint8_t resHash[32];
        if (rs_handheld_rns_resource_advertisement_hash(adv, advLen, resHash) != RS_HANDHELD_OK)
            return;
        if (_in.active) {
            if (_in.iface == ifaceId && memcmp(_in.linkId, linkId, 16) == 0 &&
                memcmp(_in.resourceHash, resHash, 32) == 0) {
                // A lost request caused re-advertisement: preserve received parts and deadlines.
                if (!_in.cancelPending && !_in.awaitingSource) sendRequest();
            } else {
                if (!frameLinkEncrypted(ifaceId, linkId, key, RustWire::PT_DATA,
                                   RustWire::CTX_RESOURCE_RCL, resHash, 32, true))
                    Serial.println("[RUST-RES] busy rejection backpressured");
            }
            return;
        }
        if (!_d.lxmf || _d.lxmf->incoming().resourcePending(ifaceId, linkId, resHash)) return;
        const auto receipt = _d.lxmf->incoming().reserveResource(ifaceId, linkId, resHash);
        if (!receipt.valid()) { Serial.println("[RUST-RES] no receipt credit; ADV deferred"); return; }
        rs_handheld_status_t st = rs_handheld_rns_resource_advertise_accept(
            _d.ctx, adv, advLen, &numParts, &transferSize, &dataSize, resHash);
        if (st != RS_HANDHELD_OK) {
            if (!frameLinkEncrypted(ifaceId, linkId, key, RustWire::PT_DATA,
                                    RustWire::CTX_RESOURCE_RCL, resHash, 32, true, receipt)) {
                _d.lxmf->incoming().releaseReservation(receipt);
                Serial.println("[RUST-RES] rejection backpressured");
            }
            Serial.printf("[RUST-RES] receiver declined ADV (%d)\n", (int)st);
            return;
        }
        _in.active = true; _in.receipt = receipt;
        _d.lxmf->incoming().setResourceDeadline(receipt, waitMs(ifaceId, 2 * numParts + 8,
            RECEIVER_BASE_TIMEOUT_MS + RECEIVER_PER_PART_TIMEOUT_MS * numParts));
        memcpy(_in.peerDest, peerDest, 16);
        memcpy(_in.linkId, linkId, 16);
        memcpy(_in.key, key, 64);
        memcpy(_in.resourceHash, resHash, 32);
        _in.iface = ifaceId;
        _in.numParts = numParts;
        _in.startMs = millis();
        _in.lastPartMs = _in.startMs;
        _in.reqRetriesLeft = MAX_PART_RETRIES;
        sendRequest();
        Serial.printf("[RUST-RES] receiver ACCEPT (%u parts, %u bytes)\n", (unsigned)numParts,
                      (unsigned)dataSize);
        return;
    }

    const bool outbound = _out.active && _out.iface == ifaceId &&
                          memcmp(_out.linkId, linkId, 16) == 0;
    const bool inbound = _in.active && _in.iface == ifaceId &&
                         memcmp(_in.linkId, linkId, 16) == 0;
    if (f.context == RustWire::CTX_RESOURCE_ICL) {
        uint8_t hash[48]; size_t len = 0;
        if (rs_handheld_rns_link_decrypt(key, f.payload, f.payload_len, hash, sizeof(hash), &len) !=
            RS_HANDHELD_OK || len != 32) return;
        if (inbound && !memcmp(hash, _in.resourceHash, 32)) closeInbound(false);
        if (_d.lxmf) _d.lxmf->incoming().cancelResource(ifaceId, linkId, hash);
        return;
    }
    if (f.context == RustWire::CTX_RESOURCE_RCL && outbound) {
        uint8_t hash[48]; size_t len = 0;
        if (rs_handheld_rns_link_decrypt(key, f.payload, f.payload_len, hash, sizeof(hash), &len) ==
            RS_HANDHELD_OK && len == 32 && !memcmp(hash, _out.resourceHash, 32)) closeOutbound(false);
        return;
    }

    if (f.context == RustWire::CTX_RESOURCE_REQ && outbound) {
        // SENDER: serve the requested parts.
        uint8_t req[RS_HANDHELD_RESOURCE_REQUEST_MAX + 32];
        size_t reqLen = 0;
        if (rs_handheld_rns_link_decrypt(_out.key, f.payload, f.payload_len, req, sizeof(req),
                                         &reqLen) != RS_HANDHELD_OK) {
            return;
        }
        uint32_t indices[RS_HANDHELD_RESOURCE_MAX_PARTS];
        uint32_t count = 0;
        if (rs_handheld_rns_resource_request_serve(_d.ctx, req, reqLen, indices, &count) !=
            RS_HANDHELD_OK) {
            return;
        }
        _out.lastActivityMs = millis();  // receiver is alive; reset the idle timeout
        _out.reqReceived = true;         // stop re-advertising once the receiver responds
        for (uint32_t i = 0; i < count; i++) {
            if (indices[i] < _out.numParts) _out.pendingParts |= 1u << indices[i];
        }
        servePendingParts();
        return;
    }

    if (f.context == RustWire::CTX_RESOURCE_PRF && outbound) {
        // SENDER: validate the delivery proof. It rides the link PLAINTEXT (Packet.py:196), so
        // validate the raw payload directly — do NOT link-decrypt (that broke Python interop).
        // Only a VALID proof resolves the transfer; a non-matching one is ignored.
        int32_t valid = 0;
        if (rs_handheld_rns_resource_proof_validate(_d.ctx, f.payload, f.payload_len, &valid) ==
                RS_HANDHELD_OK &&
            valid) {
            Serial.println("[RUST-RES] sender transfer DELIVERED");
            closeOutbound(true);
        }
        return;
    }

    if (f.context == RustWire::CTX_RESOURCE && inbound) {
        // RECEIVER: ingest a raw part.
        int32_t isNew = 0, complete = 0;
        if (rs_handheld_rns_resource_part_ingest(_d.ctx, f.payload, f.payload_len, &isNew,
                                                 &complete) != RS_HANDHELD_OK) {
            return;
        }
        if (complete) {
            if (!_in.awaitingSource) acceptAssembled();
        } else if (isNew) {
            _in.lastPartMs = millis();
            _in.reqRetriesLeft = MAX_PART_RETRIES;
            // A shrunken window or duplicate ADV can request fewer than four parts.
            if (_in.partsRemaining > 0 && --_in.partsRemaining == 0) {
                rs_handheld_rns_resource_window_grow(_d.ctx, 0);
                sendRequest();
            }
        }
    }
}

void RustResourceEngine::acceptAssembled() {
    bool admitted = false;
    size_t outLen = 0;
    // The Rust receiver retains the authenticated assembled bytes. Re-copy into
    // shared scratch only for this synchronous validation attempt; no extra body
    // buffer or borrowed scratch survives a service poll.
    if (_d.lxmf && rs_handheld_rns_resource_assemble(_d.ctx, _in.key, _codec,
            sizeof(_codec), &outLen) == RS_HANDHELD_OK) {
        uint8_t proof[RS_HANDHELD_RESOURCE_PROOF_LEN], raw[128];
        size_t proofLen = 0, rawLen = 0;
        RustIncomingDelivery::ReceiptSeed seed;
        if (rs_handheld_rns_resource_proof_build(_d.ctx, proof, sizeof(proof), &proofLen) == RS_HANDHELD_OK &&
            buildRawProof(_in.linkId, proof, proofLen, raw, rawLen) &&
            _d.lxmf->incoming().resourceSeed(_in.receipt, seed, raw, rawLen)) {
            const auto received = _d.lxmf->onDirectPayload(_codec, outLen, seed);
            if (received.error == RustIncomingDelivery::ReceiveError::SourceUnknown) {
                // Discovery is transient. Keep the original Resource reservation
                // and deadline; duplicate traffic cannot renew either. LXMF owns
                // the bounded/throttled identity request. No proof is sent yet.
                _in.awaitingSource = true;
                _in.sourcePollMs = millis();
                return;
            }
            admitted = received.code == RustIncomingDelivery::ReceiveCode::Pending ||
                received.code == RustIncomingDelivery::ReceiveCode::HandledNonpersistent;
        }
    }
    // Only validated bytes reach storage; its existing commit gate owns proof.
    closeInbound(!admitted, admitted);
}

void RustResourceEngine::loop() {
    unsigned long now = millis();
    // A dead Link cannot carry terminal ICL. Close the exact local transfer
    // instead of retrying an impossible cancellation admission forever.
    if (_out.active && (!_d.lxmf || _d.lxmf->resourceSendBinding(_out.ticket, _out.iface, _out.linkId) != _out.linkBinding))
        closeOutbound(false);
    if (_out.active) {
        servePendingParts();
        const uint32_t idleLimit = _out.reqReceived
            ? waitMs(_out.iface, 8, SENDER_IDLE_TIMEOUT_MS)
            : waitMs(_out.iface, 2, ADV_RETRY_MS) * (MAX_ADV_RETRIES + 1);
        if (now - _out.lastActivityMs > idleLimit) {
            Serial.println("[RUST-RES] sender transfer timed out");
            cancelOutbound();
        } else if (!_out.reqReceived && _out.advRetriesLeft > 0 &&
                   now - _out.lastAdvMs > waitMs(_out.iface, 2, ADV_RETRY_MS)) {
            // No part-request yet: re-advertise (identical ADV plaintext, fresh IV — the
            // receiver treats a same-link re-ADV as a sender retry, Resource.py:580-588).
            if (frameLinkEncrypted(_out.iface, _out.linkId, _out.key, RustWire::PT_DATA,
                                   RustWire::CTX_RESOURCE_ADV, _out.adv, _out.advLen)) {
                _out.advRetriesLeft--;
                _out.lastAdvMs = now;
                Serial.printf("[RUST-RES] sender re-ADV (%u left)\n", (unsigned)_out.advRetriesLeft);
            }
        }
    }
    if (_in.active) {
        if (!_d.lxmf || !_d.lxmf->incoming().resourceLive(_in.receipt)) { closeInbound(false); return; }
        if (_in.cancelPending) { closeInbound(true); return; }
        if (_in.requestPending) sendRequest();
        if (now - _in.startMs >
            waitMs(_in.iface, 2 * _in.numParts + 8,
                   RECEIVER_BASE_TIMEOUT_MS + RECEIVER_PER_PART_TIMEOUT_MS * _in.numParts)) {
            Serial.println("[RUST-RES] receiver transfer timed out");
            closeInbound(true);
        } else if (_in.awaitingSource) {
            if (now - _in.sourcePollMs >= 250) acceptAssembled();
        } else if (!_in.requestPending &&
                   now - _in.lastPartMs > waitMs(_in.iface, 6, RECEIVER_NO_PROGRESS_MS) &&
                   now - _in.lastReqMs > waitMs(_in.iface, 6, RECEIVER_NO_PROGRESS_MS)) {
            // No progress since the last request: shrink + re-request, bounded by the
            // per-part retry budget (Resource.py:613-628 MAX_RETRIES) before cancelling.
            if (_in.reqRetriesLeft == 0) {
                Serial.println("[RUST-RES] receiver out of part-request retries; cancelling");
                closeInbound(true);
            } else {
                _in.reqRetriesLeft--;
                rs_handheld_rns_resource_window_shrink(_d.ctx);
                sendRequest();
            }
        }
    }
}

void RustResourceEngine::endAll() {
    if (_out.active) {
        rs_handheld_rns_resource_outbound_close(_d.ctx);
        _out = Out{};
    }
    if (_in.active) {
        closeInbound(false);
    }
}

void RustResourceEngine::dropPeer(const uint8_t peerDest[16]) {
    if (_out.active && memcmp(_out.peerDest, peerDest, 16) == 0) {
        closeOutbound(false, false);
    }
    if (_in.active && memcmp(_in.peerDest, peerDest, 16) == 0) {
        closeInbound(false);
    }
}
