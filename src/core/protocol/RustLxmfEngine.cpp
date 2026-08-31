
#include "protocol/RustLxmfEngine.h"
#include "protocol/RustClock.h"
#include "protocol/RustEntropy.h"
#include "protocol/RustKeyMap.h"
#include "protocol/RustInterfacePump.h"
#include "protocol/RustWire.h"
#include "protocol/RustLinkManager.h"
#include "protocol/RustResourceEngine.h"
#include "storage/MessageStore.h"
#include <Arduino.h>
#include <algorithm>

namespace {
constexpr unsigned long DISCOVERY_RETRY_MS = 10000;
constexpr int DISCOVERY_MAX_ATTEMPTS = 7;   // immediate + six 10s retries ~= 60s
// Proof-wait before requeue. Shorter than the donor's 60s (closer to upstream LXMF's 10s
// DELIVERY_RETRY_WAIT), sized for the worst-case LoRa link RTT (~5-8s) with margin. Plus a
// per-pending random jitter so messages lost together (a burst) don't retry in lockstep and
// re-collide on the half-duplex link. Both are local timing (wire-neutral).
constexpr unsigned long PROOF_TIMEOUT_MS = 20000;
constexpr unsigned long PROOF_JITTER_MAX_MS = 8000;
constexpr uint8_t PROOF_MAX_ATTEMPTS = 3;
constexpr unsigned long LINK_WAIT_TIMEOUT_MS = 60000;  // six discovery retry intervals

unsigned long proofJitterMs() {
    uint32_t r = 0;
    RustEntropy::fill((uint8_t*)&r, sizeof(r));
    return r % PROOF_JITTER_MAX_MS;
}

std::string hex(const uint8_t* d, size_t n) {
    static const char* H = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) {
        s.push_back(H[d[i] >> 4]);
        s.push_back(H[d[i] & 0xF]);
    }
    return s;
}

void secureZero(uint8_t* data, size_t len) {
    volatile uint8_t* p = data;
    while (len--) *p++ = 0;
}
}  // namespace

void RustLxmfEngine::begin(const Deps& deps) { _d = deps; }

bool RustLxmfEngine::send(const uint8_t dest[16], const char* content, const char* title,
                          bool preferLink) {
    // In-flight proof tracking counts against the same bound: _pending entries hold
    // full message content and would otherwise grow past the queue cap in a burst
    // (heap headroom on the no-PSRAM board).
    if ((int)_outQueue.size() >= RSDECK_MAX_OUTQUEUE ||
        (int)_pending.size() >= RSDECK_MAX_OUTQUEUE) {
        Serial.println("[RUST-LXMF] outqueue full; refusing");
        return false;
    }
    OutMsg m;
    memcpy(m.dest, dest, 16);
    m.content = content ? content : "";
    m.title = title ? title : "";
    m.preferLink = preferLink;
    uint64_t epoch = RustClock::epochSecs();
    m.timestamp = epoch ? (double)epoch : (millis() / 1000.0);

    // Persistence is the queue transaction boundary. Never create an in-memory
    // send that the UI/history cannot represent or recover after reboot.
    if (!_d.store) return false;
    LXMFMessage lm;
    if (_d.ourDestHash) lm.sourceHash = rs::Bytes(_d.ourDestHash, 16);
    lm.destHash = rs::Bytes(dest, 16);
    lm.timestamp = m.timestamp;
    lm.content = m.content;
    lm.title = m.title;
    lm.incoming = false;
    lm.status = LXMFStatus::QUEUED;
    if (!_d.store->saveMessage(lm)) {
        Serial.println("[RUST-LXMF] message store rejected send; refusing");
        return false;
    }
    m.savedCounter = lm.savedCounter;
    _outQueue.push_back(m);
    return true;
}

void RustLxmfEngine::restorePending(const std::vector<LXMFMessage>& pending) {
    size_t restored = 0;
    for (const auto& lm : pending) {
        if ((int)_outQueue.size() >= RSDECK_MAX_OUTQUEUE) break;
        if (lm.destHash.size() != 16) continue;
        OutMsg m;
        memcpy(m.dest, lm.destHash.data(), 16);
        m.content = lm.content;
        m.title = lm.title;
        m.timestamp = lm.timestamp;
        m.savedCounter = lm.savedCounter;
        _outQueue.push_back(m);
        restored++;
    }
    if (restored) {
        Serial.printf("[RUST-LXMF] restored %u pending outgoing\n", (unsigned)restored);
    }
}

