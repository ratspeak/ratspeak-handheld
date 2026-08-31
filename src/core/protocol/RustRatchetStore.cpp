
#include "protocol/RustRatchetStore.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "protocol/RustEntropy.h"
#include "storage/FlashStore.h"

namespace {
constexpr size_t IDENTITY_HASH_LEN = 16;

void identityPath(char* out, size_t outLen, const char* stem,
                  const uint8_t identityHash[IDENTITY_HASH_LEN]) {
    // Not "HEX": Arduino Print.h #defines HEX/DEC/OCT/BIN.
    static const char HEX_DIGITS[] = "0123456789abcdef";
    const int prefixLen = snprintf(out, outLen, "/transport/%s_", stem);
    if (prefixLen < 0 || static_cast<size_t>(prefixLen) + IDENTITY_HASH_LEN * 2 + 5 > outLen) {
        if (outLen) out[0] = '\0';
        return;
    }
    size_t pos = static_cast<size_t>(prefixLen);
    for (size_t i = 0; i < IDENTITY_HASH_LEN; ++i) {
        out[pos++] = HEX_DIGITS[identityHash[i] >> 4];
        out[pos++] = HEX_DIGITS[identityHash[i] & 0x0f];
    }
    memcpy(out + pos, ".bin", 5);
}

bool backupPath(char* out, size_t outLen, const char* path) {
    if (!out || outLen == 0 || !path) return false;
    const int written = snprintf(out, outLen, "%s.bak", path);
    if (written < 0 || static_cast<size_t>(written) >= outLen) {
        out[0] = '\0';
        return false;
    }
    return true;
}

bool stateFileExists(FlashStore* flash, const char* path) {
    if (!flash || !path) return false;
    if (flash->exists(path)) return true;
    char backup[96];
    return backupPath(backup, sizeof(backup), path) && flash->exists(backup);
}
}  // namespace

void RustRatchetStore::secureZero(uint8_t* data, size_t len) {
    volatile uint8_t* p = data;
    while (len--) *p++ = 0;
}

uint64_t RustRatchetStore::fingerprint(const uint8_t* data, size_t len) {
    // The peer table contains public keys only. This fingerprint is a readback-corruption check,
    // not an authenticity mechanism (own ring/order blobs use signed exact-candidate commits).
    uint64_t value = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < len; ++i) {
        value ^= data[i];
        value *= UINT64_C(1099511628211);
    }
    return value;
}

bool RustRatchetStore::quarantine(const char* path, const char* label) {
    if (!_flash || !path) return true;
    const char* source = path;
    char backup[96];
    if (!_flash->exists(source)) {
        if (!backupPath(backup, sizeof(backup), path) || !_flash->exists(backup)) return true;
        source = backup;
    }
    char target[96];
    for (unsigned i = 0; i < 10; ++i) {
        if (i == 0)
            snprintf(target, sizeof(target), "%s.corrupt", path);
        else
            snprintf(target, sizeof(target), "%s.corrupt.%u", path, i);
        if (_flash->exists(target)) continue;
        if (_flash->rename(source, target)) {
            Serial.printf("[RUST] preserved rejected %s state as %s\n", label, target);
            return true;
        }
        break;
    }
    Serial.printf("[RUST] could not quarantine rejected %s state; primary preserved\n", label);
    return false;
}

bool RustRatchetStore::restoreRing(rs_handheld_rns_t* ctx) {
    if (!stateFileExists(_flash, _ringPath)) return true;
    size_t len = 0;
    const bool read = _flash->readFileFully(_ringPath, _blob, sizeof(_blob), len);
    const rs_handheld_status_t seeded =
        read ? rs_handheld_rns_ratchet_seed(ctx, _blob, len) : RS_HANDHELD_ERR_INVALID_ARG;
    secureZero(_blob, sizeof(_blob));
    if (!read || seeded != RS_HANDHELD_OK) {
        Serial.printf("[RUST] ratchet ring rejected (%d); preserving for diagnosis\n",
                      static_cast<int>(seeded));
        quarantine(_ringPath, "ratchet ring");
        return false;
    }

    if (rs_handheld_rns_ratchet_current(ctx, _persistedPub) == RS_HANDHELD_OK) {
        _havePub = true;
        Serial.println("[RUST] ratchet ring restored");
    } else {
        // A valid signed empty ring is harmless; the next announce creates its first key.
        Serial.println("[RUST] empty ratchet ring restored");
    }
    return true;
}

