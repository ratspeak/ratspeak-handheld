#include "audio/AudioNotify.h"
#include "config/BoardConfig.h"

namespace {
// Channel 4 has its own timer, separate from LCD 0 and keyboard backlight 2.
constexpr uint8_t buzzerChannel = 4;
struct Note { uint16_t from, to, duration; };
// The existing handheld startup motif, rendered by a passive PWM buzzer.
constexpr Note bootNotes[] = {
    {300, 1200, 160}, {0, 0, 25},
    {659, 659, 45}, {0, 0, 12}, {831, 831, 45}, {0, 0, 12},
    {988, 988, 45}, {0, 0, 25}, {2400, 1600, 60}, {0, 0, 20},
    {1319, 1319, 100},
};
}

void AudioNotify::begin() {
    ledcSetup(buzzerChannel, 2200, 8);
    ledcAttachPin(BUZZER_PIN, buzzerChannel);
    ledcWrite(buzzerChannel, 0);
    _ready = true;
    end();
}

void AudioNotify::tone(uint16_t frequency) {
    if (!_ready) return;
    const uint8_t duty = frequency ? uint32_t(_volume) * 127 / 100 : 0;
    if (frequency && frequency != _frequency) {
        ledcSetup(buzzerChannel, frequency, 8);
        // A timer reconfiguration may change duty; always write it below.
        ledcWrite(buzzerChannel, duty);
    } else if (duty != _duty) {
        ledcWrite(buzzerChannel, duty);
    }
    _frequency = frequency;
    _duty = duty;
}

void AudioNotify::end() {
    tone(0);
    _sound = Sound::None;
    _pending.store(false);
}

void AudioNotify::playBoot() {
    if (!_ready || !_enabled || !_volume) return;
    _sound = Sound::BootPending;
    // Start on the next UI loop so remaining setup cannot stretch a note.
}

void AudioNotify::loop() {
    if (!_ready) return;
    if (!_enabled || !_volume) { end(); return; }
    const uint32_t now = millis();
    if (_sound == Sound::BootPending) {
        _started = now;
        _sound = Sound::Boot;
    }
    if (_sound == Sound::Boot) {
        uint32_t elapsed = now - _started;
        for (const auto& note : bootNotes) {
            if (elapsed < note.duration) {
                const int32_t delta = int32_t(note.to) - note.from;
                tone(note.from + delta * int32_t(elapsed) / note.duration);
                return;
            }
            elapsed -= note.duration;
        }
        tone(0);
        _sound = Sound::None;
    } else if (_sound == Sound::Message && uint32_t(now - _started) >= 80) {
        tone(0);
        _sound = Sound::None;
    }
    // Coalesce notifications and wait for the current sound to finish.
    if (_sound == Sound::None && _pending.exchange(false)) {
        _started = now;
        _sound = Sound::Message;
    }
    if (_sound == Sound::Message) tone(2200);
}