void RustLxmfEngine::markStatus(const std::string& peerHex, double ts, uint32_t counter,
                                LXMFStatus st) {
    if (_d.store) {
        if (counter > 0) _d.store->updateMessageStatusByCounter(peerHex, counter, false, st);
        else _d.store->updateMessageStatus(peerHex, ts, false, st);
    }
    if (_d.statusCb && *_d.statusCb) (*_d.statusCb)(peerHex, ts, counter, st);
}

bool RustLxmfEngine::attempt(OutMsg& msg) {
    std::string peerHex = hex(msg.dest, 16);

    rs_handheld_route_t route = {};
    if (rs_handheld_rns_route(_d.ctx, msg.dest, _d.clock->nowMs(), &route) != RS_HANDHELD_OK) {
        return false;
    }

    // A persisted Rust-known identity key remains usable while the live path
    // expires; in that case Rust returns BROADCAST and the packet fans out.
    uint8_t pubkey[64] = {};
    bool havePub = _d.keymap && _d.keymap->recall(msg.dest, pubkey);
    int32_t hasPath = 0, hasNext = 0;
    uint8_t hops = 0, nextHop[16] = {}, pathPubkey[64] = {};
    if (rs_handheld_rns_path_info(_d.ctx, msg.dest, _d.clock->nowMs(), &hasPath, &hops, nextHop,
                                  &hasNext, pathPubkey) != RS_HANDHELD_OK) {
        return false;
    }
    if (!havePub && hasPath) {
        memcpy(pubkey, pathPubkey, sizeof(pubkey));
        havePub = true;
        if (_d.keymap) _d.keymap->learn(msg.dest, pubkey, _d.clock->nowMs());
    }
    if (!havePub) {
        uint8_t tag[16];
        RustEntropy::fill(tag, sizeof(tag));
        rs_handheld_rns_request_path(_d.ctx, msg.dest, tag, 0, _d.clock->nowMs());
        msg.retries++;
        if (msg.retries >= DISCOVERY_MAX_ATTEMPTS) {
            Serial.printf("[RUST-LXMF] no path for %s after ~60s -> FAILED\n", peerHex.substr(0, 8).c_str());
            markStatus(peerHex, msg.timestamp, msg.savedCounter, LXMFStatus::FAILED);
            return true;
        }
        return false;  // keep in queue
    }

    // Build the opportunistic ECIES packet payload (skipped once the message is committed to
    // the link/resource path — no point re-running ECIES every pass while a link establishes).
    if (!msg.viaLink && !msg.preferLink) {
        uint8_t ephemeral[32], iv[16];
        RustEntropy::fill(ephemeral, sizeof(ephemeral));
        RustEntropy::fill(iv, sizeof(iv));
        uint8_t cipher[600];
        size_t cipherLen = 0;
        uint8_t outDest[16], mid[32];
        rs_handheld_status_t bst = rs_handheld_rns_lxmf_build(
            _d.ctx, pubkey, msg.timestamp, RustClock::epochSecs(), _d.clock->nowMs(),
            (const uint8_t*)(msg.title.empty() ? nullptr : msg.title.data()), msg.title.size(),
            (const uint8_t*)(msg.content.empty() ? nullptr : msg.content.data()), msg.content.size(),
            ephemeral, iv, cipher, sizeof(cipher), &cipherLen, outDest, mid);
        secureZero(ephemeral, sizeof(ephemeral));
        secureZero(iv, sizeof(iv));

        if (bst == RS_HANDHELD_OK) {
            memcpy(msg.messageId, mid, 32);
            msg.haveId = true;
            // Rust route() owns header/interface selection. DIRECT uses exactly
            // one learned interface; BROADCAST is reserved for no live path.
            uint8_t raw[640];
            size_t rawLen = 0;
            int32_t headerType = route.header_type;
            const uint8_t* transportId = headerType == 1 ? route.next_hop : nullptr;
            rs_handheld_rns_packet_build(headerType, RustWire::PT_DATA, RustWire::DT_SINGLE,
                                         RustWire::CTX_NONE, transportId, msg.dest, cipher, cipherLen,
                                         raw, sizeof(raw), &rawLen);
            // Only a selected LoRa route has the RNode single-frame gate. A TCP
            // route can carry the full Reticulum MDU even while LoRa is online.
            bool loraGate = route.kind == RS_HANDHELD_ROUTE_DIRECT &&
                route.interface_id == RustInterfacePump::LORA_IFACE_ID &&
                rawLen > RSDECK_RNODE_SINGLE_FRAME_RAW_MAX;
            if (rawLen != 0 && !loraGate) {
                bool transmitted = true;
                if (_d.pump) {
                    if (route.kind == RS_HANDHELD_ROUTE_DIRECT) {
                        transmitted = _d.pump->sendTo(route.interface_id, raw, rawLen);
                    } else {
                        transmitted = _d.pump->sendAll(raw, rawLen);
                    }
                } else {
                    transmitted = false;
                }
                if (!transmitted) return false;
                uint8_t hash[32];
                rs_handheld_rns_packet_hash(raw, rawLen, headerType, hash);
                Pending p;
                memcpy(p.hash, hash, 32);
                memcpy(p.pubkey, pubkey, 64);
                memcpy(p.dest, msg.dest, 16);
                p.content = msg.content;
                p.title = msg.title;
                p.timestamp = msg.timestamp;
                p.savedCounter = msg.savedCounter;
                p.createdMs = millis();
                p.jitterMs = proofJitterMs();
                p.interfaceWaitMs = _d.pump->interfaceTxWaitMs(
                    route.kind == RS_HANDHELD_ROUTE_DIRECT ? route.interface_id
                                                           : RustInterfacePump::LORA_IFACE_ID, 2);
                p.attempts = msg.proofAttempts;  // proof budget is independent of discovery retries
                _pending[hex(hash, 32)] = p;
                markStatus(peerHex, msg.timestamp, msg.savedCounter, LXMFStatus::SENT);
                Serial.printf("[RUST-LXMF] opportunistic SENT %s raw=%u\n",
                              peerHex.substr(0, 8).c_str(), (unsigned)rawLen);
                return true;
            }
        }
        // Oversize / LoRa-gated: commit to the link/resource path for all later passes.
        msg.viaLink = true;
    }

    // Fallback: link + resource delivery for oversize / LoRa-gated / link-preferred messages.
    // Bounded wait (six discovery retry intervals): a link that never
    // establishes must terminate FAILED, not hold a queue slot forever.
    if (_d.links && _d.resources) {
        bool done = false;
        if (deliverViaLinkOrResource(msg, pubkey, &done)) {
            if (done) return true;   // resolved (SENT/queued on the link path)
            unsigned long now = millis();
            if (msg.linkStartMs == 0) msg.linkStartMs = now;
            const uint32_t radioWait = _d.pump ? _d.pump->interfaceTxWaitMs(route.interface_id, 4) : 0;
            if (now - msg.linkStartMs > LINK_WAIT_TIMEOUT_MS + radioWait) {
                Serial.printf("[RUST-LXMF] link to %s never established -> FAILED\n",
                              peerHex.substr(0, 8).c_str());
                markStatus(peerHex, msg.timestamp, msg.savedCounter, LXMFStatus::FAILED);
                return true;
            }
            return false;            // link establishing; retry next pass
        }
    }
    Serial.printf("[RUST-LXMF] %s needs link/resource delivery (unavailable) -> FAILED\n",
                  peerHex.substr(0, 8).c_str());
    markStatus(peerHex, msg.timestamp, msg.savedCounter, LXMFStatus::FAILED);
    return true;
}

