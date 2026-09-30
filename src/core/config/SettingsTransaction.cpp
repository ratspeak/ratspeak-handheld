#include "SettingsTransaction.h"
#include "util/DisplayName.h"
#include "runtime/TaskOwner.h"
#include <cstring>

namespace {
using State = SettingsTransaction::State;
constexpr SettingsTransaction::Result Complete{State::Complete, "Settings saved"};
constexpr SettingsTransaction::Result Pending{State::Pending, "Settings committed; recovery pending"};
constexpr SettingsTransaction::Result Recovery{State::RecoveryRequired, "Settings recovery required"};
constexpr SettingsTransaction::Result NoMemory{State::Invalid, "Settings memory unavailable; retry"};

bool sameBytes(const String& left, const String& right) {
    return left.length() == right.length() && (!left.length() ||
        memcmp(left.c_str(), right.c_str(), left.length()) == 0);
}

bool fitsWiFiString(const String& value, size_t maximum) {
    // Empty remains the AP auto-name or an unused STA slot. The SDK receives
    // C strings, so an embedded NUL cannot represent the saved SSID faithfully.
    return value.length() <= maximum && (!value.length() || !memchr(value.c_str(), 0, value.length()));
}

bool validAPPassword(const String& value) {
    const size_t length = value.length();
    if (!length) return true; // Open network.
    if (length < 8 || !fitsWiFiString(value, 64)) return false;
    if (length < 64) return true;
    // Pinned ESP32 SDK: 64 bytes select hexstr2bin(..., 32), not the
    // passphrase derivation used for 8..63 bytes by our WPA2 access point.
    for (size_t i = 0; i < length; ++i) {
        const char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

const char* changedCredentialError(const UserSettings& saved, const UserSettings& edited) {
    if (!sameBytes(saved.wifiAPSSID, edited.wifiAPSSID) && !fitsWiFiString(edited.wifiAPSSID, 32))
        return "AP SSID: 0-32 bytes; no NUL";
    if (!sameBytes(saved.wifiAPPassword, edited.wifiAPPassword) && !validAPPassword(edited.wifiAPPassword))
        return "AP key: empty/8-63 bytes/64 hex";
    for (const auto& network : edited.wifiSTANetworks) {
        bool knownSSID = false, unchanged = false;
        // Preserve existing legacy values on unrelated edits and when another
        // slot is removed/reordered; vector position is not network identity.
        for (const auto& old : saved.wifiSTANetworks) {
            if (!sameBytes(old.ssid, network.ssid)) continue;
            knownSSID = true;
            unchanged = unchanged || sameBytes(old.password, network.password);
        }
        if (!knownSSID && !fitsWiFiString(network.ssid, 32)) return "STA SSID: 0-32 bytes; no NUL";
        // STA negotiates authentication, including SAE with a password rather
        // than a raw WPA2 PSK. No auth mode is stored here: enforce only the
        // shared SDK field/C-string bound, leaving auth-specific checks to it.
        if (!unchanged && !fitsWiFiString(network.password, 64)) return "STA key: 0-64 bytes; no NUL";
    }
    return nullptr;
}
}

bool SettingsTransaction::bind(UserConfig& config, const std::string& hash) {
    if (hash.size() != 32) return false;
    memset(config._nameIdentity, 0, sizeof config._nameIdentity);
    for (size_t i = 0; i < hash.size(); ++i) {
        const char c = hash[i];
        const int n = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (n < 0) return false;
        config._nameIdentity[i / 2] |= uint8_t(n << (i % 2 ? 0 : 4));
    }
    config._nameBound = true;
    return true;
}

bool SettingsTransaction::matches(const UserConfig& config, const std::string& hash) {
    if (!config._nameBound || hash.size() != 32) return false;
    static constexpr char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 16; ++i)
        if (hash[2*i] != digits[config._nameIdentity[i] >> 4] || hash[2*i+1] != digits[config._nameIdentity[i] & 15]) return false;
    return true;
}

SettingsTransaction::Result SettingsTransaction::apply(UserConfig& effective, UserConfig& candidate,
        IdentityManager& identities, SDStore& sd, FlashStore& flash) {
    handheld::assertDeviceOwner();
    if (&effective == &candidate) return {State::Invalid, "Settings candidate must be separate"};
    if (effective._recoveryRequired) return Recovery;
    if (effective._namePending) return Pending;
    if (!candidate.settings().propagation.valid())
        return {State::Invalid, "Invalid propagation settings"};
    if (const char* error = changedCredentialError(effective.settings(), candidate.settings()))
        return {State::Invalid, error};
    const int index = identities.activeIndex();
    if (!identities.validateSlot(index)) return Recovery;
    const auto& slot = identities.identities()[index];
    if (candidate.settings().displayName != slot.displayName &&
        !handheld::validNewDisplayName(candidate.settings().displayName.c_str(), candidate.settings().displayName.length()))
        return {State::Invalid, "Name must contain at most 16 valid characters"};
    candidate._recoveryRequired = false;
    if (!bind(candidate, slot.hash)) return Recovery;
    // Completion cannot be undone by a stale UI value snapshot. New empty
    // completion is explicit; nonempty input completes the optional name step.
    candidate.settings().nameComplete = candidate.settings().nameComplete || slot.nameComplete || !candidate.settings().displayName.isEmpty();
    candidate._namePending = candidate.settings().displayName != slot.displayName ||
        candidate.settings().nameComplete != slot.nameComplete;
    UserConfig publication;
    {
        bool unavailable = false;
        const String json = candidate.serializeToJson(true, PersistedLimit, &unavailable);
        if (unavailable) return NoMemory;
        if (json.isEmpty() || json.length() > PersistedLimit)
            return {State::Invalid, "Settings exceed saved record capacity"};
        if (!publication.tryAssign(candidate)) return NoMemory;
        if (!candidate.saveRequired(flash, json)) return {State::Invalid, "Save failed; settings unchanged"};
    }
    if (candidate._namePending) {
        // Do not replace effective values until both required records agree.
        effective._namePending = true;
        memcpy(effective._nameIdentity, candidate._nameIdentity, sizeof effective._nameIdentity);
        effective._nameBound = true;
        return settle(effective, candidate, publication, identities, sd, flash);
    }
    publication.copyStateFrom(candidate);
    effective.swap(publication);
    effective._mirrorPending = effective.settings().sdStorageEnabled;
    effective.flushPending(sd, flash);
    return Complete;
}

SettingsTransaction::Result SettingsTransaction::settle(UserConfig& effective, UserConfig& candidate, UserConfig& publication,
        IdentityManager& identities, SDStore& sd, FlashStore& flash) {
    const int index = identities.activeIndex();
    if (!identities.validateSlot(index) || !matches(candidate, identities.identities()[index].hash)) return Recovery;
    const auto& slot = identities.identities()[index];
    if ((slot.displayName != candidate.settings().displayName || slot.nameComplete != candidate.settings().nameComplete) &&
        !identities.setDisplayName(index, candidate.settings().displayName, candidate.settings().nameComplete)) return Pending;
    candidate._namePending = false;
    if (!candidate.saveRequired(flash)) { candidate._namePending = true; return Pending; }
    publication.copyStateFrom(candidate);
    effective.swap(publication);
    effective._mirrorPending = effective.settings().sdStorageEnabled;
    effective.flushPending(sd, flash);
    return Complete;
}

SettingsTransaction::Result SettingsTransaction::recover(UserConfig& effective, IdentityManager& identities,
        SDStore& sd, FlashStore& flash) {
    handheld::assertDeviceOwner();
    if (effective._recoveryRequired) return Recovery;
    const int index = identities.activeIndex();
    if (!identities.validateSlot(index)) return Recovery;
    if (effective._namePending) {
        UserConfig candidate;
        if (!candidate.load(flash) || !candidate._namePending ||
            (candidate._source != UserConfig::Source::Flash && candidate._source != UserConfig::Source::FlashBackup)) return Recovery;
        UserConfig publication;
        if (!publication.tryAssign(candidate)) return Pending;
        return settle(effective, candidate, publication, identities, sd, flash);
    }
    const auto& slot = identities.identities()[index];
    if (matches(effective, slot.hash) && effective.settings().displayName == slot.displayName &&
        effective.settings().nameComplete == slot.nameComplete && effective._source == UserConfig::Source::Flash) return Complete;
    // Unbound global names cannot establish which identity they belong to.
    // Existing per-slot names (including legacy lengths and explicit empty)
    // remain canonical; no automatic migration writes a name into another slot.
    UserConfig candidate;
    if (!candidate.tryAssign(effective) || !UserConfig::trySetString(candidate.settings().displayName,
            slot.displayName.c_str(), slot.displayName.length())) return NoMemory;
    candidate.settings().nameComplete = slot.nameComplete;
    if (!bind(candidate, slot.hash)) return Recovery;
    candidate._namePending = false;
    UserConfig::sanitizeSettings(candidate._settings);
    UserConfig publication;
    if (!publication.tryAssign(candidate)) return NoMemory;
    // Legacy settings may exceed the new UI transfer limit: preserve them on
    // reconciliation rather than imposing a new limit on already stored data.
    if (!candidate.saveRequired(flash)) return {State::Invalid, "Name mirror save failed"};
    publication.copyStateFrom(candidate);
    effective.swap(publication);
    effective._mirrorPending = effective.settings().sdStorageEnabled;
    effective.flushPending(sd, flash);
    return Complete;
}
