#pragma once
#include "voice/VoiceWorker.h"

namespace handheld::voice {
// Physical edges belong to UI, independently of the ordinary key mailbox.
// Opening a panel never replays a held key. Release/cancel retires queued audio
// immediately, even when the Service mailbox is full.
class VoiceInput {
public:
    enum Source : uint8_t { Key, Touch };
    static VoiceInput& instance() { static VoiceInput value;return value; }
    void open(uint32_t generation) {
        if(_enabled && _generation==generation) return;
        cancel();
        if(_view==UINT32_MAX) return;
        ++_view;_generation=generation;_enabled=true;
    }
    void cancel() {
        _enabled=false;_pending=false;VoiceWorker::emergencyStop();
    }
    void edge(Source source,bool down) {
        const auto index=unsigned(source);
        if(index>1 || _held[index]==down) return;
        const bool already=_held[0] || _held[1];_held[index]=down;
        if(!down) {_pending=false;VoiceWorker::emergencyStop();}
        else if(_enabled && !already) {_pending=true;_epoch=VoiceWorker::stopEpoch();}
    }
    void poll() { if(_enabled && (_held[0] || _held[1])) VoiceWorker::keepInputAlive(); }
    bool take(Command& out) {
        if(!_enabled || !_pending || _epoch!=VoiceWorker::stopEpoch()) return false;
        _pending=false;out={};out.action=Action::TalkDown;out.generation=_generation;out.view=_view;out.stopEpoch=_epoch;return true;
    }
    bool enabled() const {return _enabled;}
private:
    uint32_t _view=0,_generation=0,_epoch=0;
    bool _enabled=false,_pending=false,_held[2]{};
};
}
