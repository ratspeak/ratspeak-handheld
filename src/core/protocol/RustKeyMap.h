#pragma once

#include <stddef.h>
#include <stdint.h>
#include <vector>

#include "ratspeak_protocol.h"

class FlashStore;
class SDStore;

// Persistence shim for the Rust-owned destination -> identity-key table. The
// table, continuity checks, LRU and profile capacity all live in the lite node;
// C++ owns only flash/SD I/O and the one-time JSON migration.
class RustKeyMap {
public:
    bool begin(FlashStore* flash, rs_handheld_rns_t* ctx, int32_t profile, uint64_t nowMs);

    // Returns false only for an invalid/conflicting binding or unavailable table.
    bool learn(const uint8_t destHash[16], const uint8_t publicKey[64], uint64_t nowMs = 0);
    // Successful recall refreshes Rust LRU recency and dirties the persisted snapshot.
    bool recall(const uint8_t destHash[16], uint8_t out[64]);
    size_t size() const;

    void loop(uint64_t nowMs);
    bool flush(uint64_t nowMs, bool force);

    // Delete-only retirement of micro transport files. keymap.json is excluded:
    // begin() imports that one file before removing it.
    static void cleanupLegacyFiles(FlashStore* flash, SDStore* sd);

private:
    bool restore(uint64_t nowMs);
    bool migrateJson(uint64_t nowMs);
    bool persist(uint64_t nowMs);

    FlashStore* _flash = nullptr;
    rs_handheld_rns_t* _ctx = nullptr;
    std::vector<uint8_t> _blob;
    bool _dirty = false;
    uint64_t _lastSaveMs = 0;

    static constexpr uint64_t SAVE_INTERVAL_MS = 60000;
    static constexpr size_t SMALL_BLOB_MAX = RS_HANDHELD_KNOWN_DESTINATIONS_BLOB_MAX;
    static constexpr size_t MICRO_BLOB_MAX = 7 + 64 * 88;
    static constexpr const char* PATH_BLOB = "/transport/known_dests.bin";
    static constexpr const char* PATH_BLOB_BACKUP = "/transport/known_dests.bin.bak";
    static constexpr const char* PATH_LEGACY_JSON = "/transport/keymap.json";
};
