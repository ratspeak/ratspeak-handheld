#pragma once

#include <stdint.h>

namespace RustLinkTiming {

// Trusted Reticulum timing: one normalised protocol MTU of first-hop serialization plus the
// six-second baseline, followed by six seconds for every hop. A zero bitrate preserves the
// authoritative unavailable-interface fallback.
constexpr uint32_t PROTOCOL_MTU_BYTES = 500;
constexpr uint32_t PER_HOP_TIMEOUT_MS = 6000;

constexpr uint32_t serializationMs(uint32_t bitrateBps) {
    return bitrateBps == 0
               ? 0
               : static_cast<uint32_t>(
                     ((uint64_t)PROTOCOL_MTU_BYTES * 8U * 1000U + bitrateBps - 1U) /
                     bitrateBps);
}

constexpr uint32_t establishmentTimeoutMs(uint8_t hops, uint32_t firstHopBitrateBps) {
    return PER_HOP_TIMEOUT_MS + serializationMs(firstHopBitrateBps) +
           PER_HOP_TIMEOUT_MS * (hops < 1 ? 1U : static_cast<uint32_t>(hops));
}

static_assert(establishmentTimeoutMs(1, 0) == 12000, "unknown-bitrate fallback drifted");
static_assert(establishmentTimeoutMs(1, 3515) == 13138, "Medium Fast timing drifted");
static_assert(establishmentTimeoutMs(1, 1070) == 15739, "Long Fast timing drifted");

}  // namespace RustLinkTiming
