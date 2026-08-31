#pragma once

#include <stdint.h>

#include "protocol/RustKeyMap.h"
#include "protocol/RustRatchetStore.h"
#include "ratspeak_protocol.h"

// The single C++ acceptance seam for a validated lxmf.delivery announce. Transport freshness is
// enforced before this is called. Continuity must accept before the public peer-ratchet table is
// allowed to change; keeping the ordering in one helper makes that security property testable.
class RustAnnouncePolicy {
public:
    static bool accept(rs_handheld_rns_t* ctx, RustKeyMap& keymap, RustRatchetStore& ratchets,
                       const rs_handheld_announce_event_t& event, uint64_t wallSecs,
                       uint64_t uptimeMs) {
        if (!keymap.learn(event.destination_hash, event.public_key, uptimeMs)) return false;
        if (event.has_ratchet) {
            ratchets.rememberPeer(ctx, event.destination_hash, event.ratchet, wallSecs, uptimeMs);
        }
        return true;
    }
};

