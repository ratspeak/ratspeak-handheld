#pragma once
#include <atomic>
#include <cstdint>

namespace handheld::storage {
// Flash cache suspension can starve audio DMA even when storage runs on the
// other CPU. A recording requests a quiet interval, then waits for admitted
// transactions to finish before starting the microphone. No filesystem mutex
// is held by the audio task. Nested leases belong to their outer transaction.
class CaptureQuiet {
public:
    static bool enter() {
        auto state=_state.load();
        do {
            if(state&Requested || state==Requested-1) return false;
        } while(!_state.compare_exchange_weak(state,state+1));
        return true;
    }
    static void leave() { _state.fetch_sub(1); }
    static bool request() {
        auto state=_state.load();
        do {
            if(state&Requested) return false;
        } while(!_state.compare_exchange_weak(state,state|Requested));
        return true;
    }
    static bool ready() { return _state.load()==Requested; }
    static void resume() { _state.fetch_and(~Requested); }
private:
    static constexpr uint32_t Requested=uint32_t(1)<<31;
    inline static std::atomic<uint32_t> _state{0};
};
}