bool RustLxmfEngine::deliverViaLinkOrResource(OutMsg& msg, const uint8_t pub[64], bool* done) {
    *done = false;
    std::string peerHex = hex(msg.dest, 16);
    rs_handheld_route_t route = {};
    if (rs_handheld_rns_route(_d.ctx, msg.dest, _d.clock->nowMs(), &route) != RS_HANDHELD_OK ||
        route.kind != RS_HANDHELD_ROUTE_DIRECT) {
        return false;
    }
    if (_d.keymap) _d.keymap->learn(msg.dest, pub, _d.clock->nowMs());

    if (!_d.links->ensureLink(msg.dest, pub, route)) {
        return true;  // link establishing; retry next pass (*done stays false)
    }

    // Link ACTIVE: build the FULL packed (DIRECT) LXMF message.
    static uint8_t packed[RS_HANDHELD_RESOURCE_DATA_MAX];
    size_t packedLen = 0;
    uint8_t outDest[16], mid[32];
    rs_handheld_status_t st = rs_handheld_rns_lxmf_build_link(
        _d.ctx, pub, msg.timestamp,
        (const uint8_t*)(msg.title.empty() ? nullptr : msg.title.data()), msg.title.size(),
        (const uint8_t*)(msg.content.empty() ? nullptr : msg.content.data()), msg.content.size(),
        packed, sizeof(packed), &packedLen, outDest, mid);
    if (st != RS_HANDHELD_OK) {
        Serial.printf("[RUST-LXMF] build_link failed (%d) for %s\n", (int)st, peerHex.substr(0, 8).c_str());
        markStatus(peerHex, msg.timestamp, msg.savedCounter, LXMFStatus::FAILED);
        *done = true;
        return true;
    }

    if (packedLen <= RS_HANDHELD_LINK_MDU) {
        // Single link data packet: track the delivery proof (explicit hash+sig signed by the
        // responder's identity — Link.py:383-390; validated by proof_validate exactly like an
        // opportunistic proof). A lost packet/proof requeues via the link path (matches upstream's
        // coarse re-attempt) instead of sticking at SENT forever.
        uint8_t hash[32];
        if (_d.links->sendLinkData(msg.dest, packed, packedLen, hash)) {
            Pending p;
            memcpy(p.hash, hash, 32);
            memcpy(p.pubkey, pub, 64);
            memcpy(p.dest, msg.dest, 16);
            p.content = msg.content;
            p.title = msg.title;
            p.timestamp = msg.timestamp;
            p.savedCounter = msg.savedCounter;
            p.createdMs = millis();
            p.jitterMs = proofJitterMs();
            p.interfaceWaitMs = _d.pump ?
                _d.pump->interfaceTxWaitMs(_d.links->activeLinkIface(msg.dest), 2) : 0;
            p.attempts = msg.proofAttempts;
            p.preferLink = true;
            _pending[hex(hash, 32)] = p;
            markStatus(peerHex, msg.timestamp, msg.savedCounter, LXMFStatus::SENT);
            Serial.printf("[RUST-LXMF] link-packet SENT %s (%u) awaiting proof\n",
                          peerHex.substr(0, 8).c_str(), (unsigned)packedLen);
            *done = true;
        }
        return true;
    }

    // Resource transfer (chunked). The resource proof is the delivery ack: the outcome
    // callback flips the tracked message DELIVERED (valid proof) or FAILED (timeout).
    const uint8_t* linkId = _d.links->activeLinkId(msg.dest);
    const uint8_t* key = _d.links->activeLinkKey(msg.dest);
    uint8_t iface = _d.links->activeLinkIface(msg.dest);
    if (linkId && key && !_resPending.active &&
        _d.resources->startSend(msg.dest, linkId, key, iface, packed, packedLen)) {
        _resPending.active = true;
        memcpy(_resPending.dest, msg.dest, 16);
        _resPending.timestamp = msg.timestamp;
        _resPending.savedCounter = msg.savedCounter;
        markStatus(peerHex, msg.timestamp, msg.savedCounter, LXMFStatus::SENT);
        Serial.printf("[RUST-LXMF] resource SENT %s (%u)\n", peerHex.substr(0, 8).c_str(),
                      (unsigned)packedLen);
        *done = true;
        return true;
    }
    // Resource busy with another transfer — retry next pass. Not an establishment
    // stall: the in-flight transfer is bounded by the resource engine's own caps,
    // so the LINK_WAIT budget must not FAIL a healthy queued message behind it.
    if (_resPending.active) msg.linkStartMs = 0;
    return true;
}

