#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "runtime/ResourceBudget.h"

namespace handheld::propagation {
// Metadata enters only after signed announce freshness and KeyMap continuity.
// Public destination metadata is shared across local identities, like KeyMap.
// Restored policy requires trusted wall time; current-boot policy uses monotonic
// age. Unknown/expired is never silently interpreted as free delivery.
class RecipientStampCosts {
public:
    static constexpr size_t Capacity = 32, BlobMax = 8 + Capacity * 25 + 4;
    static constexpr uint64_t LifetimeSecs = 45ULL * 24 * 60 * 60;
    bool dirty() const { return _dirty; }
    void committed() { _dirty = false; }
    void learn(const uint8_t destination[16], bool known, uint8_t cost, uint64_t wall, uint64_t mono) {
        Entry* slot = nullptr;
        for (auto& entry : _entries)
            if (entry.used && !std::memcmp(entry.destination, destination, 16)) { slot = &entry; break; }
        if (!known) {
            if (slot) { *slot = {}; _dirty = true; }
            return;
        }
        if (!slot) {
            for (auto& entry : _entries) if (!entry.used) { slot = &entry; break; }
        }
        if (!slot) {
            slot = &_entries[0];
            for (auto& entry : _entries) if (entry.seen < slot->seen) slot = &entry;
        }
        *slot = {}; std::memcpy(slot->destination, destination, 16);
        slot->used = true; slot->cost = cost; slot->recorded = wall; slot->seen = mono;
        _dirty = true;
    }
    bool get(const uint8_t destination[16], uint64_t wall, uint64_t mono, uint8_t& cost) const {
        for (const auto& entry : _entries) {
            if (!entry.used || std::memcmp(entry.destination, destination, 16)) continue;
            const bool valid = entry.restored
                ? wall && entry.recorded && wall >= entry.recorded && wall - entry.recorded < LifetimeSecs
                : mono >= entry.seen && mono - entry.seen < LifetimeSecs * 1000;
            if (valid) cost = entry.cost;
            return valid;
        }
        return false;
    }
    size_t encode(uint8_t* output, size_t capacity) const {
        if (!output || capacity < BlobMax) return 0;
        std::memset(output, 0, 8); std::memcpy(output, "RSC1", 4);
        size_t length = 8;
        for (const auto& entry : _entries) {
            if (!entry.used) continue;
            std::memcpy(output + length, entry.destination, 16);
            for (unsigned i = 0; i < 8; ++i) output[length + 16 + i] = uint8_t(entry.recorded >> (8 * i));
            output[length + 24] = entry.cost; length += 25; ++output[4];
        }
        const auto sum = checksum(output, length);
        for (unsigned i = 0; i < 4; ++i) output[length + i] = uint8_t(sum >> (8 * i));
        return length + 4;
    }
    bool restore(const uint8_t* bytes, size_t length) {
        if (!bytes || length < 12 || length > BlobMax || std::memcmp(bytes, "RSC1", 4) ||
            bytes[4] > Capacity || bytes[5] || bytes[6] || bytes[7] || length != 12 + size_t(bytes[4]) * 25) return false;
        uint32_t sum = 0;
        for (unsigned i = 0; i < 4; ++i) sum |= uint32_t(bytes[length - 4 + i]) << (8 * i);
        if (checksum(bytes, length - 4) != sum) return false;
        // Validate duplicate keys before publishing any row, without a second table.
        for (size_t i = 0; i < bytes[4]; ++i)
            for (size_t j = 0; j < i; ++j)
                if (!std::memcmp(bytes + 8 + i * 25, bytes + 8 + j * 25, 16)) return false;
        for (auto& entry : _entries) entry = {};
        for (size_t i = 0; i < bytes[4]; ++i) {
            const auto* input = bytes + 8 + i * 25; auto& entry = _entries[i];
            std::memcpy(entry.destination, input, 16);
            for (unsigned j = 0; j < 8; ++j) entry.recorded |= uint64_t(input[16 + j]) << (8 * j);
            entry.used = entry.restored = true; entry.cost = input[24];
        }
        _dirty = false; return true;
    }
private:
    // Storage corruption detection only; authentication belongs to announces.
    static uint32_t checksum(const uint8_t* bytes, size_t length) {
        uint32_t hash = 2166136261u;
        while (length--) { hash ^= *bytes++; hash *= 16777619u; }
        return hash;
    }
    struct Entry {
        uint8_t destination[16]{};
        uint64_t recorded = 0, seen = 0;
        uint8_t cost = 0;
        bool used = false, restored = false;
    };
    Entry _entries[Capacity]{};
    bool _dirty = false;
};
static_assert(sizeof(RecipientStampCosts) <= ResourceBudget::RecipientStampCosts,
              "Recipient stamp policies exceed their retained budget");
}
