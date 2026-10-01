#pragma once
#include <cstddef>
#include <cstdint>
#include "voice/PcmRate.h"
namespace handheld::voice {
// The single voice worker owns all calls. read/write operate in <=20ms chunks.
// Board drivers must leave the microphone stopped after prepare().
class AudioDevice {
public:
    bool prepare(uint8_t volume);
    bool capture(bool enabled);
    size_t read(int16_t* pcm, size_t samples);
    size_t write(const int16_t* pcm, size_t samples);
    void volume(uint8_t value) { _volume=value; }
    void end();
    void flush();
private:
    bool _ready=false, _capture=false, _rx=false, _tx=false;
    uint8_t _volume=70;
    int16_t _io[640]{};
    PcmRate _rate;
};
}
