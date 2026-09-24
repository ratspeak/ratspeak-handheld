#include "audio/AudioNotify.h"
#include "config/BoardConfig.h"
void AudioNotify::begin() { ledcSetup(4, 2200, 8); ledcAttachPin(BUZZER_PIN, 4); ledcWrite(4, 0); }
void AudioNotify::end() { ledcWrite(4, 0); _sounding = false; _pending.store(false); }
void AudioNotify::loop() {
    if (!_enabled) { end(); return; }
    if (_sounding && uint32_t(millis() - _started) >= 80) { ledcWrite(4, 0); _sounding = false; }
    if (_pending.exchange(false) && _volume) {
        ledcWrite(4, uint32_t(_volume) * 127 / 100);
        _started = millis(); _sounding = true;
    }
}
