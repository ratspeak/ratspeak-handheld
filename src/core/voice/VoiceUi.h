#pragma once
#include "voice/VoiceInput.h"
#include <cstring>

namespace handheld::voice {
// Native frontends share the same session/view fencing and microphone policy.
// Rendering and command transport stay with each frontend's existing owner.
class VoiceUi {
public:
    using Submit=bool(*)(void*,const Command&);
    void begin(void* context,Submit submit) {_context=context;_submit=submit;}
    void update(const Status& value,bool foregroundAllowed) {
        const bool changed=value.generation!=_status.generation;
        if(changed) {_error=Code::Ok;_pending=false;VoiceInput::instance().cancel();}
        _status=value;
        if(_status.phase==Phase::Incoming && _status.verified && _offered!=_status.generation) {
            _offered=_status.generation;_incoming=true;show();
        }
        const bool ready=_status.phase==Phase::Ready || _status.phase==Phase::Receiving || _status.phase==Phase::Talking;
        if(_visible && foregroundAllowed && ready && (_status.capabilities&1)) {
            auto& input=VoiceInput::instance();input.open(_status.generation);input.poll();
            Command command;if(input.take(command) && !submit(command)) input.cancel();
        } else if(VoiceInput::instance().enabled()) VoiceInput::instance().cancel();
    }
    bool start(const uint8_t peer[16]) {
        if(active(_status.phase)) {show();return false;}
        Command command;command.action=Action::Start;std::memcpy(command.peer,peer,16);
        show();_pending=true;
        if(!submit(command)) {failed(Code::Busy);return false;}
        return true;
    }
    void show() {_visible=true;_error=Code::Ok;VoiceInput::instance().cancel();}
    void hide() {_visible=false;VoiceInput::instance().cancel();}
    bool takeIncoming() {const bool value=_incoming;_incoming=false;return value;}
    bool visible() const {return _visible;}
    const Status& status() const {return _status;}
    bool pending() const {return _pending;}
    const char* text() const {
        if(_error!=Code::Ok) {Status failure;failure.phase=Phase::Ended;failure.reason=_error;return description(failure);}
        return _pending?"Requesting voice...":description(_status);
    }
    void failed(Code code) {_error=code;_pending=false;VoiceInput::instance().cancel();}
    void action(Action action) {
        if(action==Action::End || action==Action::Decline) VoiceInput::instance().cancel();
        Command command;command.generation=_status.generation;command.action=action;
        if(!submit(command)) failed(Code::Busy);
    }
    void volume(int delta) {
        const int value=int(_status.volume)+delta;
        Command command;command.generation=_status.generation;command.action=Action::Volume;
        command.volume=uint8_t(value<0?0:value>100?100:value);
        if(submit(command)) _status.volume=command.volume;
    }
private:
    bool submit(const Command& c) {return _submit && _submit(_context,c);}
    void* _context=nullptr;Submit _submit=nullptr;
    Status _status;
    uint32_t _offered=0;
    Code _error=Code::Ok;
    bool _visible=false,_pending=false,_incoming=false;
};
}
