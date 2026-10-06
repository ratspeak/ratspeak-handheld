#include "MemoUi.h"
#include <algorithm>

namespace handheld::memo {
void Ui::open(const uint8_t peer[16],uint32_t counter,bool incoming) {
    requestOpen(peer,counter,incoming,false);
}
void Ui::toggleMessage(const uint8_t peer[16],uint32_t counter,bool incoming) {
    if(!counter) return;
    if(_visible && _inline && !_openAfterClose && !_end && _status.counter==counter &&
       _status.incoming==incoming && !memcmp(_status.peer,peer,16)) {
        if(_pending || _status.phase==Phase::Pausing) return;
        if(_status.phase==Phase::Playing) send(Action::Pause);
        else if(_status.phase==Phase::Review || _status.phase==Phase::Paused) send(Action::Play);
        else if(_status.phase==Phase::Unavailable || _status.phase==Phase::Sent) requestOpen(peer,counter,incoming,true,Action::Play);
        return;
    }
    requestOpen(peer,counter,incoming,true,Action::Play);
}
void Ui::retryMessage(const uint8_t peer[16],uint32_t counter,bool incoming) {
    if(counter && !incoming) requestOpen(peer,counter,incoming,true,Action::Retry);
}
void Ui::requestOpen(const uint8_t peer[16],uint32_t counter,bool incoming,bool inlinePlayback,Action afterOpen) {
    if(_end || _close || _pending || active()) {
        memcpy(_nextOpen.peer,peer,16);_nextOpen.counter=counter;_nextOpen.incoming=incoming;
        _nextInline=inlinePlayback;_nextAction=afterOpen;_visible=true;_openAction=Action::Close;
        if(_end) _openAfterEnd=true;
        else {_openAfterClose=true;stop(true);}
        return;
    }
    static uint32_t nextView=0;
    if(nextView==UINT32_MAX) return;
    const auto caps=_status.capabilities,volume=_status.volume;
    _status={};_status.view=++nextView;_status.capabilities=caps;_status.volume=volume;
    memcpy(_status.peer,peer,16);_status.counter=counter;_status.incoming=incoming;_status.fromMessage=counter!=0;
    _inline=inlinePlayback;_openAction=afterOpen;_elapsed=0;
    _status.phase=Phase::Loading;_visible=true;_menu=Menu::Main;_focus=0;_stop=false;_deletion=Deletion::None;
    send(Action::Open);
}
void Ui::send(Action action) {
    if(!_submit || _serial==UINT32_MAX) {_error=Code::Busy;_deletion=Deletion::None;return;}
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
        _deletion=Deletion::None;
        if(_action==Action::EndConversation) _endAccepted=false;
        if(_action==Action::Open) {
            _status.phase=Phase::Unavailable;_close=_stop=false;_openAction=Action::Close;
            if(_end) {
                if(_owner.generation) _status=_owner;
                else _status.phase=Phase::Closed;
            }
        }
    } else if(_action==Action::Close || _action==Action::Stop || _action==Action::EndConversation) {
        _pending=false;
        if(_action==Action::Close) _close=false;
        if(_action==Action::Stop) _stop=false;
        if(_action==Action::EndConversation) {_endAccepted=true;_close=_stop=false;}
    }
}
void Ui::update(const Status& value,bool foregroundAllowed) {
    if(!_status.view) {_status.capabilities=value.capabilities;_status.volume=value.volume;return;}
    const auto before=layout();
    const auto boundGeneration=std::max(_status.generation,_owner.generation);
    if(!value.view && boundGeneration && value.generation>=boundGeneration) {
        voice::VoiceWorker::emergencyStop();_status.generation=0;_status.phase=Phase::Unavailable;
        _owner={};
        _error=Code::Stale;_pending=_close=_stop=_end=_endAccepted=_openAfterEnd=_openAfterClose=false;_openAction=Action::Close;_menu=Menu::Main;_deletion=Deletion::None;
    }
    if(value.view==_status.view && !memcmp(value.peer,_status.peer,16) &&
        value.generation>=_status.generation && value.revision>=_status.revision) {
        if(!_pending || (_action!=Action::Close && _action!=Action::Stop && value.revision>_anchor &&
            (_action!=Action::Volume || value.volume==_status.volume))) {
            // A confirmation belongs to the clip the user saw, not a newer one.
            if(value.draftRevision!=_status.draftRevision) _menu=Menu::Main;
            if(value.phase==Phase::Playing || value.phase==Phase::Pausing || value.phase==Phase::Paused) _elapsed=value.frames;
            else if((_status.phase==Phase::Playing || _status.phase==Phase::Pausing) && value.phase==Phase::Review && value.reason==Code::Ok)
                _elapsed=value.length/4;
            _status=value;
            _owner=value.phase==Phase::Closed?Status{}:value;
            if(_pending) {_pending=false;_error=Code::Ok;}
            if(_status.phase!=Phase::Review) _menu=Menu::Main;
            if(_deletion!=Deletion::None) {
                if(_status.reason!=Code::Ok) _deletion=Deletion::None;
                else if(_status.draftRevision>_deleteRevision && !_status.length && _status.phase!=Phase::Saving) {
                    const bool close=_deletion==Deletion::Close && _status.phase==Phase::Idle;
                    _deletion=Deletion::None;
                    if(close) hide();
                }
            }
        }
    }
    if(_visible && !foregroundAllowed) hide();
    if(_visible && foregroundAllowed && !_close) voice::VoiceWorker::keepInputAlive();
    // Close/Stop remain outstanding across mailbox pressure or an Open whose
    // generation had not reached the UI yet. Hardware cancellation is immediate.
    if(_end && (_status.phase==Phase::Closed) && !_pending) {
        _end=_endAccepted=_close=_stop=false;
        if(_openAfterEnd) {
            const auto next=_nextOpen;_openAfterEnd=false;
            requestOpen(next.peer,next.counter,next.incoming,_nextInline,_nextAction);
        }
    }
    if(((_end && !_endAccepted) || _close || _stop) && !_pending && _status.generation && value.view==_status.view)
        send(_end?Action::EndConversation:_close?Action::Close:Action::Stop);
    if(_openAfterClose && !_end && !_close && !_pending && !active()) {
        const auto next=_nextOpen;_openAfterClose=false;
        requestOpen(next.peer,next.counter,next.incoming,_nextInline,_nextAction);
    }
    if(_openAction!=Action::Close && !_pending && !_close && !_end && !_openAfterClose && _status.phase==Phase::Review) {
        const auto action=_openAction;_openAction=Action::Close;
        if(_status.reason==Code::Ok && (action==Action::Play?bool(_status.capabilities&2):_status.retryable)) send(action);
    }
    changed(before);
}
void Ui::stop(bool close) {
    voice::VoiceWorker::emergencyStop();_menu=Menu::Main;
    _close|=close;_stop=!_close;
    // Supersede a queued Record; its epoch can no longer start the microphone.
    if(_status.generation) send(_end?Action::EndConversation:_close?Action::Close:Action::Stop);
}
void Ui::hide() {
    if(!_visible) return;
    _visible=false;
    _openAfterEnd=_openAfterClose=false;_openAction=Action::Close;
    if(!_status.generation && !_pending) return;
    stop(true);
}
void Ui::closeConversation() {
    _visible=false;_openAfterEnd=_openAfterClose=false;_openAction=Action::Close;
    if(!_status.view) return;
    if(!_status.generation && !_pending) {
        if(!_owner.generation) return;
        _status=_owner;
    }
    if(_status.phase==Phase::Closed && !_pending) return;
    _end=true;_endAccepted=false;_deletion=Deletion::None;
    stop(true);
}
void Ui::back() {
    if(!_visible || _deletion!=Deletion::None) return;
    if(_end) {hide();return;}
    const auto before=layout();
    if(_menu!=Menu::Main) _menu=Menu::Main;else hide();
    changed(before);
}
unsigned Ui::count() const {
    unsigned n=0;while(n<3 && choice(n)!=Choice::None) ++n;return n;
}
Ui::Choice Ui::choice(unsigned index) const {
    if(index>=3) return Choice::None;
    if(_end) return index==0 && _openAfterEnd?Choice::Back:Choice::None;
    if(_deletion!=Deletion::None) return Choice::None;
    if(_menu==Menu::Replace || _menu==Menu::Discard)
        return index==0?(_menu==Menu::Replace?Choice::ConfirmReplace:Choice::ConfirmDiscard):index==1?Choice::Cancel:Choice::None;
    if(_menu==Menu::More) return index==0?Choice::Back:index==1?Choice::Replace:Choice::Discard;
    if(_pending && _action!=Action::Record && _action!=Action::Replace && _action!=Action::Volume) return index==0?Choice::Back:Choice::None;
    if(_stop || _close) return index==0?Choice::Back:Choice::None;
    if((_pending && (_action==Action::Record || _action==Action::Replace)) || _status.phase==Phase::Starting || _status.phase==Phase::Recording || _status.phase==Phase::Playing)
        return index==0?(_status.phase==Phase::Playing?Choice::StopPlayback:Choice::Stop):index==1?Choice::Back:Choice::None;
    if(_status.phase==Phase::Review || _status.phase==Phase::Paused) {
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
    case Choice::Back: back();break;
    case Choice::Cancel: _menu=Menu::Main;break;
    case Choice::More: _menu=Menu::More;break;
    case Choice::Replace: _menu=Menu::Replace;break;
    case Choice::Discard: _menu=Menu::Discard;break;
    case Choice::Stop: case Choice::StopPlayback: stop(false);break;
    case Choice::Record: send(Action::Record);break;
    case Choice::Play: send(Action::Play);break;
    case Choice::Send: send(Action::Send);break;
    case Choice::RetryDelivery: send(Action::Retry);break;
    case Choice::ConfirmReplace: case Choice::ConfirmDiscard:
        _menu=Menu::Main;_deleteRevision=_status.draftRevision;
        _deletion=choiceValue==Choice::ConfirmReplace?Deletion::RecordAgain:Deletion::Close;
        send(choiceValue==Choice::ConfirmReplace?Action::Replace:Action::Discard);break;
    case Choice::Retry: {uint8_t peer[16];memcpy(peer,_status.peer,16);open(peer,_status.counter,_status.incoming);break;}
    case Choice::None: break;
    }
    changed(before);
}
void Ui::move(int delta) {const int n=int(count());if(n) _focus=uint8_t((int(_focus)+delta%n+n)%n);}
void Ui::volume(int delta) {
    if(!_visible || _pending || _end || _deletion!=Deletion::None || !(_status.capabilities&2)) return;
    _status.volume=uint8_t(std::max(0,std::min(100,int(_status.volume)+delta)));send(Action::Volume);
}
uint32_t Ui::layout() const {
    return uint32_t(choice(0))|(uint32_t(choice(1))<<8)|(uint32_t(choice(2))<<16);
}
void Ui::changed(uint32_t before) {
    if(before!=layout() || _focus>=count()) _focus=(_menu==Menu::Replace || _menu==Menu::Discard)?1:0;
}
const char* Ui::label(Choice c) {
    switch(c) {
    case Choice::Record: return "Record";case Choice::Stop: return "Stop";case Choice::Play: return "Play";
    case Choice::StopPlayback: return "Stop playback";
    case Choice::RetryDelivery: return "Retry";
    case Choice::Send: return "Send";case Choice::More: return "More";case Choice::Back: return "Back";
    case Choice::Replace: return "Record again";case Choice::Discard: return "Discard";case Choice::Cancel: return "Cancel";
    case Choice::ConfirmReplace: case Choice::ConfirmDiscard: return "Delete";case Choice::Retry: return "Retry";
    case Choice::None: return "";
    }return "";
}
const char* Ui::text() const {
    if(_end) return _status.reason==Code::StorageUnavailable?"Storage unavailable":_openAfterEnd?"Opening...":"Closing...";
    if(_deletion!=Deletion::None) return "Deleting...";
    if(_menu==Menu::Replace || _menu==Menu::Discard) return "Delete current clip?";
    if(_menu==Menu::More) return "Voice clip";
    if(_error!=Code::Ok) {auto s=_status;s.reason=_error;return description(s);}
    if(_stop || _close) return "Stopping...";
    if(_pending && _action!=Action::Volume) return _action==Action::Record || _action==Action::Replace?"Starting...":_action==Action::Send?"Adding to messages...":_action==Action::Retry?"Retrying message...":"Please wait...";
    return description(_status);
}
const char* Ui::guidance() const {
    if(_end) return "";
    if(_menu==Menu::Replace || _deletion==Deletion::RecordAgain) return "Then record a new clip";
    if(_menu==Menu::Discard || _deletion==Deletion::Close) return "Return to the conversation";
    if(_status.phase==Phase::Recording || _status.phase==Phase::Starting) return "Tap Stop when you are done";
    if(_status.reason==Code::Recovered) return "Previous recording wasn't saved";
    if(_status.reason==Code::NoMemory) return "Close other activity and retry";
    if(_status.reason==Code::DeviceBusy || _status.reason==Code::CaptureOverflow) return "Try again when the device is idle";
    if(_status.reason==Code::InputLost) return "Keep this screen open to record";
    if(_status.phase==Phase::Review && !_status.fromMessage) return "Leaving this chat deletes the clip";
    if(_status.phase==Phase::Review && !(_status.capabilities&2)) return "No speaker on this device";
    if(_status.phase==Phase::Idle) return (_status.capabilities&1)?"Up to 15 seconds":"No microphone on this device";
    if(_status.phase==Phase::Sent) return "Check delivery in the conversation";
    return "";
}
}
