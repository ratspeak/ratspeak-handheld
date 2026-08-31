#pragma once

#include <Arduino.h>
#include <M5Unified.h>

class AudioNotify {
public:
    void begin();
    void loop();

    // Notification sounds
    void playMessage();     // Double-beep: 50ms 1kHz × 2
    void playAnnounce();    // Single tick: 20ms 800Hz
    void playError();       // Triple-beep: 100ms 400Hz × 3
    void playBoot();        // Ascending chime: A4→C#5→E5→A5

    // Settings
    void setEnabled(bool enabled);
    bool isEnabled() const { return _enabled; }
    void setVolume(uint8_t vol);
    uint8_t volume() const { return _volume; }

private:
    struct Note {
        uint16_t freq;
        uint16_t durationMs;
        uint16_t gapMs;
    };

    static constexpr uint8_t MAX_NOTES = 8;

    void tone(uint16_t freq, uint16_t durationMs);
    void playSequence(const Note* notes, uint8_t count);
    void startCurrent(unsigned long now);
    void stop();

    bool _enabled = true;
    uint8_t _volume = 80;  // 0-100
    Note _notes[MAX_NOTES] = {};
    uint8_t _noteCount = 0;
    uint8_t _noteIndex = 0;
    unsigned long _nextAt = 0;
    bool _playingTone = false;
    bool _inGap = false;
    bool _begun = false;
};
