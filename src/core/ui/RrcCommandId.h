#pragma once
#include <cstdint>

namespace handheld::ui {
// Cardputer's cooperative UI has one command namespace across its browser and
// editor. LVGL uses ServiceMailbox request IDs instead.
inline uint32_t nextCardRrcCommand() {
    static uint32_t next = 0;
    return next == UINT32_MAX ? 0 : ++next;
}
}
