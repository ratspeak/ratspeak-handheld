#include "audio/AudioNotify.h"
#include "config/BoardConfig.h"

namespace {
// Channel 4 has its own timer, separate from LCD 0 and keyboard backlight 2.
constexpr uint8_t buzzerChannel = 4;
struct Note { uint16_t frequency, duration; };
// Keep the ascending E-major motif, without speaker-style sweeps. Steady
// pitches avoid audible timer steps when UI polling is uneven.
constexpr Note bootNotes[] = {
    {659, 90}, {0, 25}, {831, 90}, {0, 25}, {988, 120},
};

uint8_t noteLevel(uint32_t elapsed, uint32_t duration) {
    constexpr uint32_t fadeMs = 15;
    if (elapsed >= duration) return 0;
    const uint32_t remaining = duration - elapsed;
    if (elapsed < fadeMs) return elapsed * 100 / fadeMs;
    if (remaining < fadeMs) return remaining * 100 / fadeMs;
    return 100;
}
}

void AudioNotify::begin() {
    ledcSetup(buzzerChannel, 2200, 8);
    ledcAttachPin(BUZZER_PIN, buzzerChannel);
    ledcWrite(buzzerChannel, 0);
    _ready = true;
    end();
}

void AudioNotify::tone(uint16_t frequency, uint8_t level) {
    if (!_ready) return;
    const uint8_t duty = frequency ? uint32_t(_volume) * level * 127 / 10000 : 0;
    if (frequency && frequency != _frequency) {
        // A late UI poll can skip a gap; never retune a sounding timer.
        if (_duty) ledcWrite(buzzerChannel, 0);
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
                tone(note.frequency, noteLevel(elapsed, note.duration));
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
    if (_sound == Sound::Message) tone(2200, noteLevel(now - _started, 80));
}
