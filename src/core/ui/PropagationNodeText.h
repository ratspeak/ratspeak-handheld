#pragma once
#include "config/PropagationSettings.h"
#include <cstdio>

namespace handheld::ui {
// Selected describes the AUTO candidate, not a successful connection. An
// unavailable candidate remains visibly unavailable in the same snapshot.
inline void propagationNodeTitle(const propagation::NodeView& node, bool automatic,
                                 char* out, size_t capacity) {
    std::snprintf(out, capacity, "%s%s%s: %s", automatic && node.active ? "Selected " : "",
        node.interface == UINT8_MAX ? "Unknown route" : node.interface ? "WiFi/TCP" : "LoRa",
        node.usable ? "" : " unavailable", node.name[0] ? node.name : "Node");
}
}
