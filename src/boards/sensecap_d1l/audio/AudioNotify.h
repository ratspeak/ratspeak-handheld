#pragma once
#include <cstdint>

// No audio hardware on this board (HAS_AUDIO is false — see BoardConfig.h).
// No-op stub so the shared LvglApplication.cpp runtime's audio call sites
// link without a per-board #if HAS_AUDIO guard at every call site.
class AudioNotify {
public:
    void begin() {}
    void end() {}
    void playMessage() {}
    void playAnnounce() {}
    void playError() {}
    void playBoot() {}
    void requestMessage() {}
    void loop() {}
    void setEnabled(bool) {}
    bool isEnabled() const { return false; }
    void setVolume(uint8_t) {}
    uint8_t volume() const { return 0; }
};
