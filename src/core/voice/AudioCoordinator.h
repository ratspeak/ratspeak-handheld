#pragma once
#include <atomic>

namespace handheld::voice {
// UI notification owner and audio worker transfer hardware ownership explicitly.
// Only UI runs suspend/restore; only worker runs voice DMA/codec operations.
class AudioCoordinator {
public:
    enum State : unsigned { Notify, Requested, Yielding, Available, Voice, Returning, Restoring };
    static AudioCoordinator& instance();
    bool request() { unsigned expected=Notify; return _state.compare_exchange_strong(expected,Requested); }
    bool claim() { unsigned expected=Available; return _state.compare_exchange_strong(expected,Voice); }
    bool release() {
        unsigned state=_state.load();
        if (state==Notify || state==Returning || state==Restoring) return true;
        if (state==Yielding) return false;
        return _state.compare_exchange_strong(state,state==Requested ? Notify : Returning);
    }
    bool notifications() const { return _state.load()==Notify; }
    bool held() const { return !notifications(); }
    // Called from AudioNotify::loop on its UI owner, including while quiescing.
    template<class Suspend, class Restore> bool poll(Suspend suspend, Restore restore) {
        unsigned expected=Requested;
        if (_state.compare_exchange_strong(expected,Yielding)) {
            suspend(); _state.store(Available); return false;
        }
        expected=Returning;
        if (_state.compare_exchange_strong(expected,Restoring)) { restore(); _state.store(Notify); }
        return notifications();
    }
private:
    std::atomic<unsigned> _state{Notify};
};
}
