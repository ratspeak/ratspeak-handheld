
#include "protocol/ProtocolRuntime.h"
#include "runtime/TaskOwner.h"
#include "protocol/RustEntropy.h"
#include "protocol/RustWire.h"
#include "reticulum/IdentityManager.h"
#include "reticulum/AnnounceManager.h"
#include "storage/FlashStore.h"
#include "storage/SDStore.h"
#include "storage/MessageStore.h"
#include "transport/LoRaInterface.h"
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
    if (_maintenanceRadio && !maintenanceDrained()) return false;
    if (_maintenanceRadio && !_ctx) end();
    if (_ctx) {
        stopReceive(); pollReceive();
        if (!receiveDrained()) return false;
        end();
    }

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
    // The outgoing owner recovers persisted records through bounded metadata
    // queries; it never materializes a second startup collection of bodies.
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
    _ratchets.begin(flash, _ctx, _identityHash, RustClock::synchronizedEpochSecs(), _clock.nowMs());
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
    _resources.setOutcomeCallback([this](handheld::outgoing::Ticket ticket, bool delivered) {
        _lxmf.onResourceOutcome(ticket, delivered);
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
    if (!_lxmf.begin(ed)) return false;
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

void ProtocolRuntime::stopReceive() {
    handheld::assertDeviceOwner();
    _lxmf.incoming().stopAdmissions();
    _lxmf.stopAdmissions();
}
void ProtocolRuntime::pollReceive() {
    handheld::assertDeviceOwner();
    _lxmf.loop();
}

void ProtocolRuntime::beginMaintenance(LoRaInterface& radio) {
    handheld::assertDeviceOwner();
    if (_maintenanceRadio) {
        configASSERT(_maintenanceRadio == &radio);
        return;
    }
    _maintenanceRadio = &radio;
    stopReceive();
    radio.beginMaintenance();
    _pump.stop();
    _normalAnnouncePendingUntil = 0;
    for (auto& response : _pathResponses) response = {};
    _announceTiming = 0;
    _normalAnnouncePendingLen = 0;
}

void ProtocolRuntime::pollMaintenance() {
    handheld::assertDeviceOwner();
    if (!_maintenanceRadio) return;
    _maintenanceRadio->pollMaintenance();
    if (!_maintenanceRadio->pollBeforeBlockingWork()) return;
    pollReceive();
}

bool ProtocolRuntime::maintenanceDrained() const {
    handheld::assertDeviceOwner();
    return _maintenanceRadio && _maintenanceRadio->maintenanceDrained() && receiveDrained();
}

bool ProtocolRuntime::maintenanceFailed() const {
    handheld::assertDeviceOwner();
    return _maintenanceRadio && _maintenanceRadio->maintenanceFailed();
}

void ProtocolRuntime::end() {
    handheld::assertDeviceOwner();
    configASSERT(!_maintenanceRadio || maintenanceDrained());
    stopReceive();
    // Teardown: the ctx is still live and the pump is stopped, so nothing can dirty the table
    // between here and shutdown. Without this, up to PEER_SAVE_INTERVAL_MS of learning is lost.
    if (_ctx) {
        _keymap.flush(_clock.nowMs(), true);
        _ratchets.flushPeers(_ctx, millis(), true);
    }
    // Teardown order (header contract): pump quiesce (no late sink RX/TX into a
    // freed node) -> resource closes + link zeroize -> shutdown(ctx) -> free(buf).
    _pump.stop();
    pollReceive();
    configASSERT(receiveDrained());
    _pump.setReceiptHook(nullptr, nullptr);
    _lxmf.incoming().detach();
    _enginesUp = false;
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
    _normalAnnouncePendingUntil = 0;
    _normalAnnouncePendingLen = 0;
    _announceTiming = 0;
    for (auto& response : _pathResponses) response = {};
    _maintenanceRadio = nullptr;
}

bool ProtocolRuntime::pollRadioBeforeBlockingWork() {
    handheld::assertDeviceOwner();
    return _maintenanceRadio ? _maintenanceRadio->pollBeforeBlockingWork()
                             : _pump.pollRadioBeforeBlockingWork();
}

void ProtocolRuntime::loop() {
    handheld::assertDeviceOwner();
    if (_maintenanceRadio) { pollMaintenance(); return; }
    if (!_ctx || !_nodeOpen) return;
    _pump.loop();
    pollPathResponses();
    if ((_announceTiming & NormalPending) && int32_t(uint32_t(millis()) - _normalAnnouncePendingUntil) >= 0 &&
        pollRadioBeforeBlockingWork()) {
        _announceTiming &= ~NormalPending;
        _normalAnnouncePendingUntil = 0;
        const uint8_t* app = _normalAnnouncePendingLen ? _lastAppData : nullptr;
        if (emitAnnounce(app, _normalAnnouncePendingLen) ==
            AnnounceResult::Deferred) {
            _normalAnnouncePendingUntil = millis() + 1000;
            _announceTiming |= NormalPending;
        }
    }
    if (_enginesUp) {
        _lxmf.loop();
        _links.loop();
        _resources.loop();
    }
    // TX is asynchronous. Do not hold the owner in flash while the modem
    // finishes into standby and a remote peer immediately returns a proof.
    if (!pollRadioBeforeBlockingWork()) return;
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
    if (_maintenanceRadio) return;
    // Transport freshness was accepted before this event. KeyMap continuity must then accept
    // before the peer-ratchet table is allowed to change.
    if (!RustAnnouncePolicy::accept(_ctx, _keymap, _ratchets, ev, RustClock::synchronizedEpochSecs(),
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
    if (!_enginesUp || _maintenanceRadio) return;
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
    // Nonempty deferred announces borrow this same buffer. A committed name
    // change replaces both bytes and length; an explicitly empty retry stays empty.
    if ((_announceTiming & NormalPending) && _normalAnnouncePendingLen)
        _normalAnnouncePendingLen = len;
}

ProtocolBackend::AnnounceResult ProtocolRuntime::announce(const uint8_t* appData, size_t len) {
    handheld::assertDeviceOwner();
    if (_maintenanceRadio) return AnnounceResult::Failed;
    // A newer caller request supersedes any same-second retry retained for the previous call.
    _announceTiming &= ~NormalPending;
    _normalAnnouncePendingUntil = 0;
    _normalAnnouncePendingLen = 0;
    // Cache the display-name app_data so a later path response can reuse it (fix map §4).
    if (appData && len > 0 && len <= APP_DATA_MAX) {
        memcpy(_lastAppData, appData, len);
        _lastAppDataLen = len;
    }
    const AnnounceResult result = emitAnnounce(appData, len);
    if (result == AnnounceResult::Deferred) {
        if (len <= APP_DATA_MAX && (len == 0 || appData)) {
            _normalAnnouncePendingLen = len;
            _normalAnnouncePendingUntil = millis() + 1000;
            _announceTiming |= NormalPending;
        } else {
            Serial.println("[RUST] announce deferred but app_data is too large to retain");
            return AnnounceResult::Failed;
        }
    }
    return result;
}

ProtocolRuntime::AnnounceResult ProtocolRuntime::buildAnnouncePacket(
        const uint8_t* appData, size_t len, uint8_t context,
        uint8_t* raw, size_t capacity, size_t& rawLen) {
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
        _ctx, RustClock::synchronizedEpochSecs(), _clock.nowMs(), wireValue, ratchet);
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
    // Frame as a HEADER_1 broadcast ANNOUNCE (SINGLE dest). The caller owns admission.
    rawLen = 0;
    st = rs_handheld_rns_packet_build_flagged(
        0, RustWire::PT_ANNOUNCE, RustWire::DT_SINGLE, context, haveRatchet ? 1 : 0, nullptr,
        annDest, out, outLen, raw, capacity, &rawLen);
    if (st != RS_HANDHELD_OK || rawLen == 0) {
        Serial.printf("[RUST] announce frame build failed (%d)\n", (int)st);
        return AnnounceResult::Failed;
    }
    return AnnounceResult::Sent;
}

ProtocolRuntime::AnnounceResult ProtocolRuntime::emitAnnounce(const uint8_t* appData, size_t len) {
    uint8_t raw[500];
    size_t rawLen = 0;
    const auto result = buildAnnouncePacket(appData, len, RustWire::CTX_NONE,
                                           raw, sizeof(raw), rawLen);
    if (result != AnnounceResult::Sent) return result;
    if (!_pump.sendAll(raw, rawLen)) {
        Serial.println("[RUST] announce not accepted by any eligible interface");
        return AnnounceResult::Failed;
    }
    _lastAnnounceMs = millis();
    Serial.printf("[RUST] announce TX %u bytes app=%u%s\n", (unsigned)rawLen, (unsigned)len,
                  (raw[0] & 0x20) ? " ratcheted" : " base-key fallback");
#ifdef PROTOCOL_PACKET_TRACE
    Serial.print("[ANN-WIRE] ");
    for (size_t i = 0; i < rawLen; i++) Serial.printf("%02x", raw[i]);
    Serial.println();
#endif
    return AnnounceResult::Sent;
}

void ProtocolRuntime::onOwnPathRequest(uint8_t ifaceId, const uint8_t tag[16], size_t tagLen) {
    handheld::assertDeviceOwner();
    if (_maintenanceRadio || !_ctx || !_identityLoaded || ifaceId >= PATH_RESPONSE_INTERFACES ||
        !tag || tagLen == 0 || tagLen > 16) return;
    const uint32_t generation = _pump.interfaceGeneration(ifaceId);
    if (!generation) return;
    const uint64_t now = _clock.nowMs();
    auto& response = _pathResponses[ifaceId];
    if (response.generation != generation) response = {};
    if (response.pending && (now < response.bornMs ||
                             now - response.bornMs >= PATH_RESPONSE_MAX_AGE_MS)) {
        response.pending = false;
        response.rawLen = 0;
        response.lease = {};
    }
    promotePathResponse(ifaceId, now);
    if (response.pending) {
        // A fresh forthcoming broadcast serves this physical interface's burst.
        // A cached replay may already have been seen: retain a fresh followup too.
        // Rust's bounded tag cache can evict, so its duplicate gate is not a substitute.
        if (response.replay && !response.followupTagLen &&
            (response.tagLen != tagLen || memcmp(response.tag, tag, tagLen))) {
            memcpy(response.followupTag, tag, tagLen);
            response.followupTagLen = uint8_t(tagLen);
            response.followupBornMs = now;
        }
        // Never replace the first tag, bytes, generation or either original deadline.
        return;
    }

    // Keep exact-tag replay ahead of fresh signing, with the original packet lifetime.
    // A new request may authorize a new output generation; existing pending work cannot.
    const PathResponse* cached = nullptr;
    for (const auto& candidate : _pathResponses) {
        if (candidate.rawLen && candidate.tagLen == tagLen &&
            !memcmp(candidate.tag, tag, tagLen) && now >= candidate.packetBornMs &&
            now - candidate.packetBornMs < PATH_RESPONSE_MAX_AGE_MS) {
            cached = &candidate;
            break;
        }
    }
    response.replay = cached != nullptr;
    if (cached) {
        if (cached != &response) {
            memcpy(response.raw, cached->raw, cached->rawLen);
            response.rawLen = cached->rawLen;
            response.lease = cached->lease;
            response.packetBornMs = cached->packetBornMs;
        }
        response.lease.interfaceId = ifaceId;
        response.lease.generation = generation;
        response.bornMs = response.packetBornMs;
    } else {
        response.rawLen = 0;
        response.lease = {};
        response.bornMs = now;
        response.packetBornMs = 0;
    }
    memcpy(response.tag, tag, tagLen);
    response.tagLen = uint8_t(tagLen);
    response.generation = generation;
    response.readyMs = now + PATH_REQUEST_GRACE_MS;
    if (response.hasLastSent && response.lastSentMs + PATH_RESPONSE_INTERVAL_MS > response.readyMs)
        response.readyMs = response.lastSentMs + PATH_RESPONSE_INTERVAL_MS;
    response.pending = true;
}

void ProtocolRuntime::promotePathResponse(uint8_t ifaceId, uint64_t now) {
    auto& response = _pathResponses[ifaceId];
    if (response.pending || !response.followupTagLen) return;
    if (now < response.followupBornMs ||
        now - response.followupBornMs >= PATH_RESPONSE_MAX_AGE_MS) {
        response.followupTagLen = 0;
        return;
    }
    memcpy(response.tag, response.followupTag, response.followupTagLen);
    response.tagLen = response.followupTagLen;
    response.bornMs = response.followupBornMs;
    response.followupTagLen = 0;
    response.rawLen = 0;
    response.lease = {};
    response.packetBornMs = 0;
    response.replay = false;
    response.readyMs = response.bornMs + PATH_REQUEST_GRACE_MS;
    if (response.hasLastSent && response.lastSentMs + PATH_RESPONSE_INTERVAL_MS > response.readyMs)
        response.readyMs = response.lastSentMs + PATH_RESPONSE_INTERVAL_MS;
    response.pending = true;
}

void ProtocolRuntime::pollPathResponses() {
    const uint64_t now = _clock.nowMs();
    // A newly built signed packet can serve this snapshot of eligible interfaces. Its
    // immutable copies have independent admission, original request age and generation.
    const PathResponse* built = nullptr;
    bool buildUnavailable = false;
    for (uint8_t id = 0; id < PATH_RESPONSE_INTERFACES; ++id) {
        auto& response = _pathResponses[id];
        if (response.generation && response.generation != _pump.interfaceGeneration(id)) {
            response = {};
            continue;
        }
        if (response.pending && (now < response.bornMs ||
                                 now - response.bornMs >= PATH_RESPONSE_MAX_AGE_MS)) {
            response.pending = false;
            response.rawLen = 0;
            response.lease = {};
            Serial.printf("[RUST] path-response expired iface=%u\n", (unsigned)id);
        }
        promotePathResponse(id, now);
        if (!response.pending || now < response.readyMs) continue;
        if (!response.rawLen) {
            if (built) {
                memcpy(response.raw, built->raw, built->rawLen);
                response.rawLen = built->rawLen;
            } else {
                // Flash/signing must yield during an active radio burst; already-built
                // socket replies below still progress independently of that radio.
                if (!pollRadioBeforeBlockingWork()) continue;
                if (buildUnavailable) {
                    response.readyMs = now + 1000;
                    continue;
                }
                size_t rawLen = 0;
                const auto result = buildAnnouncePacket(_lastAppDataLen ? _lastAppData : nullptr,
                    _lastAppDataLen, RustWire::CTX_PATH_RESPONSE,
                    response.raw, sizeof(response.raw), rawLen);
                if (result == AnnounceResult::Deferred) {
                    response.readyMs = now + 1000;
                    buildUnavailable = true;
                    continue;
                }
                if (result != AnnounceResult::Sent) {
                    // Construction/storage failures can be temporary too. Retry within
                    // the unchanged operation deadline, never on every owner loop.
                    response.readyMs = now + 1000;
                    buildUnavailable = true;
                    continue;
                }
                response.rawLen = uint16_t(rawLen);
                built = &response;
            }
            if (!_pump.captureLeaseAt(id, response.raw, response.rawLen, response.bornMs,
                                       PATH_RESPONSE_MAX_AGE_MS, response.lease) ||
                response.lease.generation != response.generation) {
                response.pending = false;
                response.rawLen = 0;
                if (built == &response) built = nullptr;
                continue;
            }
            response.packetBornMs = response.bornMs;
        }
        if (!_pump.leaseLive(response.lease)) {
            response.pending = false;
            response.rawLen = 0;
            if (built == &response) built = nullptr;
            continue;
        }
        if (_pump.sendLeased(response.raw, response.rawLen, response.lease)) {
            response.pending = false;
            response.lastSentMs = now;
            response.hasLastSent = true;
            _lastAnnounceMs = millis();
            Serial.printf("[RUST] path-response TX %u bytes iface=%u\n",
                          (unsigned)response.rawLen, (unsigned)id);
#ifdef PROTOCOL_PACKET_TRACE
            Serial.print("[ANN-WIRE] ");
            for (size_t i = 0; i < response.rawLen; ++i) Serial.printf("%02x", response.raw[i]);
            Serial.println();
#endif
        }
        // A live lease refused by a full queue/socket stays owned here, byte-for-byte.
        // Retry admission at a bounded cadence without moving its original expiry.
        if (response.pending) response.readyMs = now + PATH_RESPONSE_RETRY_MS;
    }
}

handheld::outgoing::Submission ProtocolRuntime::lxmfSubmit(const uint8_t dest[16],
        const uint8_t* title, size_t titleLength, const uint8_t* content,
        size_t contentLength, bool preferLink) {
    handheld::assertDeviceOwner();
    if (!_enginesUp) return {};
    return _lxmf.submit(dest, title, titleLength, content, contentLength, preferLink);
}

handheld::outgoing::Poll ProtocolRuntime::lxmfPoll(handheld::outgoing::Ticket ticket,
        handheld::outgoing::InitialResult& result) const {
    handheld::assertDeviceOwner();
    return _enginesUp ? _lxmf.poll(ticket, result) : handheld::outgoing::Poll::Invalid;
}

bool ProtocolRuntime::lxmfAcknowledge(handheld::outgoing::Ticket ticket) {
    handheld::assertDeviceOwner();
    return _enginesUp && _lxmf.acknowledge(ticket);
}

bool ProtocolRuntime::lxmfCancel(handheld::outgoing::Ticket ticket) {
    handheld::assertDeviceOwner();
    return _enginesUp && _lxmf.cancel(ticket);
}

bool ProtocolRuntime::lxmfStatus(const handheld::storage::RecordKey& key,
        handheld::outgoing::StatusView& result) const {
    handheld::assertDeviceOwner();
    return _enginesUp && _lxmf.status(key, result);
}

uint32_t ProtocolRuntime::lxmfStatusRevision() const {
    handheld::assertDeviceOwner();
    return _enginesUp ? _lxmf.statusRevision() : 0;
}

void ProtocolRuntime::lxmfStopAdmissions() {
    handheld::assertDeviceOwner();
    if (_enginesUp) _lxmf.stopAdmissions();
}

bool ProtocolRuntime::lxmfDrained() const {
    handheld::assertDeviceOwner();
    return !_enginesUp || _lxmf.drained();
}

handheld::storage::Error ProtocolRuntime::lxmfDrainError() const {
    handheld::assertDeviceOwner();
    return _enginesUp ? _lxmf.drainError() : handheld::storage::Error::None;
}

bool ProtocolRuntime::lxmfBeginPeerDelete(const uint8_t peer[16]) {
    handheld::assertDeviceOwner();
    return _enginesUp && _lxmf.beginPeerDelete(peer);
}

void ProtocolRuntime::lxmfFinishPeerDelete(const uint8_t peer[16],
        const handheld::storage::Result& result) {
    handheld::assertDeviceOwner();
    if (_enginesUp) _lxmf.finishPeerDelete(peer, result);
}