bool RustRatchetStore::restoreAnnounceState(rs_handheld_rns_t* ctx) {
    if (!stateFileExists(_flash, _announceStatePath)) return true;
    size_t len = 0;
    const bool read = _flash->readFileFully(_announceStatePath, _blob,
                                            RS_HANDHELD_ANNOUNCE_STATE_BLOB_MAX, len);
    const rs_handheld_status_t seeded =
        read ? rs_handheld_rns_announce_state_seed(ctx, _blob, len)
             : RS_HANDHELD_ERR_INVALID_ARG;
    secureZero(_blob, sizeof(_blob));
    if (!read || seeded != RS_HANDHELD_OK) {
        Serial.printf("[RUST] announce-order state rejected (%d); preserving for diagnosis\n",
                      static_cast<int>(seeded));
        quarantine(_announceStatePath, "announce-order");
        return false;
    }
    Serial.println("[RUST] announce-order state restored");
    return true;
}

bool RustRatchetStore::restorePeers(rs_handheld_rns_t* ctx, uint64_t wallSecs,
                                    uint64_t uptimeMs) {
    if (!stateFileExists(_flash, PATH_PEERS)) return true;
    size_t len = 0;
    int32_t changed = 0;
    const bool read = _flash->readFileFully(PATH_PEERS, _blob,
                                            RS_HANDHELD_PEER_RATCHETS_BLOB_MAX, len);
    const rs_handheld_status_t seeded =
        read ? rs_handheld_rns_peer_ratchets_seed_at(ctx, _blob, len, wallSecs, uptimeMs,
                                                     &changed)
             : RS_HANDHELD_ERR_INVALID_ARG;
    secureZero(_blob, sizeof(_blob));
    if (!read || seeded != RS_HANDHELD_OK) {
        Serial.printf("[RUST] peer-ratchet table rejected (%d); preserving for diagnosis\n",
                      static_cast<int>(seeded));
        quarantine(PATH_PEERS, "peer-ratchet");
        return false;
    }
    _peersDirty = changed != 0;
    if (_peersDirty) {
        Serial.println("[RUST] peer-ratchet ages anchored; v2 persistence pending");
    } else {
        Serial.println("[RUST] peer-ratchet table restored");
    }
    return true;
}

void RustRatchetStore::begin(FlashStore* flash, rs_handheld_rns_t* ctx,
                             const uint8_t identityHash[16], uint64_t wallSecs,
                             uint64_t uptimeMs) {
    secureZero(_blob, sizeof(_blob));
    secureZero(_persistedPub, sizeof(_persistedPub));
    memset(_ringPath, 0, sizeof(_ringPath));
    memset(_announceStatePath, 0, sizeof(_announceStatePath));
    _flash = flash;
    _havePub = false;
    _peersDirty = false;
    _lastPeerSaveMs = static_cast<uint32_t>(uptimeMs);
    if (!_flash || !ctx || !identityHash) return;

    // Use all 128 identity-hash bits. Short prefixes can collide across identity slots and make
    // one slot quarantine or overwrite another slot's still-advertised private ratchet ring.
    identityPath(_ringPath, sizeof(_ringPath), "ratchet", identityHash);
    identityPath(_announceStatePath, sizeof(_announceStatePath), "announce", identityHash);
    if (_ringPath[0] == '\0' || _announceStatePath[0] == '\0') {
        Serial.println("[RUST] ratchet persistence path construction failed");
        return;
    }

    restoreRing(ctx);
    restoreAnnounceState(ctx);
    restorePeers(ctx, wallSecs, uptimeMs);
}

