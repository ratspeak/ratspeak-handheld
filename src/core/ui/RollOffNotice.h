#pragma once

#include <Preferences.h>
#include "config/Config.h"
#include "ui/StorageHistoryText.h"

// The roll-off notice shows once per device; it counts as seen once dismissed.
namespace handheld::ui::storage_history {
inline bool rollOffSeen() {
    Preferences marker;
    if (!marker.begin(NVS_NS_MSG, true)) return false;
    const bool seen = marker.getBool("rolloff_seen", false);
    marker.end(); return seen;
}
inline void markRollOffSeen() {
    Preferences marker;
    if (marker.begin(NVS_NS_MSG, false)) { marker.putBool("rolloff_seen", true); marker.end(); }
}
}