void RustLxmfEngine::onResourceOutcome(const uint8_t peerDest[16], bool delivered) {
    if (!_resPending.active || memcmp(_resPending.dest, peerDest, 16) != 0) return;
    std::string peerHex = hex(_resPending.dest, 16);
    markStatus(peerHex, _resPending.timestamp, _resPending.savedCounter,
               delivered ? LXMFStatus::DELIVERED : LXMFStatus::FAILED);
    Serial.printf("[RUST-LXMF] resource %s %s\n", delivered ? "DELIVERED" : "FAILED",
                  peerHex.substr(0, 8).c_str());
    _resPending.active = false;
}

void RustLxmfEngine::loop() {
    unsigned long now = millis();
    // Proof timeouts (requeue up to 3x, then FAILED).
    for (auto it = _pending.begin(); it != _pending.end();) {
        if (now - it->second.createdMs >
            PROOF_TIMEOUT_MS + it->second.jitterMs + it->second.interfaceWaitMs) {
            Pending p = it->second;
            it = _pending.erase(it);
            requeueOrFail(p);
        } else {
            ++it;
        }
    }

    if (_outQueue.empty()) return;
    int processed = 0;
    // Index-based: attempt() fires status/store callbacks that may re-enter
    // send() (deque push_back invalidates iterators, not indices).
    for (size_t i = 0; i < _outQueue.size() && processed < 3;) {
        OutMsg& m = _outQueue[i];
        if (m.retries > 0 && (now - m.lastRetryMs) < DISCOVERY_RETRY_MS) {
            ++i;
            continue;
        }
        m.lastRetryMs = now;
        if (attempt(m)) {
            processed++;
            _outQueue.erase(_outQueue.begin() + i);
        } else {
            ++i;
        }
    }
}

