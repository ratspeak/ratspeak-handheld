#pragma once
#include <stdint.h>
class AudioNotify {
public:
 void begin() {} void end() {} void loop() {}
 void playMessage() {} void playAnnounce() {} void playError() {} void playBoot() {}
 void requestMessage() {} void setEnabled(bool) {} void setVolume(uint8_t) {}
 bool isEnabled() const { return false; } uint8_t volume() const { return 0; }
};
