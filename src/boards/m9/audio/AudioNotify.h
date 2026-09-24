#pragma once
#include <Arduino.h>
#include <atomic>

class AudioNotify {
public:
    void begin();
    void end();
    void playMessage() { requestMessage(); }
    void playAnnounce() { requestMessage(); }
    void playError() { requestMessage(); }
    void playBoot();
    void requestMessage() { _pending.store(true); }
    void loop();
    void setEnabled(bool enabled) { _enabled = enabled; if (!enabled) end(); }
    bool isEnabled() const { return _enabled; }
    void setVolume(uint8_t volume) { _volume = volume > 100 ? 100 : volume; }
    uint8_t volume() const { return _volume; }
private:
    std::atomic<bool> _pending{false};
    enum class Sound : uint8_t { None, Message, BootPending, Boot };
    Sound _sound = Sound::None;
    bool _enabled = true, _ready = false;
    uint8_t _volume = 80;
    uint32_t _started = 0;
    uint16_t _frequency = 0;
    uint8_t _duty = 0;
    void tone(uint16_t frequency);
};
