#pragma once

#include <stddef.h>
#include <stdint.h>
#include "ratspeak_protocol.h"

class FlashStore;

// C++-owned persistence for the Rust announce-ratchet state (2026-07-19 sprint).
//
// Rust owns the ring, announce-order state, peer memory, and every crypto decision; this class
// moves their opaque blobs between flash and the FFI and supplies separate host clocks. Upstream
// posture: always enable, never enforce.
//
// FAIL-SAFE DIVERGENCE (deliberate, adjudicated): if the ring cannot be persisted we announce
// WITHOUT a ratchet instead of announcing one we could lose on reboot. Upstream Python announces
// regardless and logs; on a device a lost ring means peers keep encrypting to a key we no longer
// hold and those messages are unreadable. Degrading to the base key costs forward secrecy for
// that window but never loses a message.
class RustRatchetStore {
public:
    enum class AnnounceMode : uint8_t {
        Deferred,
        BaseKey,
        Ratcheted,
    };

    // Restore the signed identity ring/order state and the shared public peer table. Call AFTER
    // the identity is loaded: both private-state files are identity-scoped and signed.
    void begin(FlashStore* flash, rs_handheld_rns_t* ctx, const uint8_t identityHash[16],
               uint64_t wallSecs, uint64_t uptimeMs);

    // Transactionally persist the next wire-order value and any due ratchet change. Deferred
    // means wall order did not advance; BaseKey is the explicit persistence-failure fallback;
    // Ratcheted returns an exact durably committed public key and order.
    AnnounceMode prepareAnnounce(rs_handheld_rns_t* ctx, uint64_t wallSecs, uint64_t uptimeMs,
                                 uint64_t& outWireValue, uint8_t outPub[32]);

    // Commit an announced public ratchet only after transport freshness and KeyMap continuity
    // have both accepted it. Re-observing the same key is a strict no-op in Rust.
    bool rememberPeer(rs_handheld_rns_t* ctx, const uint8_t destinationHash[16],
                      const uint8_t ratchet[32], uint64_t wallSecs, uint64_t uptimeMs);

    // Persist the peer table if dirty. Throttled unless `force` (shutdown / persistData).
    bool flushPeers(rs_handheld_rns_t* ctx, uint32_t nowMs, bool force);

    bool ringReady() const { return _havePub; }

private:
    enum class PreparedKind : uint8_t { Ring, AnnounceState };

    bool restoreRing(rs_handheld_rns_t* ctx);
    bool restoreAnnounceState(rs_handheld_rns_t* ctx);
    bool restorePeers(rs_handheld_rns_t* ctx, uint64_t wallSecs, uint64_t uptimeMs);
    bool writePreparedAndCommit(rs_handheld_rns_t* ctx, const char* path, size_t len,
                                PreparedKind kind, uint64_t& outWireValue,
                                uint8_t outPub[32]);
    bool quarantine(const char* path, const char* label);
    static void secureZero(uint8_t* data, size_t len);
    static uint64_t fingerprint(const uint8_t* data, size_t len);

    FlashStore* _flash = nullptr;
    // Identity-scoped so switching identity slots cannot destroy another identity's ring:
    // the blob is signed by the identity that owns it, and a foreign blob is rejected.
    char _ringPath[64] = {};
    char _announceStatePath[64] = {};
    uint8_t _persistedPub[32] = {};
    // One reusable BSS-resident buffer for every opaque blob. The Cardputer loop task has an
    // 8 KiB stack; putting the 2.7 KiB ring or 1.8 KiB peer table there is not acceptable.
    uint8_t _blob[RS_HANDHELD_RATCHET_RING_BLOB_MAX] = {};
    bool _havePub = false;
    bool _peersDirty = false;
    uint32_t _lastPeerSaveMs = 0;
    // Peer-table writes are cheap but flash is not; one write per minute at most.
    static constexpr uint32_t PEER_SAVE_INTERVAL_MS = 60000;
    // Peer ratchets are peer-owned public keys — identity-independent, so one shared file.
    static constexpr const char* PATH_PEERS = "/transport/peer_ratchets.bin";
};
