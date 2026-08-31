#include "AudioNotify.h"

void AudioNotify::begin() {
    M5.Speaker.begin();
    _begun = true;
    M5.Speaker.setVolume(_volume * 255 / 100);
}

void AudioNotify::loop() {
    if (!_enabled || !_noteCount) return;

    unsigned long now = millis();
    if ((long)(now - _nextAt) < 0) return;

    if (_playingTone) {
        M5.Speaker.stop();
        _playingTone = false;

        uint16_t gapMs = _notes[_noteIndex].gapMs;
        _noteIndex++;
        if (_noteIndex >= _noteCount) {
            _noteCount = 0;
            _inGap = false;
            return;
        }
        if (gapMs > 0) {
            _inGap = true;
            _nextAt = now + gapMs;
            return;
        }
    }

    if (_inGap) {
        _inGap = false;
    }

    startCurrent(now);
}

void AudioNotify::tone(uint16_t freq, uint16_t durationMs) {
    if (!_enabled) return;
    Note note{freq, durationMs, 0};
    playSequence(&note, 1);
}

void AudioNotify::playSequence(const Note* notes, uint8_t count) {
    if (!_enabled || !notes || count == 0) return;
    if (count > MAX_NOTES) count = MAX_NOTES;

    M5.Speaker.stop();
    for (uint8_t i = 0; i < count; i++) {
        _notes[i] = notes[i];
    }
    _noteCount = count;
    _noteIndex = 0;
    _playingTone = false;
    _inGap = false;
    startCurrent(millis());
}

void AudioNotify::startCurrent(unsigned long now) {
    if (!_enabled || !_begun || _noteIndex >= _noteCount) {
        _noteCount = 0;
        return;
    }
    M5.Speaker.setVolume(_volume * 255 / 100);
    M5.Speaker.tone(_notes[_noteIndex].freq, _notes[_noteIndex].durationMs);
    _playingTone = true;
    _nextAt = now + _notes[_noteIndex].durationMs;
}

void AudioNotify::stop() {
    if (_begun) {
        M5.Speaker.stop();
    }
    _noteCount = 0;
    _noteIndex = 0;
    _playingTone = false;
    _inGap = false;
}

void AudioNotify::setEnabled(bool enabled) {
    _enabled = enabled;
    if (!_enabled) stop();
}

void AudioNotify::setVolume(uint8_t vol) {
    _volume = vol;
    if (_begun) {
        M5.Speaker.setVolume(_volume * 255 / 100);
    }
}

void AudioNotify::playMessage() {
    if (!_enabled) return;
    const Note notes[] = {
        {1000, 50, 50},
        {1000, 50, 0},
    };
    playSequence(notes, 2);
}

void AudioNotify::playAnnounce() {
    if (!_enabled) return;
    tone(800, 20);
}

void AudioNotify::playError() {
    if (!_enabled) return;
    const Note notes[] = {
        {400, 100, 50},
        {400, 100, 50},
        {400, 100, 0},
    };
    playSequence(notes, 3);
}

void AudioNotify::playBoot() {
    if (!_enabled) return;
    const Note notes[] = {
        {440, 60, 30},   // A4
        {554, 60, 30},   // C#5
        {659, 60, 30},   // E5
        {880, 90, 0},    // A5
    };
    playSequence(notes, 4);
}
