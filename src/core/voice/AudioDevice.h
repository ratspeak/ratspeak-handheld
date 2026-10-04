#pragma once
#include <cstddef>
#include <cstdint>
#include "voice/PcmRate.h"
#include "voice/VoiceTypes.h"
namespace handheld::voice {
// The single voice worker owns all calls. PCM is consumed in 20 ms chunks;
// capture may wait for a complete 40 ms DMA buffer. prepare() defaults to playback;
// only an explicitly requested recording starts the microphone.
class AudioDevice {
public:
    bool prepare(uint8_t volume, bool record = false);
    bool capture(bool enabled);
    size_t read(int16_t* pcm, size_t samples);
    size_t write(const int16_t* pcm, size_t samples);
    void volume(uint8_t value) { _volume=value; }
    void end();
    void flush();
    Code error() const { return _error; }
private:
    bool _ready=false, _capture=false, _rx=false, _tx=false;
    uint8_t _volume=70;
#ifdef RSCARDPUTER
    // Two 40 ms vendor recording buffers. Requeue the consumed buffer before
    // encoding so capture never waits for the codec or resets between chunks.
    int16_t _io[1280]{};
    uint8_t _readBuffer=0, _readHalf=0;
#else
    int16_t _io[640]{};
#endif
    void* _events=nullptr;
    Code _error=Code::AudioUnavailable;
    bool inputHealthy();
    PcmRate _rate;
};
}
