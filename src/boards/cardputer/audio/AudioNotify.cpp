#include "AudioNotify.h"
#include "voice/AudioCoordinator.h"

namespace {
// 16 samples at 16 kHz: one millisecond of silence per repeat.
constexpr int8_t Silence[16] = {};
constexpr uint32_t SilenceRate = 16000;
}

void AudioNotify::begin() {
    M5.Speaker.begin();
    _begun = true;
    M5.Speaker.setVolume(_volume * 255 / 100);
    prime();
}

// The ADV codec clocks from BCLK; start it before the first audible note.
void AudioNotify::prime() {
    if (_enabled && _begun) M5.Speaker.playRaw(Silence, sizeof(Silence), SilenceRate, false, 1, 0, true);
}

void AudioNotify::loop() {
    handheld::voice::AudioCoordinator::instance().poll([this] { end(); }, [this] { begin(); });
}

bool AudioNotify::drain(uint32_t timeoutMs) {
    if (!_begun || !M5.Speaker.isPlaying()) return true;
    const uint32_t started = millis();
    while (M5.Speaker.isPlaying()) {
        if (millis() - started >= timeoutMs) return false;
        delay(5);
    }
    // The mixer finishes up to one DMA ring ahead of the speaker.
    const auto cfg = M5.Speaker.config();
    delay(cfg.dma_buf_count * cfg.dma_buf_len * 1000 / cfg.sample_rate + 5);
    return true;
}

void AudioNotify::tone(uint16_t freq, uint16_t durationMs) {
    if (!_enabled) return;
    Note note{freq, durationMs, 0};
    playSequence(&note, 1);
}

void AudioNotify::playSequence(const Note* notes, uint8_t count) {
    if (!_enabled || !_begun || !notes || count == 0) return;
    if (count > MAX_NOTES) count = MAX_NOTES;

    M5.Speaker.stop();
    M5.Speaker.setVolume(_volume * 255 / 100);
    // Each note waits out its offset in silence on its own channel, so the
    // speaker task keeps the rhythm however long the caller blocks.
    uint32_t at = 0;
    for (uint8_t i = 0; i < count; i++) {
        if (at) M5.Speaker.playRaw(Silence, sizeof(Silence), SilenceRate, false, at, i, true);
        M5.Speaker.tone(notes[i].freq, notes[i].durationMs, i, at == 0);
        at += notes[i].durationMs + notes[i].gapMs;
    }
}

void AudioNotify::stop() {
    if (_begun) {
        M5.Speaker.stop();
    }
}

void AudioNotify::setEnabled(bool enabled) {
    if (enabled == _enabled) return;
    _enabled = enabled;
    if (_enabled) prime();
    else stop();
}

void AudioNotify::setVolume(uint8_t vol) {
    _volume = vol;
    if (_begun) {
        M5.Speaker.setVolume(_volume * 255 / 100);
    }
}

void AudioNotify::playMessage() {
    if (!handheld::voice::AudioCoordinator::instance().notifications()) return;
    if (!_enabled) return;
    const Note notes[] = {
        {1000, 50, 50},
        {1000, 50, 0},
    };
    playSequence(notes, 2);
}

void AudioNotify::playAnnounce() {
    if (!handheld::voice::AudioCoordinator::instance().notifications()) return;
    if (!_enabled) return;
    tone(800, 20);
}

void AudioNotify::playError() {
    if (!handheld::voice::AudioCoordinator::instance().notifications()) return;
    if (!_enabled) return;
    const Note notes[] = {
        {400, 100, 50},
        {400, 100, 50},
        {400, 100, 0},
    };
    playSequence(notes, 3);
}

void AudioNotify::playBoot() {
    if (!handheld::voice::AudioCoordinator::instance().notifications()) return;
    if (!_enabled) return;
    const Note notes[] = {
        {440, 60, 30},   // A4
        {554, 60, 30},   // C#5
        {659, 60, 30},   // E5
        {880, 90, 0},    // A5
    };
    playSequence(notes, 4);
}

void AudioNotify::end() { stop(); M5.Speaker.end(); _begun=false; }