void RustLxmfEngine::requeueOrFail(Pending& p) {
    std::string peerHex = hex(p.dest, 16);
    p.attempts++;
    bool retry = p.attempts < PROOF_MAX_ATTEMPTS && (int)_outQueue.size() < RSDECK_MAX_OUTQUEUE;
    if (retry) {
        OutMsg m;
        memcpy(m.dest, p.dest, 16);
        m.timestamp = p.timestamp;
        m.savedCounter = p.savedCounter;
        m.proofAttempts = p.attempts;  // discovery budget resets; proof budget carries (independent budgets)
        m.content = p.content;         // retained in Pending — the store tail may have scrolled
        m.title = p.title;
        m.preferLink = p.preferLink;   // a link-packet requeue re-takes the link path (skip ECIES)
        markStatus(peerHex, p.timestamp, p.savedCounter, LXMFStatus::QUEUED);
        _outQueue.push_back(m);
        Serial.printf("[RUST-LXMF] proof timeout; requeued %s (%u/3)\n", peerHex.substr(0, 8).c_str(),
                      (unsigned)p.attempts + 1);
    } else {
        markStatus(peerHex, p.timestamp, p.savedCounter, LXMFStatus::FAILED);
        Serial.printf("[RUST-LXMF] proof timeout; FAILED %s\n", peerHex.substr(0, 8).c_str());
    }
}

void RustLxmfEngine::emitProof(const uint8_t packetHash[32], uint8_t ifaceId) {
    uint8_t proof[RS_HANDHELD_PROOF_MAX];
    size_t proofLen = 0;
    if (rs_handheld_rns_proof_build(_d.ctx, packetHash, 1, proof, sizeof(proof), &proofLen) !=
        RS_HANDHELD_OK) {
        return;
    }
    // Proof destination = truncated packet hash (Python ProofDestination), SINGLE, PROOF, HEADER_1.
    uint8_t proofDest[16];
    memcpy(proofDest, packetHash, 16);
    uint8_t raw[128];
    size_t rawLen = 0;
    rs_handheld_rns_packet_build(0, RustWire::PT_PROOF, RustWire::DT_SINGLE, RustWire::CTX_NONE,
                                 nullptr, proofDest, proof, proofLen, raw, sizeof(raw), &rawLen);
    if (rawLen && _d.pump) _d.pump->sendTo(ifaceId, raw, rawLen);
}

