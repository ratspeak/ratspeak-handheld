#pragma once

// RRC remains implemented while its handheld release qualification is pending.
// Enable explicitly for RRC testing; normal builds expose Direct chats only.
#ifndef HANDHELD_RRC_UI_ENABLED
#define HANDHELD_RRC_UI_ENABLED 0
#endif
static_assert(HANDHELD_RRC_UI_ENABLED == 0 || HANDHELD_RRC_UI_ENABLED == 1,
              "RRC UI availability must be 0 or 1");

namespace handheld::ui {
inline constexpr bool RrcEnabled = HANDHELD_RRC_UI_ENABLED != 0;
}
