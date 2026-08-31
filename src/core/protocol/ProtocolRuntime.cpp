
#include "protocol/ProtocolRuntime.h"
#include "runtime/TaskOwner.h"
#include "protocol/RustEntropy.h"
#include "protocol/RustWire.h"
#include "reticulum/IdentityManager.h"
#include "reticulum/AnnounceManager.h"
#include "storage/FlashStore.h"
#include "storage/SDStore.h"
#include "storage/MessageStore.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>

namespace {

String toHex(const uint8_t* data, size_t len) {
    String out;
    out.reserve(len * 2);
    char b[3];
    for (size_t i = 0; i < len; i++) {
        snprintf(b, sizeof(b), "%02x", data[i]);
        out += b;
    }
    return out;
}

// Micro diag format: first 12 hex chars as xxxx:xxxx:xxxx.
String toColonHash(const String& hex) {
    if (hex.length() < 12) return hex;
    return hex.substring(0, 4) + ":" + hex.substring(4, 8) + ":" + hex.substring(8, 12);
}

bool hexToBytes(const std::string& hex, uint8_t* out, size_t outLen) {
    if (hex.size() != outLen * 2) return false;
    for (size_t i = 0; i < outLen; i++) {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int hi = nib(hex[i * 2]), lo = nib(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

void secureZero(uint8_t* buf, size_t len) {
    volatile uint8_t* p = buf;
    while (len--) *p++ = 0;
}

}  // namespace

ProtocolRuntime::~ProtocolRuntime() {
    handheld::assertDeviceOwner(); end(); }

bool ProtocolRuntime::begin(FlashStore* flash, SDStore* sd, IdentityManager* idMgr, MessageStore* store,
                        AnnounceManager* announceMgr, int32_t profile, uint32_t nodeHeapCaps) {
    handheld::assertDeviceOwner();
    if (_ctx) end();

    rs_handheld_status_t st = rs_handheld_rns_init(&_ctx);
    if (st != RS_HANDHELD_OK || !_ctx) {
        Serial.printf("[RUST] init failed (%d) — backend not ready\n", (int)st);
        _ctx = nullptr;
        return false;
    }
    Serial.printf("[RUST] FFI %s\n", rs_handheld_rns_version());

    if (!loadOrCreateIdentity(idMgr)) {
        Serial.println("[RUST] identity unavailable — backend not ready");
        return false;
    }

    // Dedup boot-seed (G9 contract): replay the boot-cached recent message ids
    // BEFORE the pump starts, so post-reboot retries dedup correctly.
    seedDedup(store);

    _profile = profile;
    if (!openTransport(profile, nodeHeapCaps)) {
        Serial.println("[RUST] transport unavailable — backend not ready");
        return false;
    }

    _pump.begin(_ctx, &_clock);
    _pump.setSink(this);
    if (!startEngines(flash, sd, store, announceMgr)) return false;
    // Boot restore (micro LXMFManager::begin parity): re-queue persisted
    // QUEUED/SENDING outgoing so a reboot doesn't strand them at QUEUED forever.
    if (_enginesUp && store) _lxmf.restorePending(store->startupPendingOutgoing());
    Serial.printf("[RUST] backend up: identity=%s dest=%s node=%u bytes (%s) engines=%s\n",
                  _identityHashStr.c_str(), _destHashStr.c_str(),
                  (unsigned)_nodeBufLen,
                  profile == RS_HANDHELD_PROFILE_MICRO ? "MICRO" : "SMALL",
                  _enginesUp ? "up" : "off");
    return true;
}

bool ProtocolRuntime::startEngines(FlashStore* flash, SDStore* sd, MessageStore* store,
                               AnnounceManager* announceMgr) {
    handheld::assertDeviceOwner();
    _announceMgr = announceMgr;
    RustKeyMap::cleanupLegacyFiles(flash, sd);
    if (!_keymap.begin(flash, _ctx, _profile, _clock.nowMs())) return false;
    // Ratchet state must be restored BEFORE the first announce: the ring blob is
    // identity-signed, so this runs after the identity load, and announcing a ratchet
    // whose private key we no longer hold would strand peer traffic.
    _ratchets.begin(flash, _ctx, _identityHash, RustClock::epochSecs(), _clock.nowMs());
    // Seed the KeyMap with our own identity so self-continuity holds.
    _keymap.learn(_destHash, _publicKey, _clock.nowMs());

    RustLinkManager::Deps ld;
    ld.ctx = _ctx;
    ld.clock = &_clock;
    ld.keymap = &_keymap;
    ld.pump = &_pump;
    ld.lxmf = &_lxmf;
    ld.resources = &_resources;
    ld.ourDestHash = _destHash;
    _links.begin(ld);

    RustResourceEngine::Deps rd;
    rd.ctx = _ctx;
    rd.clock = &_clock;
    rd.pump = &_pump;
    rd.lxmf = &_lxmf;
    _resources.begin(rd);
    // Resource proof = delivery ack: flip the tracked resource-sent message DELIVERED/FAILED.
    _resources.setOutcomeCallback([this](const uint8_t* peerDest, bool delivered) {
        _lxmf.onResourceOutcome(peerDest, delivered);
    });

    RustLxmfEngine::Deps ed;
    ed.ctx = _ctx;
    ed.clock = &_clock;
    ed.keymap = &_keymap;
    ed.store = store;
    ed.pump = &_pump;
    ed.links = &_links;
    ed.resources = &_resources;
    ed.onMessage = &_onMessage;
    ed.statusCb = &_statusCb;
    ed.ourDestHash = _destHash;
    _lxmf.begin(ed);
    _enginesUp = true;
    return true;
}

bool ProtocolRuntime::loadOrCreateIdentity(IdentityManager* idMgr) {
    handheld::assertDeviceOwner();
    if (!idMgr) return false;
    uint8_t key[64];
    bool haveKey = idMgr->readActiveIdentityKey(key);
    if (!haveKey) {
        // Flash slot/path loss falls back to the NVS mirror before any new
        // identity is generated. Rust validates the key before it is adopted.
        uint8_t restoredHash[16];
        if (idMgr->readNvsIdentityKey(key) &&
            rs_handheld_rns_validate_identity(key, restoredHash, nullptr) == RS_HANDHELD_OK) {
            String hashHex = toHex(restoredHash, 16);
            if (idMgr->createIdentityFromRaw(key, hashHex.c_str()) >= 0) {
                haveKey = true;
                Serial.println("[RUST] identity restored from NVS");
            }
        }
    }
    if (!haveKey) {
        if (idMgr->hasIdentityData()) {
            secureZero(key, sizeof(key));
            Serial.println("[RUST] Existing identity data is unreadable; refusing automatic replacement");
            return false;
        }
        // Fresh rust-env boot with no identity anywhere: create one from TRNG
        // entropy and persist it through the shared slot layout (micro-env
        // boots import the same key from PATH_IDENTITY).
        uint8_t entropy[64];
        RustEntropy::fill(entropy, sizeof(entropy));
        uint8_t identityHash[16];
        rs_handheld_status_t st = rs_handheld_rns_create_identity(entropy, key, identityHash);
        secureZero(entropy, sizeof(entropy));
        if (st != RS_HANDHELD_OK) {
            Serial.printf("[RUST] create_identity failed (%d)\n", (int)st);
            return false;
        }
        String hashHex = toHex(identityHash, 16);
        if (idMgr->createIdentityFromRaw(key, hashHex.c_str()) < 0) {
            secureZero(key, sizeof(key));
            return false;
        }
        Serial.printf("[RUST] created identity %s\n", hashHex.c_str());
    }

    if (rs_handheld_rns_validate_identity(key, _identityHash, _publicKey) != RS_HANDHELD_OK ||
        rs_handheld_rns_load_identity(_ctx, key) != RS_HANDHELD_OK) {
        secureZero(key, sizeof(key));
        Serial.println("[RUST] identity load failed");
        return false;
    }
    idMgr->mirrorIdentityToNvs(key);
    secureZero(key, sizeof(key));

    if (rs_handheld_rns_destination_hash(_ctx, _destHash) != RS_HANDHELD_OK) {
        Serial.println("[RUST] destination hash derivation failed");
        return false;
    }

    _identityHashHex = toHex(_identityHash, 16);
    _identityHashStr = toColonHash(_identityHashHex);
    _destHashHex = toHex(_destHash, 16);
    _destHashStr = toColonHash(_destHashHex);
    _publicKeyHex = toHex(_publicKey, 64);
    _identityLoaded = true;
    return true;
}

void ProtocolRuntime::seedDedup(MessageStore* store) {
    handheld::assertDeviceOwner();
    if (!store) return;
    size_t seeded = 0;
    for (const auto& idHex : store->startupRecentMessageIds(100)) {
        uint8_t id[32];
        if (!hexToBytes(idHex, id, sizeof(id))) continue;
        if (rs_handheld_rns_seed_seen_message(_ctx, id) == RS_HANDHELD_OK) seeded++;
    }
    Serial.printf("[RUST] dedup boot-seed: %u ids\n", (unsigned)seeded);
}

bool ProtocolRuntime::openTransport(int32_t profile, uint32_t nodeHeapCaps) {
    handheld::assertDeviceOwner();
    size_t size = rs_handheld_rns_transport_size(profile);
    size_t align = rs_handheld_rns_transport_align(profile);
    if (size == 0 || align == 0) {
        // Wrong-artifact tripwire: this .a was not compiled for `profile`.
        Serial.printf("[RUST] artifact/profile mismatch (profile=%ld)\n", (long)profile);
        return false;
    }

    // LiteNode requires `align`-byte alignment; heap_caps_malloc only guarantees
    // 4 bytes (PSRAM returned a 4-aligned ptr → open_transport rejected it), so
    // request aligned memory explicitly (freed with heap_caps_free per IDF).
    _nodeBuf = (uint8_t*)heap_caps_aligned_alloc(align, size, nodeHeapCaps);
    if (!_nodeBuf) {
        Serial.printf("[RUST] node alloc failed (%u bytes, align=%u, caps=0x%08lx)\n",
                      (unsigned)size, (unsigned)align, (unsigned long)nodeHeapCaps);
        return false;
    }
    if (((uintptr_t)_nodeBuf % align) != 0) {
        Serial.printf("[RUST] node buffer misaligned (%p %% %u)\n", _nodeBuf, (unsigned)align);
        heap_caps_free(_nodeBuf);
        _nodeBuf = nullptr;
        return false;
    }
    _nodeBufLen = size;

    // Endpoint posture: transport_enabled=0 (no relay). transport_id = our
    // identity hash — stable, unique per device.
    rs_handheld_status_t st = rs_handheld_rns_open_transport(
        _ctx, profile, _identityHash, 0, _nodeBuf, _nodeBufLen);
    if (st != RS_HANDHELD_OK) {
        Serial.printf("[RUST] open_transport failed (%d)\n", (int)st);
        heap_caps_free(_nodeBuf);
        _nodeBuf = nullptr;
        _nodeBufLen = 0;
        return false;
    }
    _nodeOpen = true;
    return true;
}

void ProtocolRuntime::end() {
    handheld::assertDeviceOwner();
    // Teardown: the ctx is still live and the pump is stopped, so nothing can dirty the table
    // between here and shutdown. Without this, up to PEER_SAVE_INTERVAL_MS of learning is lost.
    if (_ctx) {
        _keymap.flush(_clock.nowMs(), true);
        _ratchets.flushPeers(_ctx, millis(), true);
    }
    // Teardown order (header contract): pump quiesce (no late sink RX/TX into a
    // freed node) -> resource closes + link zeroize -> shutdown(ctx) -> free(buf).
    _enginesUp = false;
    _pump.stop();
    _resources.endAll();
    _links.endAll();
    if (_ctx) {
        rs_handheld_rns_shutdown(_ctx);
        _ctx = nullptr;
    }
    if (_nodeBuf) {
        secureZero(_nodeBuf, _nodeBufLen);
        heap_caps_free(_nodeBuf);
        _nodeBuf = nullptr;
        _nodeBufLen = 0;
    }
    _nodeOpen = false;
    _identityLoaded = false;
    _pathRespPendingUntil = 0;
    _normalAnnouncePendingUntil = 0;
    _normalAnnouncePendingLen = 0;
    _pendingPathResponseTagLen = 0;
    _pathResponseCache.clear();
}

void ProtocolRuntime::loop() {
    handheld::assertDeviceOwner();
    if (!_ctx || !_nodeOpen) return;
    _pump.loop();
    // Fire a scheduled path-response re-announce off the ingest callstack once the grace window
    // elapses (fix map §4) — a burst of requests inside the window collapses into this one answer.
    if (_pathRespPendingUntil != 0 && millis() >= _pathRespPendingUntil) {
        _pathRespPendingUntil = 0;
        sendPathResponseAnnounce();
    }
    if (_normalAnnouncePendingUntil != 0 && millis() >= _normalAnnouncePendingUntil) {
        _normalAnnouncePendingUntil = 0;
        const uint8_t* app = _normalAnnouncePendingLen ? _lastAppData : nullptr;
        if (emitAnnounce(app, _normalAnnouncePendingLen, RustWire::CTX_NONE, false) ==
            AnnounceResult::Deferred) {
            _normalAnnouncePendingUntil = millis() + 1000;
        }
    }
    if (_enginesUp) {
        _lxmf.loop();
        _links.loop();
        _resources.loop();
    }
    _ratchets.flushPeers(_ctx, millis(), false);
    _keymap.loop(_clock.nowMs());
}

bool ProtocolRuntime::persistData() {
    handheld::assertDeviceOwner();
    if (!_ctx) return true;
    const bool keys = _keymap.flush(_clock.nowMs(), true);
    const bool peers = _ratchets.flushPeers(_ctx, millis(), true);
    return keys && peers;
}

size_t ProtocolRuntime::pathCount() const {
    handheld::assertDeviceOwner();
    if (!_ctx || !_nodeOpen) return 0;
    uint32_t count = 0;
    if (rs_handheld_rns_path_count(_ctx, const_cast<RustClock&>(_clock).nowMs(), &count) ==
        RS_HANDHELD_OK) {
        return (size_t)count;  // live (unexpired) paths
    }
    return 0;
}

size_t ProtocolRuntime::linkCount() const {
    handheld::assertDeviceOwner(); return _enginesUp ? _links.activeCount() : 0; }

uint32_t ProtocolRuntime::announceFilterCount() const {
    handheld::assertDeviceOwner();
    rs_handheld_transport_stats_t stats = {};
    if (!transportStats(stats)) return 0;
    return stats.announces_rate_dropped > UINT32_MAX
        ? UINT32_MAX
        : (uint32_t)stats.announces_rate_dropped;
}

void ProtocolRuntime::onAnnounceEvent(const rs_handheld_announce_event_t& ev, uint8_t ifaceId) {
    handheld::assertDeviceOwner();
    // Transport freshness was accepted before this event. KeyMap continuity must then accept
    // before the peer-ratchet table is allowed to change.
    if (!RustAnnouncePolicy::accept(_ctx, _keymap, _ratchets, ev, RustClock::epochSecs(),
                                    _clock.nowMs())) {
        Serial.println("[RUST] announce key-change rejected (continuity defense)");
        return;
    }
    if (_announceMgr) {
        // Hop count from the freshly-learned path; RSSI/SNR from the LoRa driver when the
        // announce arrived over LoRa (micro received_announce parity).
        uint8_t hops = 1;
        int32_t hp = 0, hn = 0;
        uint8_t nh[16];
        if (rs_handheld_rns_path_info(_ctx, ev.destination_hash, _clock.nowMs(), &hp, &hops, nh,
                                      &hn, nullptr) != RS_HANDHELD_OK ||
            !hp) {
            hops = 1;
        }
        int rssi = 0;
        float snr = 0;
        if (ifaceId == RustInterfacePump::LORA_IFACE_ID) {
            rssi = _pump.lastLoraRssi();
            snr = _pump.lastLoraSnr();
        }
        _announceMgr->receivedAnnounceEvent(ev.destination_hash, ev.identity_hash, ev.app_data,
                                            ev.app_data_len, rssi, snr, hops);
    }
}

void ProtocolRuntime::onLocalFrame(const rs_handheld_local_frame_t& f, uint8_t ifaceId) {
    handheld::assertDeviceOwner();
    (void)ifaceId;
    if (!_enginesUp) return;
    switch (f.packet_type) {
        case RustWire::PT_LINKREQUEST:
            _links.onLocalFrame(f, ifaceId);
            break;
        case RustWire::PT_PROOF:
            // LRPROOF (handshake) and RESOURCE_PRF (resource delivery proof, Packet.py:196)
            // ride PROOF packets addressed to the link; bare proofs are LXMF delivery receipts.
            if (f.context == RustWire::CTX_LRPROOF || f.context == RustWire::CTX_RESOURCE_PRF)
                _links.onLocalFrame(f, ifaceId);
            else
                _lxmf.onProofFrame(f);
            break;
        case RustWire::PT_DATA:
            // Opportunistic LXMF is addressed to OUR delivery dest with context None; any other
            // local DATA frame targets a registered link_id (link data / LRRTT / keepalive / resource).
            if (f.context == RustWire::CTX_NONE && memcmp(f.destination_hash, _destHash, 16) == 0) {
                _lxmf.onDataFrame(f, ifaceId);
            } else {
                _links.onLocalFrame(f, ifaceId);
            }
            break;
        default:
            break;
    }
}

bool ProtocolRuntime::transportStats(rs_handheld_transport_stats_t& out) const {
    handheld::assertDeviceOwner();
    if (!_ctx || !_nodeOpen) return false;
    return rs_handheld_rns_transport_stats(_ctx, &out) == RS_HANDHELD_OK;
}

void ProtocolRuntime::seedAnnounceAppData(const uint8_t* appData, size_t len) {
    handheld::assertDeviceOwner();
    if (!appData || len == 0 || len > APP_DATA_MAX) return;
    memcpy(_lastAppData, appData, len);
    _lastAppDataLen = len;
}

ProtocolBackend::AnnounceResult ProtocolRuntime::announce(const uint8_t* appData, size_t len) {
    handheld::assertDeviceOwner();
    // A newer caller request supersedes any same-second retry retained for the previous call.
    _normalAnnouncePendingUntil = 0;
    _normalAnnouncePendingLen = 0;
    // Cache the display-name app_data so a later path response can reuse it (fix map §4).
    if (appData && len > 0 && len <= APP_DATA_MAX) {
        memcpy(_lastAppData, appData, len);
        _lastAppDataLen = len;
    }
    const AnnounceResult result = emitAnnounce(appData, len, RustWire::CTX_NONE, false);
    if (result == AnnounceResult::Deferred) {
        if (len <= APP_DATA_MAX && (len == 0 || appData)) {
            _normalAnnouncePendingLen = len;
            _normalAnnouncePendingUntil = millis() + 1000;
        } else {
            Serial.println("[RUST] announce deferred but app_data is too large to retain");
            return AnnounceResult::Failed;
        }
    }
    return result;
}

ProtocolRuntime::AnnounceResult ProtocolRuntime::emitAnnounce(const uint8_t* appData, size_t len,
                                                      uint8_t context, bool pathResponse) {
    handheld::assertDeviceOwner();
    if (!_ctx || !_identityLoaded) {
        Serial.println("[RUST] announce: backend not ready");
        return AnnounceResult::Failed;
    }
    // Persist the independent 40-bit wire ordering value and any key rotation before building
    // the signed announce. Wall time/uptime only govern rotation; they are never conflated with
    // the on-wire logical counter. Same/backward wall time is coalesced and retried next second.
    uint64_t wireValue = 0;
    uint8_t ratchet[32] = {};
    const RustRatchetStore::AnnounceMode mode = _ratchets.prepareAnnounce(
        _ctx, RustClock::epochSecs(), _clock.nowMs(), wireValue, ratchet);
    if (mode == RustRatchetStore::AnnounceMode::Deferred) {
        Serial.println("[RUST] announce coalesced until wire time advances");
        return AnnounceResult::Deferred;
    }
    const bool haveRatchet = mode == RustRatchetStore::AnnounceMode::Ratcheted;
    uint8_t rngSeed[5];
    RustEntropy::fill(rngSeed, sizeof(rngSeed));
    uint8_t out[600];
    size_t outLen = 0;
    uint8_t annDest[16];
    rs_handheld_status_t st = rs_handheld_rns_announce(_ctx, rngSeed, wireValue,
                                                       haveRatchet ? ratchet : nullptr,
                                                       appData, len,
                                                       out, sizeof(out), &outLen, annDest);
    secureZero(rngSeed, sizeof(rngSeed));
    secureZero(ratchet, sizeof(ratchet));
    if (st != RS_HANDHELD_OK) {
        Serial.printf("[RUST] announce build failed (%d)\n", (int)st);
        return AnnounceResult::Failed;
    }
    // Frame as a HEADER_1 broadcast ANNOUNCE (SINGLE dest) and TX on all interfaces.
    uint8_t raw[640];
    size_t rawLen = 0;
    st = rs_handheld_rns_packet_build_flagged(
        0, RustWire::PT_ANNOUNCE, RustWire::DT_SINGLE, context, haveRatchet ? 1 : 0, nullptr,
        annDest, out, outLen, raw, sizeof(raw), &rawLen);
    if (st != RS_HANDHELD_OK || rawLen == 0) {
        Serial.printf("[RUST] announce frame build failed (%d)\n", (int)st);
        return AnnounceResult::Failed;
    }
    const bool accepted = pathResponse
                              ? _pump.sendTo(_pendingPathResponseIface, raw, rawLen)
                              : _pump.sendAll(raw, rawLen);
    if (!accepted) {
        Serial.println("[RUST] announce not accepted by any eligible interface");
        return AnnounceResult::Failed;
    }
    if (pathResponse && _pendingPathResponseTagLen > 0) {
        _pathResponseCache.store(_pendingPathResponseTag, _pendingPathResponseTagLen, raw, rawLen,
                                 _clock.nowMs());
    }
    _lastAnnounceMs = millis();
    Serial.printf("[RUST] announce TX %u bytes app=%u%s%s\n", (unsigned)rawLen, (unsigned)len,
                  pathResponse ? " (path-response)" : "",
                  haveRatchet ? " ratcheted" : " base-key fallback");
#ifdef PROTOCOL_PACKET_TRACE
        // Optional serial diagnostic (-DPROTOCOL_PACKET_TRACE): emit the raw
        // on-wire announce as hex so an offline Python RNS validator can byte-check
        // what the hardware produced. Not built in mainline/release.
        Serial.print("[ANN-WIRE] ");
        for (size_t i = 0; i < rawLen; i++) Serial.printf("%02x", raw[i]);
        Serial.println();
#endif
    return AnnounceResult::Sent;
}

void ProtocolRuntime::onOwnPathRequest(uint8_t ifaceId, const uint8_t tag[16], size_t tagLen) {
    handheld::assertDeviceOwner();
    if (!tag || tagLen == 0 || tagLen > sizeof(_pendingPathResponseTag)) return;
    memset(_pendingPathResponseTag, 0, sizeof(_pendingPathResponseTag));
    memcpy(_pendingPathResponseTag, tag, tagLen);
    _pendingPathResponseTagLen = tagLen;
    _pendingPathResponseIface = ifaceId;
    // A peer's cached path to us expired and it requested ours (endpoint parity with Python
    // Transport.path_request local-dest branch). Schedule a throttled PATH_RESPONSE re-announce.
    RustWire::schedulePathResponse(millis(), PATH_REQUEST_GRACE_MS, PATH_RESP_DEDUP_MS,
                                   _pathRespPendingUntil, _lastPathRespMs);
}

void ProtocolRuntime::sendPathResponseAnnounce() {
    handheld::assertDeviceOwner();
    const uint8_t* cachedRaw = nullptr;
    size_t cachedRawLen = 0;
    if (_pathResponseCache.recall(_pendingPathResponseTag, _pendingPathResponseTagLen,
                                  _clock.nowMs(), cachedRaw, cachedRawLen)) {
        if (_pump.sendTo(_pendingPathResponseIface, cachedRaw, cachedRawLen)) {
            _lastAnnounceMs = millis();
            _lastPathRespMs = millis();
            Serial.printf("[RUST] path-response replay TX %u exact cached bytes\n",
                          (unsigned)cachedRawLen);
        } else {
            Serial.println("[RUST] cached path-response not accepted by request interface");
        }
        return;
    }
    // Reuse the cached display-name app_data; empty is a valid announce if we've never announced.
    const uint8_t* app = _lastAppDataLen ? _lastAppData : nullptr;
    const AnnounceResult result =
        emitAnnounce(app, _lastAppDataLen, RustWire::CTX_PATH_RESPONSE, true);
    if (result == AnnounceResult::Sent) {
        _lastPathRespMs = millis();
    } else if (result == AnnounceResult::Deferred) {
        // Never mark a deferred response as transmitted. Retry once the 1-second wall-order
        // granularity can advance; this avoids fabricating a timestamp a few seconds in future.
        _pathRespPendingUntil = millis() + 1000;
    }
}

bool ProtocolRuntime::lxmfSendMessage(const uint8_t dest[16], const char* content, const char* title,
                                  bool preferLink) {
    handheld::assertDeviceOwner();
    if (!_enginesUp) {
        Serial.println("[RUST] LXMF send: backend not ready");
        return false;
    }
    return _lxmf.send(dest, content, title, preferLink);
}

void ProtocolRuntime::lxmfDropPeer(const std::string& peerHex) {
    handheld::assertDeviceOwner();
    if (!_enginesUp) return;
    uint8_t dest[16];
    if (!hexToBytes(peerHex, dest, sizeof(dest))) return;
    _lxmf.dropPeer(dest);
}
