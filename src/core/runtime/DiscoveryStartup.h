#pragma once

#include <memory>
#include <new>
#include "protocol/ProtocolRuntime.h"
#include "reticulum/AnnounceManager.h"
#include "util/AnnounceData.h"

namespace handheld {

// Setup owns the temporary until all allocating discovery work has succeeded.
// Call before the first protocol poll; no partially loaded bridge is published.
inline bool prepareDiscovery(ProtocolRuntime& runtime, SDStore& sd, FlashStore& flash,
                             const String& name, AnnounceManager*& output) noexcept {
    if (output) return false;
    try {
        auto candidate = std::make_unique<AnnounceManager>("lxmf.delivery");
        candidate->setStorage(&sd, &flash);
        candidate->setLocalDestHash(rs::Bytes(runtime.localDestHash(), 16));
        candidate->loadContacts();
        candidate->loadNameCache();
        // The first path response uses the same name/capabilities as an announce.
        const auto seed = encodeAnnounceName(name, rs::Bytes(runtime.localDestHash(), 16).toHex().c_str());
        runtime.seedAnnounceAppData(seed.data(), seed.size());
        runtime.setAnnounceManager(candidate.get());
        output = candidate.release();
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

} // namespace handheld