bool RustLxmfEngine::deliverLocal(const uint8_t src[16], const uint8_t pub[64],
                                  const rs_handheld_lxmf_message_t& m, double localTs) {
    // Message-level dedup (LXMF message id), independent of the transport hashlist.
    int32_t seen = 0;
    if (rs_handheld_rns_has_seen_message(_d.ctx, m.message_id, &seen) != RS_HANDHELD_OK) return false;
    if (seen) return true; // A previously committed duplicate still deserves a proof.
    if (_d.keymap) _d.keymap->learn(src, pub);

    LXMFMessage lm;
    lm.sourceHash = rs::Bytes(src, 16);
    if (_d.ourDestHash) lm.destHash = rs::Bytes(_d.ourDestHash, 16);
    lm.timestamp = localTs;  // local receive time
    lm.title.assign((const char*)m.title, m.title_len);
    lm.content.assign((const char*)m.content, m.content_len);
    lm.messageId = rs::Bytes(m.message_id, 32);
    lm.incoming = true;
    lm.status = LXMFStatus::DELIVERED;
    if (!_d.store || !_d.store->saveMessage(lm)) return false;
    if (rs_handheld_rns_seed_seen_message(_d.ctx, m.message_id) != RS_HANDHELD_OK) return false;
    if (_d.onMessage && *_d.onMessage) (*_d.onMessage)(lm);
    return true;
}

void RustLxmfEngine::onDataFrame(const rs_handheld_local_frame_t& f, uint8_t ifaceId) {
    if (f.context != RustWire::CTX_NONE) return;  // link/resource contexts routed elsewhere
    uint8_t src[16];
    uint8_t keyHint = RS_HANDHELD_LXMF_BASE_KEY_HINT;
    if (rs_handheld_rns_lxmf_peek_source_hint(_d.ctx, f.payload, f.payload_len, src, &keyHint) !=
        RS_HANDHELD_OK) {
        return;
    }
    uint8_t pub[64];
    bool havePub = _d.keymap && _d.keymap->recall(src, pub);
    if (!havePub) {
        // Try the path table's announced key; else request a path and drop (sender retries).
        int32_t hp = 0, hn = 0;
        uint8_t hops = 0, nh[16] = {};
        if (rs_handheld_rns_path_info(_d.ctx, src, _d.clock->nowMs(), &hp, &hops, nh, &hn, pub) ==
                RS_HANDHELD_OK &&
            hp) {
            havePub = true;
        }
    }
    if (!havePub) {
        uint8_t tag[16];
        RustEntropy::fill(tag, sizeof(tag));
        rs_handheld_rns_request_path(_d.ctx, src, tag, 0, _d.clock->nowMs());
        Serial.println("[RUST-LXMF] inbound: source key unknown; requested path, dropping");
        return;
    }
    rs_handheld_lxmf_message_t msg;
    if (rs_handheld_rns_lxmf_parse_hint(_d.ctx, f.payload, f.payload_len, keyHint, pub, &msg) !=
        RS_HANDHELD_OK) {
        Serial.println("[RUST-LXMF] inbound parse/validate failed");
        return;
    }
    // A delivery proof is a claim that a valid message reached this identity, not merely that an
    // arbitrary packet addressed here arrived. Proof only after decrypt, signature, destination,
    // and source binding all validate AND a new message has been saved.
    if (msg.is_reaction) {
        emitProof(f.packet_hash, ifaceId);
        Serial.println("[RUST-LXMF] dropping reaction message");
        return;
    }
    uint64_t epoch = RustClock::epochSecs();
    double localTs = epoch ? (double)epoch : msg.timestamp;
    if (deliverLocal(src, pub, msg, localTs)) emitProof(f.packet_hash, ifaceId);
}

void RustLxmfEngine::onProofFrame(const rs_handheld_local_frame_t& f) {
    for (auto it = _pending.begin(); it != _pending.end(); ++it) {
        int32_t valid = 0;
        if (rs_handheld_rns_proof_validate(it->second.pubkey, it->second.hash, f.payload,
                                           f.payload_len, &valid) == RS_HANDHELD_OK &&
            valid) {
            Pending p = it->second;
            std::string peerHex = hex(p.dest, 16);
            _pending.erase(it);
            markStatus(peerHex, p.timestamp, p.savedCounter, LXMFStatus::DELIVERED);
            Serial.printf("[RUST-LXMF] DELIVERED proof for %s\n", peerHex.substr(0, 8).c_str());
            return;
        }
    }
}