bool RustRatchetStore::writePreparedAndCommit(rs_handheld_rns_t* ctx, const char* path,
                                              size_t len, PreparedKind kind,
                                              uint64_t& outWireValue, uint8_t outPub[32]) {
    if (!_flash || !ctx || !path || len == 0 || len > sizeof(_blob)) {
        secureZero(_blob, sizeof(_blob));
        return false;
    }
    if (!_flash->writeAtomic(path, _blob, len)) {
        secureZero(_blob, sizeof(_blob));
        return false;
    }

    // Commit from a fresh, complete read of the promoted primary. The Rust side checks the
    // identity signature and the exact candidate digest prepared earlier, so neither a torn
    // write nor a different valid older blob can become advertisable.
    secureZero(_blob, sizeof(_blob));
    size_t readLen = 0;
    if (!_flash->readFileFully(path, _blob, sizeof(_blob), readLen) || readLen != len) {
        secureZero(_blob, sizeof(_blob));
        quarantine(path, kind == PreparedKind::Ring ? "ratchet ring" : "announce-order");
        return false;
    }

    rs_handheld_status_t st;
    if (kind == PreparedKind::Ring) {
        st = rs_handheld_rns_ratchet_commit(ctx, _blob, readLen, outPub);
    } else {
        st = rs_handheld_rns_announce_state_commit(ctx, _blob, readLen, &outWireValue);
    }
    secureZero(_blob, sizeof(_blob));
    if (st != RS_HANDHELD_OK) {
        Serial.printf("[RUST] persisted %s candidate rejected on commit (%d)\n",
                      kind == PreparedKind::Ring ? "ratchet" : "announce-order",
                      static_cast<int>(st));
        quarantine(path, kind == PreparedKind::Ring ? "ratchet ring" : "announce-order");
        return false;
    }
    return true;
}

RustRatchetStore::AnnounceMode RustRatchetStore::prepareAnnounce(
    rs_handheld_rns_t* ctx, uint64_t wallSecs, uint64_t uptimeMs, uint64_t& outWireValue,
    uint8_t outPub[32]) {
    outWireValue = wallSecs ? wallSecs : uptimeMs / 1000;
    memset(outPub, 0, 32);
    if (!ctx) return AnnounceMode::BaseKey;

    size_t stateLen = 0;
    int32_t ready = 0;
    uint64_t candidateWireValue = 0;
    rs_handheld_status_t st = rs_handheld_rns_announce_state_prepare(
        ctx, wallSecs, _blob, sizeof(_blob), &stateLen, &candidateWireValue, &ready);
    if (st != RS_HANDHELD_OK) {
        secureZero(_blob, sizeof(_blob));
        Serial.printf("[RUST] announce-order prepare failed (%d); announcing base-key\n",
                      static_cast<int>(st));
        return AnnounceMode::BaseKey;
    }
    outWireValue = candidateWireValue;
    if (!ready) {
        secureZero(_blob, sizeof(_blob));
        return AnnounceMode::Deferred;
    }
    if (stateLen != RS_HANDHELD_ANNOUNCE_STATE_BLOB_MAX ||
        !writePreparedAndCommit(ctx, _announceStatePath, stateLen,
                                PreparedKind::AnnounceState, outWireValue, outPub)) {
        Serial.println("[RUST] announce-order persist/verify FAILED; announcing base-key");
        return AnnounceMode::BaseKey;
    }

    uint8_t entropy[32];
    RustEntropy::fill(entropy, sizeof(entropy));
    size_t ringLen = 0;
    int32_t action = RS_HANDHELD_RATCHET_PREP_UNCHANGED;
    st = rs_handheld_rns_ratchet_prepare(ctx, entropy, wallSecs, uptimeMs, _blob,
                                         sizeof(_blob), &ringLen, &action, outPub);
    secureZero(entropy, sizeof(entropy));
    if (st != RS_HANDHELD_OK) {
        secureZero(_blob, sizeof(_blob));
        Serial.printf("[RUST] ratchet prepare failed (%d); announcing base-key\n",
                      static_cast<int>(st));
        return AnnounceMode::BaseKey;
    }

    if (action == RS_HANDHELD_RATCHET_PREP_UNCHANGED ||
        action == RS_HANDHELD_RATCHET_PREP_FULL_RING_PROTECTED) {
        secureZero(_blob, sizeof(_blob));
        if (ringLen != 0) {
            Serial.println("[RUST] ratchet prepare returned inconsistent unchanged state");
            return AnnounceMode::BaseKey;
        }
        if (action == RS_HANDHELD_RATCHET_PREP_FULL_RING_PROTECTED) {
            Serial.println("[RUST] ratchet ring full with unknown-age key; retaining current key");
        }
        memcpy(_persistedPub, outPub, 32);
        _havePub = true;
        return AnnounceMode::Ratcheted;
    }

    if ((action != RS_HANDHELD_RATCHET_PREP_PERSIST_METADATA &&
         action != RS_HANDHELD_RATCHET_PREP_ROTATED) ||
        ringLen == 0 ||
        !writePreparedAndCommit(ctx, _ringPath, ringLen, PreparedKind::Ring,
                                outWireValue, outPub)) {
        Serial.println("[RUST] ratchet persist/verify FAILED; announcing base-key");
        return AnnounceMode::BaseKey;
    }

    memcpy(_persistedPub, outPub, 32);
    _havePub = true;
    Serial.println(action == RS_HANDHELD_RATCHET_PREP_ROTATED
                       ? "[RUST] ratchet rotated + durably committed"
                       : "[RUST] ratchet metadata durably committed");
    return AnnounceMode::Ratcheted;
}

