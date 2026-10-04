#include "MemoUi.h"
#include <algorithm>

namespace handheld::memo {
void Ui::open(const uint8_t peer[16],uint32_t counter,bool incoming) {
    if(_close || active()) {if(_status.view) _visible=true;return;}
    static uint32_t nextView=0;
    if(nextView==UINT32_MAX) return;
    const auto caps=_status.capabilities,volume=_status.volume;
    _status={};_status.view=++nextView;_status.capabilities=caps;_status.volume=volume;
    memcpy(_status.peer,peer,16);_status.counter=counter;_status.incoming=incoming;_status.fromMessage=counter!=0;
    _status.phase=Phase::Loading;_visible=true;_menu=Menu::Main;_focus=0;_stop=false;
    send(Action::Open);
}
void Ui::send(Action action) {
    if(!_submit || _serial==UINT32_MAX) {_error=Code::Busy;return;}
    Command c;c.action=action;c.view=_status.view;c.generation=_status.generation;
    c.draftRevision=_status.draftRevision;c.counter=_status.counter;c.incoming=_status.incoming;
    c.volume=_status.volume;c.stopEpoch=voice::VoiceWorker::stopEpoch();memcpy(c.peer,_status.peer,16);
    _action=action;_anchor=_status.revision;_pending=true;_error=Code::Ok;
    const auto serial=++_serial;
    if(!_submit(_context,c,serial)) acknowledge(serial,Code::Busy);
}
void Ui::acknowledge(uint32_t serial,Code code) {
    if(serial!=_serial) return;
    if(code!=Code::Ok) {
        _pending=false;_error=code;
        if(_action==Action::Open) {_status.phase=Phase::Unavailable;_close=_stop=false;}
    } else if(_action==Action::Close || _action==Action::Stop || _action==Action::Volume) {
        _pending=false;
        if(_action==Action::Close) _close=false;
        if(_action==Action::Stop) _stop=false;
    }
}
void Ui::update(const Status& value,bool foregroundAllowed) {
    if(!_status.view) {_status.capabilities=value.capabilities;_status.volume=value.volume;return;}
    const auto before=layout();
    if(!value.view && _status.generation && value.generation>=_status.generation) {
        voice::VoiceWorker::emergencyStop();_status.generation=0;_status.phase=Phase::Unavailable;
        _error=Code::Stale;_pending=_close=_stop=false;_menu=Menu::Main;
    }
    if(value.view==_status.view && !memcmp(value.peer,_status.peer,16)) {
        if(!_pending || (_action!=Action::Close && _action!=Action::Stop && _action!=Action::Volume && value.revision>_anchor)) {
            _status=value;
            if(_pending) {_pending=false;_error=Code::Ok;}
            if(_status.phase!=Phase::Review) _menu=Menu::Main;
        }
    }
    if(_visible && !foregroundAllowed) hide();
    if(_visible && foregroundAllowed && !_close) voice::VoiceWorker::keepInputAlive();
    // Close/Stop remain outstanding across mailbox pressure or an Open whose
    // generation had not reached the UI yet. Hardware cancellation is immediate.
    if((_close || _stop) && !_pending && _status.generation && value.view==_status.view)
        send(_close?Action::Close:Action::Stop);
    changed(before);
}
void Ui::stop(bool close) {
    voice::VoiceWorker::emergencyStop();_menu=Menu::Main;
    _close|=close;_stop=!_close;
    // Supersede a queued Record; its epoch can no longer start the microphone.
    if(_status.generation) send(_close?Action::Close:Action::Stop);
}
void Ui::hide() {
    if(!_visible) return;
    _visible=false;
    if(!_status.generation && !_pending) return;
    stop(true);
}
unsigned Ui::count() const {
    unsigned n=0;while(n<3 && choice(n)!=Choice::None) ++n;return n;
}
Ui::Choice Ui::choice(unsigned index) const {
    if(index>=3) return Choice::None;
    if(_menu==Menu::Replace || _menu==Menu::Discard)
        return index==0?Choice::Keep:index==1?(_menu==Menu::Replace?Choice::ConfirmReplace:Choice::ConfirmDiscard):Choice::None;
    if(_menu==Menu::More) return index==0?Choice::Back:index==1?Choice::Replace:Choice::Discard;
    if(_pending && _action!=Action::Record && _action!=Action::Replace) return index==0?Choice::Back:Choice::None;
    if(_stop || _close) return index==0?Choice::Back:Choice::None;
    if((_pending && (_action==Action::Record || _action==Action::Replace)) || _status.phase==Phase::Starting || _status.phase==Phase::Recording || _status.phase==Phase::Playing)
        return index==0?(_status.phase==Phase::Playing?Choice::StopPlayback:Choice::Stop):index==1?Choice::Back:Choice::None;
    if(_status.phase==Phase::Review) {
        const bool play=(_status.capabilities&2) && _status.reason!=Code::UnsupportedAudio && _status.reason!=Code::AudioUnavailable;
        if(_status.fromMessage) {
            if(play && index==0) return Choice::Play;
            const unsigned remaining=index-(play?1:0);
            return remaining==0?(_status.retryable?Choice::RetryDelivery:Choice::Back):
                remaining==1 && _status.retryable?Choice::Back:Choice::None;
        }
        return index==0?(play?Choice::Play:Choice::Send):index==1?(play?Choice::Send:Choice::More):play?Choice::More:Choice::None;
    }
    if(_status.phase==Phase::Idle && (_status.capabilities&1)) return index==0?Choice::Record:index==1?Choice::Back:Choice::None;
    if(_status.phase==Phase::Unavailable) return index==0?Choice::Retry:index==1?Choice::Back:Choice::None;
    return index==0?Choice::Back:Choice::None;
}
void Ui::choose(Choice choiceValue) {
    if(!_visible) return;
    bool offered=false;for(unsigned i=0;i<count();++i) offered|=choice(i)==choiceValue;
    if(!offered) return;
    const auto before=layout();
    switch(choiceValue) {
    case Choice::Back: if(_menu!=Menu::Main) _menu=Menu::Main;else hide();break;
    case Choice::Keep: _menu=Menu::Main;break;
    case Choice::More: _menu=Menu::More;break;
    case Choice::Replace: _menu=Menu::Replace;break;
    case Choice::Discard: _menu=Menu::Discard;break;
    case Choice::Stop: case Choice::StopPlayback: stop(false);break;
    case Choice::Record: send(Action::Record);break;
    case Choice::Play: send(Action::Play);break;
    case Choice::Send: send(Action::Send);break;
    case Choice::RetryDelivery: send(Action::Retry);break;
    case Choice::ConfirmReplace: _menu=Menu::Main;send(Action::Replace);break;
    case Choice::ConfirmDiscard: _menu=Menu::Main;send(Action::Discard);break;
    case Choice::Retry: {uint8_t peer[16];memcpy(peer,_status.peer,16);open(peer,_status.counter,_status.incoming);break;}
    case Choice::None: break;
    }
    changed(before);
}
void Ui::move(int delta) {const int n=int(count());if(n) _focus=uint8_t((int(_focus)+delta%n+n)%n);}
void Ui::volume(int delta) {
    if(!_visible || _pending || !(_status.capabilities&2)) return;
    _status.volume=uint8_t(std::max(0,std::min(100,int(_status.volume)+delta)));send(Action::Volume);
}
uint32_t Ui::layout() const {
    return uint32_t(choice(0))|(uint32_t(choice(1))<<8)|(uint32_t(choice(2))<<16);
}
void Ui::changed(uint32_t before) {if(before!=layout() || _focus>=count()) _focus=0;}
const char* Ui::label(Choice c) {
    switch(c) {
    case Choice::Record: return "Record";case Choice::Stop: return "Stop";case Choice::Play: return "Play";
    case Choice::StopPlayback: return "Stop playback";
    case Choice::RetryDelivery: return "Retry";
    case Choice::Send: return "Send";case Choice::More: return "More";case Choice::Back: return "Back";
    case Choice::Replace: return "Record again";case Choice::Discard: return "Discard";case Choice::Keep: return "Keep draft";
    case Choice::ConfirmReplace: return "Record again";case Choice::ConfirmDiscard: return "Discard";case Choice::Retry: return "Retry";
    case Choice::None: return "";
    }return "";
}
const char* Ui::text() const {
    if(_menu==Menu::Replace) return "Replace this recording?";
    if(_menu==Menu::Discard) return "Discard this recording?";
    if(_menu==Menu::More) return "Saved draft";
    if(_error!=Code::Ok) {auto s=_status;s.reason=_error;return description(s);}
    if(_stop || _close) return "Stopping...";
    if(_pending) return _action==Action::Record || _action==Action::Replace?"Starting...":_action==Action::Send?"Adding to messages...":_action==Action::Retry?"Retrying message...":"Please wait...";
    return description(_status);
}
const char* Ui::guidance() const {
    if(_menu==Menu::Replace) return "Keep the draft or record a new clip";
    if(_menu==Menu::Discard) return "This removes the saved draft";
    if(_status.phase==Phase::Recording || _status.phase==Phase::Starting) return "Tap Stop when you are done";
    if(_status.reason==Code::Recovered) return "Previous recording wasn't saved";
    if(_status.reason==Code::NoMemory) return "Close other activity and retry";
    if(_status.reason==Code::DeviceBusy || _status.reason==Code::CaptureOverflow) return "Try again when the device is idle";
    if(_status.reason==Code::InputLost) return "Keep this screen open to record";
    if(_status.phase==Phase::Review && !_status.fromMessage) return "Back keeps this draft";
    if(_status.phase==Phase::Review && !(_status.capabilities&2)) return "No speaker on this device";
    if(_status.phase==Phase::Idle) return (_status.capabilities&1)?"Up to 15 seconds":"No microphone on this device";
    if(_status.phase==Phase::Sent) return "Check delivery in the conversation";
    return "";
}
}
