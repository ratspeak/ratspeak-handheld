#pragma once
#include "voice/VoiceTypes.h"
#include <atomic>
namespace handheld::voice {
// One worker per device, created only after explicit call acceptance. All PCM,
// codec state, I2S waits and teardown belong to this worker, never Service/UI.
class VoiceWorker final : public AudioPort {
public:
    uint32_t cancellationEpoch() const override { return stopEpoch(); }
#ifdef ARDUINO
    static void keepInputAlive();
    uint8_t capabilities() const override;
    Code prepare(uint32_t generation, uint8_t profile, uint8_t volume) override;
    void stop() override;
    bool drained() override;
    AudioStatus status() const override;
    void capture(bool, uint32_t stopEpoch = 0) override;
    void flush() override;
    void volume(uint8_t) override;
    bool receive(const Encoded&) override;
    bool take(Encoded&) override;
#else
    // SDK seam for host protocol/UI tests. Hardware qualification uses the
    // actual Arduino implementation and separate audio-worker concurrency tests.
    static void keepInputAlive() {}
    uint8_t capabilities() const override { return 0; }
    Code prepare(uint32_t,uint8_t,uint8_t) override { return Code::AudioUnavailable; }
    void stop() override {}
    bool drained() override { return true; }
    AudioStatus status() const override { return {}; }
    void capture(bool,uint32_t=0) override {}
    void flush() override {}
    void volume(uint8_t) override {}
    bool receive(const Encoded&) override { return false; }
    bool take(Encoded&) override { return false; }
#endif
    // The UI calls this before queueing TalkUp/End. Stops capture independently
    // of a full command mailbox; it never starts or re-arms capture.
    static void emergencyStop() {
        auto value=_stopEpoch.load();
        while(value!=UINT32_MAX && !_stopEpoch.compare_exchange_weak(value,value+1)) {}
    }
    static uint32_t stopEpoch() { return _stopEpoch.load(); }
private:
    struct Impl;
    Impl* _impl = nullptr;
    inline static std::atomic<uint32_t> _stopEpoch{1}, _inputHeartbeat{0};
    static void run(void*);
};
}
