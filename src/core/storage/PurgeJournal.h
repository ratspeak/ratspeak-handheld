#pragma once

#include "PreparedEnvelope.h"

namespace handheld::storage::purge {
// One flash-authoritative journal per local identity. Public metadata only.
// A valid checksum is not permission to purge: the executor also revalidates
// the referenced incoming record and its durable local-identity binding.
constexpr size_t Size = 132;
struct Journal {
    uint8_t bytes[Size] = {};
    const uint8_t* local() const { return bytes + 16; }
    const uint8_t* node() const { return bytes + 32; }
    const uint8_t* messageId() const { return bytes + 64; }
    const uint8_t* transientId() const { return bytes + 96; }
    uint32_t revision() const { return prepared::read32(bytes + 8); }
    RecordKey key() const {
        RecordKey key; key.incoming = true; key.counter = prepared::read32(bytes + 4);
        std::memcpy(key.peer, bytes + 48, 16); return key;
    }
    bool valid() const {
        return !std::memcmp(bytes, "HPJ1", 4) && key().counter && revision() &&
            !prepared::read32(bytes + 12) && prepared::read32(bytes + 128) ==
                ~prepared::checksum(UINT32_MAX, bytes, 128);
    }
    static bool make(Journal& out, const RecordKey& key, uint32_t revision,
                     const uint8_t local[16], const uint8_t node[16],
                     const uint8_t messageId[32], const uint8_t transientId[32]) {
        if (!key.incoming || !key.counter || !revision || !local || !node || !messageId || !transientId)
            return false;
        out = {}; std::memcpy(out.bytes, "HPJ1", 4);
        prepared::write32(out.bytes + 4, key.counter); prepared::write32(out.bytes + 8, revision);
        std::memcpy(out.bytes + 16, local, 16); std::memcpy(out.bytes + 32, node, 16);
        std::memcpy(out.bytes + 48, key.peer, 16); std::memcpy(out.bytes + 64, messageId, 32);
        std::memcpy(out.bytes + 96, transientId, 32);
        prepared::write32(out.bytes + 128, ~prepared::checksum(UINT32_MAX, out.bytes, 128));
        return true;
    }
};
static_assert(sizeof(Journal) == Size && Size <= Budget::SmallPayload,
              "Purge journal must fit an existing small storage credit");
}