bool RustLxmfEngine::onDirectPayload(const uint8_t* packed, size_t len) {
    if (len < 32) return false;
    uint8_t src[16];
    memcpy(src, packed + 16, 16);
    uint8_t pub[64];
    bool havePub = _d.keymap && _d.keymap->recall(src, pub);
    if (!havePub) {
        int32_t hp = 0, hn = 0;
        uint8_t hops = 0, nh[16] = {};
        if (rs_handheld_rns_path_info(_d.ctx, src, _d.clock->nowMs(), &hp, &hops, nh, &hn, pub) ==
                RS_HANDHELD_OK && hp) {
            havePub = true;
        }
    }
    if (!havePub) {
        requestUnknownSource(src);
        Serial.println("[RUST-LXMF] direct payload: source key unknown; requested path, dropping");
        return false;
    }
    uint8_t mid[32], srcOut[16];
    double ts = 0;
    static uint8_t titleBuf[RS_HANDHELD_RESOURCE_DATA_MAX];
    static uint8_t contentBuf[RS_HANDHELD_RESOURCE_DATA_MAX];
    size_t tlen = 0, clen = 0;
    int32_t isReaction = 0;
    if (rs_handheld_rns_lxmf_parse_link(_d.ctx, packed, len, pub, mid, srcOut, &ts, titleBuf,
                                        sizeof(titleBuf), &tlen, contentBuf, sizeof(contentBuf),
                                        &clen, &isReaction) != RS_HANDHELD_OK) {
        Serial.println("[RUST-LXMF] direct payload parse failed");
        return false;
    }
    if (_d.keymap) _d.keymap->learn(src, pub, _d.clock->nowMs());
    if (isReaction) {  // proofed upstream; reactions are not stored/rendered
        Serial.println("[RUST-LXMF] dropping reaction message");
        return true;
    }
    int32_t seen = 0;
    if (rs_handheld_rns_has_seen_message(_d.ctx, mid, &seen) != RS_HANDHELD_OK) return false;
    if (seen) return true;
    uint64_t epoch = RustClock::epochSecs();
    LXMFMessage lm;
    lm.sourceHash = rs::Bytes(src, 16);
    if (_d.ourDestHash) lm.destHash = rs::Bytes(_d.ourDestHash, 16);
    lm.timestamp = epoch ? (double)epoch : ts;
    lm.title.assign((const char*)titleBuf, tlen);
    lm.content.assign((const char*)contentBuf, clen);
    lm.messageId = rs::Bytes(mid, 32);
    lm.incoming = true;
    lm.status = LXMFStatus::DELIVERED;
    if (!_d.store || !_d.store->saveMessage(lm)) return false;
    if (rs_handheld_rns_seed_seen_message(_d.ctx, mid) != RS_HANDHELD_OK) return false;
    if (_d.onMessage && *_d.onMessage) (*_d.onMessage)(lm);
    return true;
}

void RustLxmfEngine::requestUnknownSource(const uint8_t source[16]) {
    const unsigned long now = _d.clock ? (unsigned long)_d.clock->nowMs() : millis();
    SourceRequest* slot = nullptr;
    for (auto& request : _sourceRequests) {
        if (request.used && memcmp(request.source, source, 16) == 0) {
            if (now - request.sentMs < SOURCE_REQUEST_THROTTLE_MS) return;
            slot = &request;
            break;
        }
        if (!request.used || now - request.sentMs >= SOURCE_REQUEST_THROTTLE_MS) {
            if (!slot) slot = &request;
        }
    }
    // Under a >16-source burst, suppress new requests until an existing throttle
    // slot expires rather than permit an attacker to evict and immediately retry.
    if (!slot) return;
    slot->used = true;
    memcpy(slot->source, source, 16);
    slot->sentMs = now;
    uint8_t tag[16];
    RustEntropy::fill(tag, sizeof(tag));
    rs_handheld_rns_request_path(_d.ctx, source, tag, 0, now);
}

void RustLxmfEngine::dropPeer(const uint8_t peerDest[16]) {
    _outQueue.erase(std::remove_if(_outQueue.begin(), _outQueue.end(),
                                   [peerDest](const OutMsg& msg) {
                                       return memcmp(msg.dest, peerDest, 16) == 0;
                                   }),
                    _outQueue.end());
    for (auto it = _pending.begin(); it != _pending.end();) {
        if (memcmp(it->second.dest, peerDest, 16) == 0) it = _pending.erase(it);
        else ++it;
    }
    if (_resPending.active && memcmp(_resPending.dest, peerDest, 16) == 0) {
        _resPending.active = false;
    }
    if (_d.resources) _d.resources->dropPeer(peerDest);
}
