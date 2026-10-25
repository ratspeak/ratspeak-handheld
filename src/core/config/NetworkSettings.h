#pragma once
#include "UserConfig.h"

namespace handheld::network_settings {
enum Change : uint8_t { None = 0, Tcp = 1, AccessPoint = 2 };
// Compare the candidate before committing, without copying its String/vector
// storage. Only a completed transaction may publish these owner notifications.
inline uint8_t changes(const UserSettings& before, const UserSettings& after) {
    uint8_t changed = None;
    if (before.wifiAPSSID != after.wifiAPSSID || before.wifiAPPassword != after.wifiAPPassword)
        changed |= AccessPoint;
    if (before.tcpConnections.size() != after.tcpConnections.size()) changed |= Tcp;
    else for (size_t i = 0; i < before.tcpConnections.size(); ++i) {
        const auto& old = before.tcpConnections[i];
        const auto& next = after.tcpConnections[i];
        if (old.host != next.host || old.port != next.port || old.autoConnect != next.autoConnect) {
            changed |= Tcp;
            break;
        }
    }
    return changed;
}
} // namespace handheld::network_settings
