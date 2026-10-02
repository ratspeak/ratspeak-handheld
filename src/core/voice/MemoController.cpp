#include "MemoController.h"
#include "storage/MessageStore.h"
#include <algorithm>
#include <cstring>

namespace handheld::memo {
namespace sm=storage::memo;
namespace {
Code audioError(voice::Code code) {
    if(code==voice::Code::NoMemory) return Code::NoMemory;
    if(code==voice::Code::Busy) return Code::Busy;
    return code==voice::Code::Ok?Code::Ok:Code::Interrupted;
}
}
void Controller::begin(const Deps& deps,const uint8_t local[16],uint8_t volume) {
    if(!drained()) return;
    _d=deps;memcpy(_local,local,16);_accepting=true;_closed=true;
    const auto generation=_status.generation;
    _status={};_status.generation=generation;_status.volume=std::min(uint8_t(100),volume);
    _status.capabilities=_d.audio?_d.audio->capabilities():0;
    _draft={};_media={};_packet={};_stopping=_recording=false;
    phase(Phase::Idle);
}
void Controller::phase(Phase value,Code code) {
    _status.phase=value;_status.reason=code;
    if(_status.revision<UINT32_MAX) ++_status.revision;
}
void Controller::review(Code code) {
    _recording=false;_stopping=false;_packet={};
    _status.draftRevision=_draft.revision;
    if(!_status.fromMessage) _status.length=_draft.state==sm::State::Ready?_draft.length:0;
    _status.frames=_status.length/sm::FrameBytes;
    phase(_status.fromMessage || _status.length?Phase::Review:Phase::Idle,code);
}
storage::RecordKey Controller::key() const {
    storage::RecordKey key;memcpy(key.peer,_status.peer,16);key.counter=_status.counter;key.incoming=_status.incoming;return key;
}
sm::Command Controller::binding() const {
    sm::Command command;memcpy(command.local,_local,16);memcpy(command.peer,_status.peer,16);
    command.revision=_draft.revision;command.timestamp=_sendTime;command.policy=_policy;return command;
}
Code Controller::command(const Command& command,sm::Command send) {
    if(!_d.store || !_d.audio || command.action>Action::Volume || command.volume>100) return Code::Invalid;
    if(command.action==Action::Open) {
        if(!_accepting || !command.view || command.view<=_viewFloor || _status.generation==UINT32_MAX) return Code::Stale;
        if(!drained() || busy(_status.phase)) return Code::Busy;
        _viewFloor=command.view;_status.view=command.view;++_status.generation;
        memcpy(_status.peer,command.peer,16);_status.counter=command.counter;_status.incoming=command.incoming;
        _status.fromMessage=command.counter!=0;_status.length=0;_status.frames=0;
        _draft={};_media={};_failure=Code::Ok;_closed=false;_stopping=false;
        _work=_status.fromMessage?Work::Message:Work::Inspect;phase(Phase::Loading);return Code::Ok;
    }
    if(command.view!=_status.view || command.generation!=_status.generation ||
        memcmp(command.peer,_status.peer,16)) return Code::Stale;
    if(command.action==Action::Stop || command.action==Action::Close) {
        if(command.action==Action::Close) _closed=true;
        if(_status.phase==Phase::Starting || _status.phase==Phase::Recording || _status.phase==Phase::Stopping) {
            _stopping=true;_d.audio->finishMemo();phase(Phase::Stopping);
        } else if(_status.phase==Phase::Playing) {_stopping=true;_d.audio->stop();}
        return Code::Ok;
    }
    if(!_accepting || _closed) return Code::Stale;
    if(command.action==Action::Volume) {
        _status.volume=command.volume;_d.audio->volume(command.volume);phase(_status.phase,_status.reason);return Code::Ok;
    }
    if(busy(_status.phase) || _ticket.valid() || _work!=Work::None || _audioOwned) return Code::Busy;
    if(command.draftRevision!=_draft.revision) return Code::Stale;
    _failure=Code::Ok;
    switch(command.action) {
    case Action::Record: case Action::Replace:
        if(_status.fromMessage) return Code::Invalid;
        if(!(_status.capabilities&1)) return Code::MicrophoneUnavailable;
        if(command.action==Action::Record && _draft.state==sm::State::Ready) return Code::Invalid;
        if(command.stopEpoch==UINT32_MAX || command.stopEpoch!=_d.audio->cancellationEpoch()) return Code::Stale;
        _epoch=command.stopEpoch;_started=_now;_stopping=false;_written=0;_packet={};
        _work=Work::Begin;phase(Phase::Starting);return Code::Ok;
    case Action::Play:
        if(!(_status.capabilities&2)) return Code::PlaybackUnavailable;
        if(!_status.length || _status.length>sm::PlaybackBytes || _status.length%sm::FrameBytes ||
            (_status.fromMessage && (_media.mode!=sm::Mode || _media.state!=1))) return Code::UnsupportedAudio;
        if(command.stopEpoch==UINT32_MAX || command.stopEpoch!=_d.audio->cancellationEpoch()) return Code::Stale;
        _epoch=command.stopEpoch;_offset=0;_started=_now;_packet={};_stopping=false;_recording=false;
        _status.frames=0;phase(Phase::Playing);return Code::Ok;
    case Action::Send:
        if(_status.fromMessage || _draft.state!=sm::State::Ready) return Code::Invalid;
        _sendTime=send.timestamp;_policy=send.policy;_work=Work::Promote;phase(Phase::Sending);return Code::Ok;
    case Action::Discard:
        if(_status.fromMessage) return Code::Invalid;
        _work=Work::Clear;phase(Phase::Saving);return Code::Ok;
    default: return Code::Invalid;
    }
}
void Controller::fail(Code code) {
    _failure=code;_stopping=true;
    if(_audioOwned) _d.audio->stop();
    if(!_ticket.valid()) _work=Work::None;
    _packet={};
    if(!_audioOwned && !_ticket.valid()) {
        if(_recording && _draft.workingRevision) _work=Work::Cancel;
        else review(code);
    }
}
void Controller::submit() {
    if(_ticket.valid() || _work==Work::None) return;
    auto command=binding();storage::Submission submission;
    using Op=storage::Operation;
    switch(_work) {
    case Work::Inspect: submission=_d.store->requestMemo(Op::MemoRead,command,nullptr,0,sizeof(sm::Snapshot));break;
    case Work::Begin:
        command.offset=_draft.workingRevision;submission=_d.store->requestMemo(Op::MemoBegin,command);break;
    case Work::Append:
        command.revision=_draft.workingRevision;command.offset=_written;
        submission=_d.store->requestMemo(Op::MemoAppend,command,_packet.bytes,_packet.length);break;
    case Work::Seal: case Work::Cancel:
        command.revision=_draft.workingRevision;
        submission=_d.store->requestMemo(_work==Work::Seal?Op::MemoSeal:Op::MemoCancel,command);break;
    case Work::Promote: submission=_d.store->requestMemo(Op::MemoPromote,command);break;
    case Work::Clear: submission=_d.store->requestMemo(Op::MemoClear,command);break;
    case Work::Message: submission=_d.store->requestRecord(key(),0,sizeof(storage::StoredRecordHeader));break;
    case Work::Clip:
        command.offset=_offset;
        submission=_status.fromMessage?_d.store->requestAudio(key(),_offset,sizeof(storage::StoredRecordHeader)+80):
            _d.store->requestMemo(Op::MemoRead,command,nullptr,0,sizeof(sm::Snapshot)+80);break;
    case Work::None: return;
    }
    if(submission.accepted()) {_ticket=submission.ticket;return;}
    if(submission.rejection==storage::Rejection::Busy || submission.rejection==storage::Rejection::Fenced) return;
    if(_work==Work::Cancel) {_work=Work::None;_recording=false;review(Code::StorageUnavailable);}
    else fail(Code::StorageUnavailable);
}
void Controller::settle() {
    if(!_ticket.valid()) return;
    storage::Result result;
    if(!_d.store->peekResult(_ticket,result)) return;
    const auto operation=_work;bool valid=result.outcome==storage::Outcome::Committed;
    sm::Snapshot snapshot;storage::StoredRecordHeader header;
    const bool message=operation==Work::Message || (operation==Work::Clip && _status.fromMessage);
    const size_t prefix=message?sizeof(header):sizeof(snapshot);
    if(valid) valid=result.length>=prefix && _d.store->readPayload(_ticket,message?static_cast<void*>(&header):&snapshot,prefix);
    if(valid && message) {
        valid=header.counter==_status.counter && header.incoming==_status.incoming &&
            !memcmp(header.incoming?header.source:header.destination,_status.peer,16) &&
            !memcmp(header.incoming?header.destination:header.source,_local,16);
    }
    if(valid && operation==Work::Clip && !_stopping) {
        const size_t length=result.length-prefix;
        valid=length && length<=80 && !(length%sm::FrameBytes) && _offset+length<=_status.length &&
            result.nextOffset==_offset+length;
        valid=valid && (message?!memcmp(&header.audio,&_media,sizeof(_media)):
            snapshot.revision==_draft.revision && snapshot.state==sm::State::Ready && snapshot.length==_status.length);
        if(valid) {
            _packet={};_packet.generation=_status.generation;_packet.length=length;
            valid=_d.store->readPayload(_ticket,_packet.bytes,length,prefix);
        }
    }
    _d.store->releaseResult(_ticket);_ticket={};_work=Work::None;
    if(!valid) {
        if(operation==Work::Cancel) {_recording=false;review(Code::StorageUnavailable);}
        else fail(result.error==storage::Error::Stale?Code::Stale:Code::StorageUnavailable);
        return;
    }
    if(!message && operation!=Work::Clip) {_draft=snapshot;_status.draftRevision=snapshot.revision;}
    switch(operation) {
    case Work::Inspect:
        if(snapshot.state==sm::State::Promoting && _accepting) {_work=Work::Promote;phase(Phase::Sending);}
        else review(snapshot.interrupted?Code::Interrupted:Code::Ok);
        break;
    case Work::Begin:
        _recording=true;
        if(_stopping || _closed || _epoch!=_d.audio->cancellationEpoch()) _work=Work::Cancel;
        break;
    case Work::Append:
        if(snapshot.workingLength!=_written+_packet.length) {fail(Code::Interrupted);break;}
        _written=snapshot.workingLength;_packet={};break;
    case Work::Seal: case Work::Clear: review();break;
    case Work::Cancel: _recording=false;review(_failure);break;
    case Work::Promote:
        _status.counter=result.key.counter;_status.length=0;
        if(_d.sent) _d.sent(_d.context,result.key);
        phase(Phase::Sent);break;
    case Work::Message:
        _media=header.audio;_status.length=header.audio.length;
        if(header.audio.state!=1) review(Code::AudioUnavailable);
        else if(header.audio.mode!=sm::Mode || !header.audio.length || header.audio.length>sm::PlaybackBytes || header.audio.length%sm::FrameBytes)
            review(Code::UnsupportedAudio);
        else review();
        break;
    case Work::Clip: break;
    case Work::None: break;
    }
}
void Controller::audio() {
    if(!_audioOwned && !_ticket.valid() && _work==Work::None &&
        ((_recording && _status.phase==Phase::Starting) || _status.phase==Phase::Playing)) {
        if(_stopping || _closed || _epoch!=_d.audio->cancellationEpoch()) {
            if(_recording) _work=Work::Cancel;else review();return;
        }
        const auto code=_d.audio->prepareMemo(_status.generation,_recording?voice::AudioUse::MemoRecord:voice::AudioUse::MemoPlayback,
            _recording?sm::RecordBytes/sm::FrameBytes:_status.length/sm::FrameBytes,_status.volume,_epoch);
        if(code==voice::Code::Busy && _now-_started<3000) return;
        if(code!=voice::Code::Ok) {
            // A failed post-task memory check can still own a stopping worker.
            // Retain that ownership until cooperative teardown has finished.
            _audioOwned=_d.audio->status().generation==_status.generation;
            fail(audioError(code));return;
        }
        _audioOwned=true;
    }
    if(!_audioOwned) return;
    const auto status=_d.audio->status();
    if(status.generation!=_status.generation) {fail(Code::AudioUnavailable);return;}
    if(status.frames!=_status.frames) {_status.frames=status.frames;phase(_status.phase,_status.reason);}
    _status.stackFree=status.stackFree;_status.encodeUs=status.encodeUs;_status.decodeUs=status.decodeUs;
    if(_recording && status.capturing && !_stopping && _status.phase==Phase::Starting) phase(Phase::Recording);
    if(status.error!=voice::Code::Ok && _failure==Code::Ok) fail(audioError(status.error));
    if(_recording) {
        if(!_ticket.valid() && _work==Work::None && _failure==Code::Ok) {
            if(!_packet.length) _d.audio->take(_packet);
            if(_packet.length) _work=Work::Append;
        }
        if(status.finished && !_ticket.valid() && _work==Work::None) {
            if(!_d.audio->drained()) return;
            _audioOwned=false;
            if(_failure!=Code::Ok || !_written) {
                if(_failure==Code::Ok) _failure=Code::TooShort;
                _work=Work::Cancel;
            } else {_work=Work::Seal;phase(Phase::Saving);}
        } else if(status.finished && _status.phase!=Phase::Stopping) phase(Phase::Stopping,_failure);
    } else {
        if(!_stopping && _failure==Code::Ok && _packet.length && _d.audio->receive(_packet)) {
            _offset+=_packet.length;_packet={};
        }
        if(!_stopping && _failure==Code::Ok && !_packet.length && !_ticket.valid() && _work==Work::None && _offset<_status.length)
            _work=Work::Clip;
        if(status.finished && !_ticket.valid()) {
            _work=Work::None;_packet={};
            if(_d.audio->drained()) {_audioOwned=false;review(_failure);}
        }
    }
}
void Controller::poll(uint64_t now) {
    if(!_d.store || !_d.audio) return;
    _now=now;settle();audio();submit();
}
void Controller::stop() {
    _accepting=false;_closed=true;_stopping=true;
    if(_audioOwned) {if(_recording) _d.audio->finishMemo();else _d.audio->stop();}
    if(_work==Work::Promote && !_ticket.valid()) {_work=Work::None;review();}
}
void Controller::volume(uint8_t value) {
    _status.volume=std::min(value,uint8_t(100));
    if(_d.audio) _d.audio->volume(_status.volume);
    phase(_status.phase,_status.reason);
}
bool Controller::drained() const {return !_audioOwned && !_ticket.valid() && _work==Work::None && !busy(_status.phase);}
}
