#include "MaintenanceOperation.h"
#include "FactoryReset.h"
#include "protocol/ProtocolBackend.h"
#include "config/UserConfig.h"
#include "config/SettingsTransaction.h"
#include "reticulum/IdentityManager.h"
#include "reticulum/AnnounceManager.h"

namespace handheld {
MaintenanceResult performMaintenance(MaintenanceBarrier& barrier, const Request& request,
        ProtocolBackend& backend, UserConfig& config, IdentityManager& identities,
        FlashStore& flash, SDStore& sd, AnnounceManager* nodes) {
    const MaintenanceBarrier::Token token{request.id, request.generation};
    if (!barrier.claimOperation(token)) return {};
    if (config.settingsPending() || config.recoveryRequired()) {
        barrier.complete(token, false);
        return {false, "Settings recovery still pending; restart required"};
    }
    const bool protocolFlushed = backend.persistData();
    const bool contactsFlushed = !nodes || nodes->flushPending();
    const bool identityFlushed = identities.flushPending();
    if (!protocolFlushed || !contactsFlushed || !identityFlushed) {
        barrier.complete(token, false);
        return {false, "Flush failed. Restart again to force; pending data may be lost."};
    }
    bool ok = true;
    const char* failure = "Operation failed; restart required";
    switch (request.operation) {
    case Operation::FormatSD:
        if (!sd.isReady()) { ok = false; failure = "SD unavailable; restart required"; break; }
        ok = sd.formatForRsDeck();
        failure = "SD initialization failed; restart required";
        break;
    case Operation::WipeSD: case Operation::ClearOldDataAndRestart:
        if (!sd.isReady()) { ok = false; failure = "SD unavailable; restart required"; break; }
        ok = sd.wipeRsDeck();
        failure = "SD erase failed; restart required";
        if (ok && nodes && request.operation == Operation::ClearOldDataAndRestart) nodes->clearAll();
        break;
    case Operation::FactoryReset: {
        const auto reset = performFactoryReset(flash, sd);
        barrier.complete(token, reset.ok());
        return {reset.ok(), reset.detail()};
    }
    case Operation::EnableSDAndRestart: {
        UserConfig candidate;
        if (!candidate.tryAssign(config)) { ok = false; failure = "Settings memory unavailable; restart required"; break; }
        candidate.settings().sdStorageEnabled = true;
        failure = "SD setting save failed; restart required";
        ok = candidate.save(sd, flash); if (ok) config.swap(candidate); break;
    }
    case Operation::SwitchIdentity: {
        int index = -1;
        const auto& entries = identities.identities();
        for (size_t i = 0; i < entries.size(); ++i) if (entries[i].hash == request.peer) index = i;
        ok = index >= 0 && identities.switchTo(index);
        if (ok) {
            ok = SettingsTransaction::recover(config, identities, sd, flash).complete();
        }
        break;
    }
    case Operation::Restart: case Operation::PowerOff: break;
    default: ok = false; break;
    }
    barrier.complete(token, ok);
    return {ok, ok ? "Ready to restart" : failure};
}
} // namespace handheld
