#pragma once

#include <stdint.h>

// Reticulum discriminants plus the two wire-neutral helpers that stay C++ policy.
// Packet byte construction is owned by rs_handheld_rns_packet_build.
namespace RustWire {

enum PacketType : uint8_t { PT_DATA = 0, PT_ANNOUNCE = 1, PT_LINKREQUEST = 2, PT_PROOF = 3 };
enum DestType : uint8_t { DT_SINGLE = 0, DT_GROUP = 1, DT_PLAIN = 2, DT_LINK = 3 };

// Context bytes (wire.rs PacketContext).
enum Ctx : uint8_t {
    CTX_NONE = 0x00,
    CTX_RESOURCE = 0x01,
    CTX_RESOURCE_ADV = 0x02,
    CTX_RESOURCE_REQ = 0x03,
    CTX_RESOURCE_PRF = 0x05,
    CTX_RESOURCE_ICL = 0x06,
    CTX_RESOURCE_RCL = 0x07,
    CTX_PATH_RESPONSE = 0x0B,  // Packet.PATH_RESPONSE: announce answering a path request (rate-exempt)
    CTX_KEEPALIVE = 0xFA,
    CTX_LINKCLOSE = 0xFC,
    CTX_LRRTT = 0xFE,
    CTX_LRPROOF = 0xFF,
};

// header_type of a raw packet (top 2 bits of flags) — for rs_handheld_rns_packet_hash.
inline int32_t headerTypeOf(const uint8_t* raw) {
    return ((raw[0] & 0x40) != 0) ? 1 : 0;
}

// Path-request self-response throttle (fix map §4), as a pure function so it is host-testable.
// On an inbound path request for our own dest, schedule a re-announce after a grace window that
// coalesces a burst (layer 2), unless one is already scheduled or we answered within the dedup
// window (layer 3). `pendingUntil`/`lastRespMs` are the caller's timers (0 = unset); `now` is
// millis(). Returns true and advances `pendingUntil` iff a NEW answer was scheduled.
inline bool schedulePathResponse(unsigned long now, unsigned long graceMs, unsigned long dedupMs,
                                 unsigned long& pendingUntil, unsigned long lastRespMs) {
    if (pendingUntil != 0) return false;                                    // already coalescing
    if (lastRespMs != 0 && now - lastRespMs < dedupMs) return false;        // answered recently
    pendingUntil = now + graceMs;
    return true;
}

}  // namespace RustWire
