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
        if(changed) {
            _error=Code::Ok;_pending=false;VoiceInput::instance().cancel();
            std::memcpy(_peer,value.peer,sizeof _peer);
        }
        _status=value;
        if(_status.phase==Phase::Incoming && _status.verified && _offered!=_status.generation) {
            _offered=_status.generation;_incoming=true;show();
        }
        const bool ready=_status.phase==Phase::Ready || _status.phase==Phase::Receiving || _status.phase==Phase::Talking;
        if(_visible && foregroundAllowed && ready && _error==Code::Ok && (_status.capabilities&1)) {
            auto& input=VoiceInput::instance();input.open(_status.generation);input.poll();
            Command command;if(input.take(command) && !submit(command)) input.cancel();
        } else if(VoiceInput::instance().enabled()) VoiceInput::instance().cancel();
    }
    bool start(const uint8_t peer[16]) {
        if(active(_status.phase) || _pending) {show();return false;}
        Command command;command.action=Action::Start;std::memcpy(command.peer,peer,16);
        std::memcpy(_peer,command.peer,sizeof _peer);
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
    const uint8_t* peer() const {return _peer;}
    Code reason() const {return _error==Code::Ok?_status.reason:_error;}
    bool ready() const {
        return _error==Code::Ok && (_status.phase==Phase::Ready || _status.phase==Phase::Receiving || _status.phase==Phase::Talking);
    }
    bool canRetry() const {
        uint8_t known=0;for(auto byte:_peer) known|=byte;
        return known && !_pending && !active(_status.phase) && _status.capabilities &&
            (_status.phase==Phase::Ended || _error!=Code::Ok) && reason()!=Code::Off && reason()!=Code::NotContact;
    }
    void retry() {if(canRetry()) start(_peer);}
    const char* guidance() const {
        if(_pending || _status.phase==Phase::Finding) return "Looking for a usable connection";
        if(_status.phase==Phase::Calling) return "Waiting for the other person";
        if(_status.phase==Phase::Incoming || _status.phase==Phase::Connecting) return "Your microphone is off";
        if(_status.phase==Phase::Ending) return "Closing the connection";
        if(ready()) return (_status.capabilities&1)?"":"Listen only: no microphone";
        switch(reason()) {
        case Code::Off: return "Enable live voice in Settings > Voice";
        case Code::SlowRoute: return "Use WiFi/TCP or a faster LoRa route";
        case Code::IdentityUnknown: return "Wait for this peer to announce";
        case Code::ProfileUnsupported: return "No compatible voice codec";
        case Code::NoMemory: case Code::Busy: return "Try again when the device is free";
        case Code::AudioUnavailable: case Code::SlowCodec: return "Try again or continue by text";
        case Code::Timeout: case Code::RouteLost: return "Check the connection, then retry";
        default: return "";
        }
    }
    const char* text() const {
        if(_error!=Code::Ok) {Status failure;failure.phase=Phase::Ended;failure.reason=_error;return description(failure);}
        return _pending?"Requesting voice...":description(_status);
    }
    void failed(Code code) {_error=code;_pending=false;VoiceInput::instance().cancel();}
    void action(Action action) {
        if(!active(_status.phase) || (action==Action::Accept && _status.phase!=Phase::Incoming)) return;
        if(action==Action::End || action==Action::Decline) VoiceInput::instance().cancel();
        Command command;command.generation=_status.generation;command.action=action;
        if(!submit(command)) failed(Code::Busy);
    }
    void volume(int delta) {
        if(!ready() || !(_status.capabilities&2)) return;
        const int value=int(_status.volume)+delta;
        Command command;command.generation=_status.generation;command.action=Action::Volume;
        command.volume=uint8_t(value<0?0:value>100?100:value);
        if(submit(command)) _status.volume=command.volume;
    }
private:
    bool submit(const Command& c) {return _submit && _submit(_context,c);}
    void* _context=nullptr;Submit _submit=nullptr;
    Status _status;
    uint8_t _peer[16]{};
    uint32_t _offered=0;
    Code _error=Code::Ok;
    bool _visible=false,_pending=false,_incoming=false;
};
}
