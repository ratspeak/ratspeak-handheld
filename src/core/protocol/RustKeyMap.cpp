#include "protocol/RustKeyMap.h"

#include "config/Config.h"
#include "storage/FlashStore.h"
#include "storage/SDStore.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <string.h>

namespace {
bool fromHex(const char* hex, size_t hexLen, uint8_t* out, size_t outLen) {
    if (!hex || hexLen != outLen * 2) return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < outLen; i++) {
        int hi = nibble(hex[i * 2]);
        int lo = nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}
}  // namespace

bool RustKeyMap::begin(FlashStore* flash, rs_handheld_rns_t* ctx, int32_t profile,
                       uint64_t nowMs) {
    _flash = flash;
    _ctx = ctx;
    const size_t cap = profile == RS_HANDHELD_PROFILE_MICRO ? MICRO_BLOB_MAX : SMALL_BLOB_MAX;
    _blob.assign(cap, 0);
    _dirty = false;
    _lastSaveMs = nowMs;
    if (!_flash || !_ctx) return false;

    const bool hadBlob = _flash->exists(PATH_BLOB) || _flash->exists(PATH_BLOB_BACKUP);
    bool restored = restore(nowMs);
    if (!restored && _flash->exists(PATH_LEGACY_JSON)) {
        return migrateJson(nowMs);
    } else if (restored && _flash->exists(PATH_LEGACY_JSON)) {
        // A valid Rust blob is authoritative; a leftover JSON is a completed or
        // interrupted migration artifact and must not overwrite newer LRU state.
        _flash->remove(PATH_LEGACY_JSON);
    }
    return restored || !hadBlob;
}

bool RustKeyMap::restore(uint64_t nowMs) {
    if ((!_flash->exists(PATH_BLOB) && !_flash->exists(PATH_BLOB_BACKUP)) || _blob.empty()) return false;
    size_t len = 0;
    if (!_flash->readFileFully(PATH_BLOB, _blob.data(), _blob.size(), len)) {
        Serial.println("[RUST-KEYS] destination blob read failed");
        return false;
    }
    if (rs_handheld_rns_known_dest_import(_ctx, _blob.data(), len, nowMs) != RS_HANDHELD_OK) {
        Serial.println("[RUST-KEYS] destination blob validation failed");
        return false;
    }
    Serial.printf("[RUST-KEYS] restored %u destinations\n", (unsigned)size());
    return true;
}

bool RustKeyMap::migrateJson(uint64_t nowMs) {
    String json = _flash->readString(PATH_LEGACY_JSON);
    if (json.isEmpty()) return false;
    JsonDocument doc;
    if (deserializeJson(doc, json) || !doc.is<JsonObject>()) {
        Serial.println("[RUST-KEYS] legacy keymap JSON invalid; retained");
        return false;
    }

    size_t imported = 0;
    for (JsonPair kv : doc.as<JsonObject>()) {
        const char* keyHex = kv.key().c_str();
        const char* publicHex = kv.value().as<const char*>();
        uint8_t dest[16], publicKey[64];
        if (!fromHex(keyHex, strlen(keyHex), dest, sizeof(dest)) || !publicHex ||
            !fromHex(publicHex, strlen(publicHex), publicKey, sizeof(publicKey))) {
            continue;
        }
        int32_t changed = 0;
        if (rs_handheld_rns_known_dest_learn(_ctx, dest, publicKey, nowMs, &changed) ==
            RS_HANDHELD_OK) {
            imported++;
        }
    }

    // Even an empty, syntactically-valid legacy object migrates to a valid empty
    // blob. Delete JSON only after atomic write + readback + Rust validation.
    _dirty = true;
    if (!flush(nowMs, true)) {
        Serial.println("[RUST-KEYS] legacy keymap migration persist failed; retained");
        return false;
    }
    _flash->remove(PATH_LEGACY_JSON);
    Serial.printf("[RUST-KEYS] migrated %u legacy destinations\n", (unsigned)imported);
    return true;
}

bool RustKeyMap::learn(const uint8_t destHash[16], const uint8_t publicKey[64], uint64_t nowMs) {
    if (!_ctx || !destHash || !publicKey) return false;
    if (nowMs == 0) nowMs = millis();
    int32_t changed = 0;
    const rs_handheld_status_t st =
        rs_handheld_rns_known_dest_learn(_ctx, destHash, publicKey, nowMs, &changed);
    if (st != RS_HANDHELD_OK) return false;
    if (changed) _dirty = true;
    return true;
}

bool RustKeyMap::recall(const uint8_t destHash[16], uint8_t out[64]) {
    if (!_ctx || !destHash || !out) return false;
    int32_t found = 0;
    if (rs_handheld_rns_known_dest_recall(_ctx, destHash, out, &found) != RS_HANDHELD_OK ||
        !found) {
        return false;
    }
    _dirty = true;  // recall refreshes LRU recency inside Rust
    return true;
}

size_t RustKeyMap::size() const {
    if (!_ctx) return 0;
    uint32_t count = 0;
    return rs_handheld_rns_known_dest_count(_ctx, &count) == RS_HANDHELD_OK ? count : 0;
}

void RustKeyMap::loop(uint64_t nowMs) { flush(nowMs, false); }

bool RustKeyMap::flush(uint64_t nowMs, bool force) {
    if (!_dirty) return true;
    if (!force && nowMs - _lastSaveMs < SAVE_INTERVAL_MS) return true;
    return persist(nowMs);
}

bool RustKeyMap::persist(uint64_t nowMs) {
    if (!_flash || !_ctx || _blob.empty()) return false;
    size_t len = 0;
    if (rs_handheld_rns_known_dest_export(_ctx, _blob.data(), _blob.size(), &len) !=
            RS_HANDHELD_OK ||
        len == 0 || !_flash->writeAtomic(PATH_BLOB, _blob.data(), len)) {
        return false;
    }

    size_t readLen = 0;
    if (!_flash->readFileFully(PATH_BLOB, _blob.data(), _blob.size(), readLen) || readLen != len ||
        rs_handheld_rns_known_dest_import(_ctx, _blob.data(), readLen, nowMs) != RS_HANDHELD_OK) {
        Serial.println("[RUST-KEYS] destination blob readback validation failed");
        return false;
    }
    _dirty = false;
    _lastSaveMs = nowMs;
    return true;
}

void RustKeyMap::cleanupLegacyFiles(FlashStore* flash, SDStore* sd) {
    if (flash) {
        static const char* paths[] = {
            "/known_destinations", "/destination_table", "/packet_hashlist",
            "/transport/paths.msgpack",
        };
        for (const char* path : paths) {
            if (flash->exists(path) && flash->remove(path)) {
                Serial.printf("[RUST] removed retired transport file %s\n", path);
            }
        }
    }
    if (sd && sd->isReady()) {
        String path = String(SD_PATH_TRANSPORT) + "/known_destinations";
        if (sd->exists(path.c_str()) && sd->remove(path.c_str())) {
            Serial.printf("[RUST] removed retired transport file %s\n", path.c_str());
        }
    }
}
