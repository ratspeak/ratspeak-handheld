#pragma once
#include <atomic>

namespace handheld::voice {
// UI notification owner and audio worker transfer hardware ownership explicitly.
// Only UI runs suspend/restore; only worker runs voice DMA/codec operations.
class AudioCoordinator {
public:
    enum State : unsigned { Notify, Requested, Yielding, Available, Voice, Returning, Restoring };
    static AudioCoordinator& instance();
    bool request(bool playbackOnly=false) {
        unsigned expected=Notify;
        return _state.compare_exchange_strong(expected,Requested | (playbackOnly?0:Capture));
    }
    bool claim() {
        unsigned expected=_state.load();
        return (expected & Mask)==Available && _state.compare_exchange_strong(expected,Voice | (expected & Capture));
    }
    bool release() {
        unsigned state=_state.load();
        const auto phase=state & Mask;
        if (phase==Notify || phase==Returning || phase==Restoring) return true;
        if (phase==Yielding) return false;
        return _state.compare_exchange_strong(state,phase==Requested ? unsigned(Notify) : Returning | (state & Capture));
    }
    bool notifications() const { return _state.load()==Notify; }
    bool held() const { return !notifications(); }
    // T-Deck center-button GPIO is unsafe during microphone ownership, but
    // speaker-only clips must retain their physical Pause control.
    bool blocksPointerClick() const { return (_state.load() & Capture)!=0; }
    // Called from AudioNotify::loop on its UI owner, including while quiescing.
    template<class Suspend, class Restore> bool poll(Suspend suspend, Restore restore) {
        unsigned expected=_state.load();
        if ((expected & Mask)==Requested && _state.compare_exchange_strong(expected,Yielding | (expected & Capture))) {
            suspend(); _state.store(Available | (expected & Capture)); return false;
        }
        expected=_state.load();
        if ((expected & Mask)==Returning && _state.compare_exchange_strong(expected,Restoring | (expected & Capture))) { restore(); _state.store(Notify); }
        return notifications();
    }
private:
    static constexpr unsigned Capture=8, Mask=7;
    std::atomic<unsigned> _state{Notify};
};
}
