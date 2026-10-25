#pragma once
#include "config/UserConfig.h"
#include "reticulum/IdentityManager.h"

// One durable roll-forward intent lives in the authoritative config record.
// This stateless owner retains no candidate, strings, callback or retry queue.
class SettingsTransaction {
public:
    enum class State : uint8_t { Complete, Pending, Invalid, RecoveryRequired };
    struct Result {
        State state;
        const char* detail;
        bool complete() const { return state == State::Complete; }
    };
    // candidate may be a UI value snapshot; its protected metadata is ignored.
    // Only Complete may publish effective settings, hardware or onboarding.
    // Complete consumes candidate: it is swapped in and receives the old value.
    static Result apply(UserConfig& effective, UserConfig& candidate,
        IdentityManager& identities, SDStore& sd, FlashStore& flash);
    // Same contract for fixed-size values: strings and lists stay canonical
    // and nothing is copied unless the name binding needs reconciliation.
    static Result applyScalars(UserConfig& effective, UserScalars scalars,
        IdentityManager& identities, SDStore& sd, FlashStore& flash);
    // Also reconciles boot/switch name mirrors from the validated active slot.
    // A pending intent must match that exact active identity; never retarget it.
    static Result recover(UserConfig& effective, IdentityManager& identities,
        SDStore& sd, FlashStore& flash);
    static constexpr size_t PersistedLimit = UserConfig::SnapshotLimit;
private:
    static Result settle(UserConfig& effective, UserConfig& candidate,
        IdentityManager& identities, SDStore& sd, FlashStore& flash);
    static bool bind(UserConfig& config, const std::string& hash);
    static bool matches(const UserConfig& config, const std::string& hash);
};