bool RustRatchetStore::rememberPeer(rs_handheld_rns_t* ctx,
                                    const uint8_t destinationHash[16],
                                    const uint8_t ratchet[32], uint64_t wallSecs,
                                    uint64_t uptimeMs) {
    if (!ctx || !destinationHash || !ratchet) return false;
    int32_t changed = 0;
    const rs_handheld_status_t st = rs_handheld_rns_peer_ratchet_remember(
        ctx, destinationHash, ratchet, wallSecs, uptimeMs, &changed);
    if (st != RS_HANDHELD_OK) {
        Serial.printf("[RUST] peer-ratchet remember failed (%d)\n", static_cast<int>(st));
        return false;
    }
    if (changed) _peersDirty = true;
    return changed != 0;
}

bool RustRatchetStore::flushPeers(rs_handheld_rns_t* ctx, uint32_t nowMs, bool force) {
    if (!_peersDirty) return true;
    if (!_flash || !ctx) return false;
    if (!force && static_cast<uint32_t>(nowMs - _lastPeerSaveMs) < PEER_SAVE_INTERVAL_MS) return false;
    // Stamp the attempt: a failed flash device must not be hammered on every loop pass.
    _lastPeerSaveMs = nowMs;

    size_t len = 0;
    if (rs_handheld_rns_peer_ratchets_export(ctx, _blob, sizeof(_blob), &len) !=
            RS_HANDHELD_OK ||
        len == 0 || len > RS_HANDHELD_PEER_RATCHETS_BLOB_MAX) {
        secureZero(_blob, sizeof(_blob));
        Serial.println("[RUST] peer-ratchet export failed");
        return false;
    }
    const uint64_t expected = fingerprint(_blob, len);
    if (!_flash->writeAtomic(PATH_PEERS, _blob, len)) {
        secureZero(_blob, sizeof(_blob));
        Serial.println("[RUST] peer-ratchet persist failed");
        return false;
    }
    secureZero(_blob, sizeof(_blob));
    size_t readLen = 0;
    const bool verified = _flash->readFileFully(PATH_PEERS, _blob,
                                                RS_HANDHELD_PEER_RATCHETS_BLOB_MAX, readLen) &&
                          readLen == len && fingerprint(_blob, readLen) == expected;
    secureZero(_blob, sizeof(_blob));
    if (!verified) {
        Serial.println("[RUST] peer-ratchet readback verification failed");
        quarantine(PATH_PEERS, "peer-ratchet");
        return false;
    }
    _peersDirty = false;
    return true;
}
