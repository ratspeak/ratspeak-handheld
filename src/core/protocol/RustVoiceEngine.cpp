#include "protocol/RustVoiceEngine.h"
#include "voice/VoiceProfile.h"
#include "protocol/RustClock.h"
#include "protocol/RustEntropy.h"
#include "protocol/RustKeyMap.h"
#include "protocol/RustInterfacePump.h"
#include <cstring>
using namespace handheld::voice;
namespace {
constexpr uint32_t ControlWait=2000;
constexpr uint8_t Ulbw=0x10,Vlbw=0x20,Lbw=0x30;
bool permitted(Route policy,uint8_t iface) {
    return policy==Route::Auto || (policy==Route::IpOnly && iface!=0) || (policy==Route::LoRaOnly && iface==0);
}
}
uint64_t RustVoiceEngine::now() const { return _d.clock ? _d.clock->nowMs() : uint64_t(millis()); }
bool RustVoiceEngine::same(RustLinkManager::Handle h) const {return h.valid() && h.slot==_link.slot && h.generation==_link.generation;}
void RustVoiceEngine::begin(const Deps& deps) {
    _d=deps;_accepting=true;_configured=false;
    _status.capabilities=_d.audio?_d.audio->capabilities():0;
    _status.phase=_status.capabilities?Phase::Off:Phase::Unavailable;
    if(_d.links) _d.links->setVoiceSink(this);
}
void RustVoiceEngine::configure(const Settings& value) {
    if(_configured && value.enabled==_settings.enabled && value.contactsOnly==_settings.contactsOnly &&
        value.volume==_settings.volume && value.route==_settings.route) return;
    _configured=true;
    const bool stopNeeded=active(_status.phase) && (!value.enabled || value.route!=_settings.route || value.contactsOnly!=_settings.contactsOnly);
    _settings=value;if(_settings.volume>100) _settings.volume=100;
    if(uint8_t(_settings.route)>uint8_t(Route::LoRaOnly)) _settings.route=Route::Auto;
    _status.volume=_settings.volume;
    if(_d.audio) _d.audio->volume(_settings.volume);
    if(stopNeeded) terminate(Code::Local);
    if(_d.ctx) rs_handheld_voice_enable(_d.ctx,_settings.enabled && _status.capabilities && _accepting);
    if(!active(_status.phase) && _status.phase!=Phase::Ended)
        _status.phase=!_status.capabilities?Phase::Unavailable:_settings.enabled?Phase::Idle:Phase::Off;
}
bool RustVoiceEngine::profileFits(uint8_t profile,uint8_t iface,uint8_t hops) const {
    const auto shape=profileInfo(profile);
    return shape.native_samples && _d.pump && _d.pump->admitsVoice(iface,shape.raw_bytes,shape.interval_ms,hops);
}
uint32_t RustVoiceEngine::allowedProfiles(uint8_t iface,uint8_t hops) const {
    if(!permitted(_settings.route,iface)) return 0;
    return (profileFits(Vlbw,iface,hops)?1u:0u) | (profileFits(Lbw,iface,hops)?2u:0u) | (profileFits(Ulbw,iface,hops)?4u:0u);
}
bool RustVoiceEngine::admitVoice(uint8_t iface,uint8_t hops) const {
    return _accepting && _settings.enabled && _status.capabilities && !active(_status.phase) && !_sessionLive &&
        allowedProfiles(iface,hops)!=0;
}
bool RustVoiceEngine::initialise(bool incoming,uint8_t iface,uint8_t hops) {
    const uint32_t allowed=allowedProfiles(iface,hops);
    if(!_d.audio || !_d.links || !allowed) return false;
    const auto preferred=(allowed&2)?Lbw:(allowed&1)?Vlbw:Ulbw;
    if(rs_handheld_voice_session_size()>sizeof _session || rs_handheld_voice_session_align()>8 ||
        rs_handheld_voice_session_init(_session,sizeof _session,incoming,preferred,allowed,
            bool(_status.capabilities&1),bool(_status.capabilities&2),now())!=RS_HANDHELD_OK) return false;
    _audioReady=0;_audioWanted=false;_view=0;_blockedEpoch=0;_inputEpoch=0;_identify=false;_signalCount=0;_controlToken=0;
    _lastMediaOutcome=0;_workerDrops=0;_mediaFailures=0;
    _sessionLive=true;_hops=hops;_status.iface=iface;_status.profile=preferred;_status.incoming=incoming;
    _status.phase=incoming?Phase::Connecting:Phase::Calling;
    _result={};_result.profile=preferred;_result.audio_generation=1;
    return true;
}
Code RustVoiceEngine::command(const Command& c,uint32_t cancellationEpoch) {
    if(c.action==Action::Start) {
        if(!_accepting || !_settings.enabled || !_status.capabilities) return Code::Off;
        if(active(_status.phase) || !drained()) return Code::Busy;
        if(_status.generation==UINT32_MAX) return Code::Busy;
        const auto capabilities=_status.capabilities,volume=_status.volume;
        const auto generation=_status.generation+1;_status={};_status.generation=generation;
        _status.capabilities=capabilities;_status.volume=volume;
        std::memcpy(_status.peer,c.peer,16);_view=0;_born=now();_lastRequest=0;_sequence=0;
        _status.phase=Phase::Finding;_status.reason=Code::Ok;_routeFailure=Code::Timeout;
        if(!_d.keys || !_d.keys->recall(c.peer,_publicKey) ||
            rs_handheld_voice_destination(_publicKey,_destination)!=RS_HANDHELD_OK) {terminate(Code::IdentityUnknown);return Code::IdentityUnknown;}
        findRoute();return Code::Ok;
    }
    if(c.generation!=_status.generation || !active(_status.phase)) return Code::Invalid;
    if(c.action==Action::End || c.action==Action::Decline) {terminate(c.action==Action::Decline?Code::Rejected:Code::Local);return Code::Ok;}
    if(c.action==Action::Volume) {_status.volume=c.volume>100?100:c.volume;if(_d.audio)_d.audio->volume(_status.volume);return Code::Ok;}
    if(!_sessionLive || !_status.verified || _closing) return Code::Invalid;
    if(c.action==Action::Accept) {apply(RS_HANDHELD_VOICE_ANSWER);return Code::Ok;}
    if(c.action==Action::TalkDown) {
        if(!c.view || c.stopEpoch!=cancellationEpoch || cancellationEpoch==UINT32_MAX || c.stopEpoch==_blockedEpoch || !(_result.flags&1)) return Code::Invalid;
        _view=c.view;_inputEpoch=c.stopEpoch;_talkBorn=now();apply(RS_HANDHELD_VOICE_PTT,1);return Code::Ok;
    }
    if(c.action==Action::TalkUp) {apply(RS_HANDHELD_VOICE_PTT,0);_view=0;return Code::Ok;}
    return Code::Invalid;
}
void RustVoiceEngine::findRoute() {
    if(_status.phase!=Phase::Finding || !_d.ctx) return;
    if(now()-_born>=30000) {terminate(_routeFailure);return;}
    rs_handheld_route_t route{};
    uint8_t rejected=UINT8_MAX;
    if(rs_handheld_rns_route(_d.ctx,_destination,now(),&route)==RS_HANDHELD_OK && route.kind==RS_HANDHELD_ROUTE_DIRECT) {
        _status.iface=route.interface_id;
        if(!permitted(_settings.route,route.interface_id)) _routeFailure=Code::RoutePolicyUnavailable;
        else if(!allowedProfiles(route.interface_id,route.hops)) _routeFailure=route.interface_id==0?Code::RfUnsupported:Code::RouteLost;
        else {
            if(!initialise(false,route.interface_id,route.hops)) {terminate(Code::AudioUnavailable);return;}
            _d.links->openVoice(_destination,_publicKey,route,_link);
            if(!_link.valid()) terminate(Code::Busy);
            return;
        }
        rejected=route.interface_id;
        // Reticulum owns the path table. Ask other usable interfaces for a
        // fresh path instead of rejecting an Auto call solely on a slow LoRa
        // route; never invent or overwrite another interface's route locally.
        bool alternative=false;
        for(uint8_t iface=0;iface<=6;++iface)
            alternative|=iface!=rejected && allowedProfiles(iface,1)!=0;
        if(!alternative) {terminate(_routeFailure);return;}
    }
    if(!_lastRequest || now()-_lastRequest>=5000) {
        _lastRequest=now();
        for(uint8_t iface=0;iface<=6;++iface) if(iface!=rejected && allowedProfiles(iface,1)) {
            uint8_t tag[16];RustEntropy::fill(tag,sizeof tag);rs_handheld_rns_request_path(_d.ctx,_destination,tag,iface,now());
        }
    }
}
void RustVoiceEngine::signal(uint32_t value) {
    if(_signalCount>=8) {terminate(Code::Busy);return;}
    if(!_signalCount) _controlBorn=now();
    _signals[_signalCount++]=value;
}
void RustVoiceEngine::apply(uint32_t operation,uint32_t argument,uint32_t extra) {
    if(!_sessionLive) return;
    rs_handheld_voice_result_t result{};
    if(rs_handheld_voice_session_apply(_session,operation,argument,extra,now(),&result)!=RS_HANDHELD_OK) {terminate(Code::AudioUnavailable);return;}
    if ((_result.flags&2) && !(result.flags&2) && operation!=RS_HANDHELD_VOICE_PTT) _blockedEpoch=_inputEpoch;
    _result=result;_status.profile=uint8_t(result.profile);
    for(uint32_t n=0;n<result.event_count;++n) {
        const auto event=result.events[n];
        switch(event.kind) {
            case 1: signal(event.value);break;
            case 2: _identify=true;_controlBorn=now();break;
            case 3: case 12: _audioReady=0;break;
            case 6: case 7: _audioWanted=true;break;
            case 20: if(_d.audio)_d.audio->capture(event.value!=0,_inputEpoch);break;
            case 21: if(_d.audio)_d.audio->flush();if(_d.links)_d.links->cancelVoiceMedia(_link);break;
            case 10: _closing=true;_closeAt=now()+1000;_audioWanted=false;if(_d.audio)_d.audio->stop();break;
            case 22: if(_status.reason==Code::Ok) _status.reason=Code(event.value);break;
            default:break; // Dial/ring never open microphone or allocate a second audio owner.
        }
    }
    publish();
}
void RustVoiceEngine::terminate(Code reason) {
    if(_closing) return;
    _closing=true;
    _status.reason=reason;
    if(_d.audio) {_d.audio->capture(false);_d.audio->flush();_d.audio->stop();}
    if(_d.links) _d.links->cancelVoiceMedia(_link);
    if(_sessionLive) apply(RS_HANDHELD_VOICE_END,uint32_t(reason)<=8?uint32_t(reason):7);
    _closing=true;_audioWanted=false;_closeAt=now()+1000;_status.phase=Phase::Ending;
}
void RustVoiceEngine::sendControl() {
    if(!_d.links || !_d.links->voiceActive(_link) || _controlToken) return;
    if(_identify) {
        if(_d.links->voiceIdentified(_link)) _identify=false;
        else {_d.links->identifyVoice(_link,_controlBorn,ControlWait);return;}
    }
    if(!_signalCount) return;
    if(now()<_controlBorn || now()-_controlBorn>=ControlWait) {
        _signalCount=0;if(!_closing)terminate(Code::RouteLost);return;
    }
    uint8_t raw[96];size_t length=0;
    if(rs_handheld_voice_packet_encode(_signals,_signalCount,2,nullptr,0,raw,sizeof raw,&length)!=RS_HANDHELD_OK) {terminate(Code::ProfileUnsupported);return;}
    const auto count=_signalCount;
    const auto token=++_sequence;
    _controlToken=token;_signalCount=0;
    if(!_d.links->sendVoice(_link,raw,length,token,_controlBorn,ControlWait)) {
        if(_controlToken==token) {_controlToken=0;_signalCount=count;}
    }
}
void RustVoiceEngine::publish() {
    if(_closing) {_status.phase=Phase::Ending;return;}
    if(!_sessionLive) return;
    if(_result.flags&2) _status.phase=Phase::Talking;
    else if(_result.flags&1) _status.phase=Phase::Ready;
    else if(_result.status==4 && _status.incoming) _status.phase=Phase::Incoming;
    else if(_result.status>=5) _status.phase=Phase::Connecting;
    else _status.phase=_status.incoming?Phase::Connecting:Phase::Calling;
}
void RustVoiceEngine::loop(uint32_t cancellationEpoch) {
    if(_status.phase==Phase::Finding) findRoute();
    if(_sessionLive && !_closing && !profileFits(_status.profile,_status.iface,_hops))
        terminate(!_d.pump->interfaceOnline(_status.iface)?Code::RouteLost:_status.iface==0?Code::RfUnsupported:Code::SlowRoute);
    if(_sessionLive && !_closing) {
        if((_result.flags&2) && (_inputEpoch!=cancellationEpoch || (_status.iface==0 && now()-_talkBorn>=10000))) {
            // Cancel the hardware immediately; keep Rust's held-input latch until
            // an actual TalkUp arrives, so timeout cannot be renewed by repeats.
            if(_d.audio)_d.audio->capture(false);
            _blockedEpoch=_inputEpoch;apply(RS_HANDHELD_VOICE_PTT,0);_view=0;
        }
        apply(RS_HANDHELD_VOICE_TICK);
        if(_audioWanted && !_closing) {
            const auto code=_d.audio->prepare(_result.audio_generation,_status.profile,_status.volume);
            if(code!=Code::Ok && code!=Code::Busy) terminate(code);
            const auto audio=_d.audio->status();
            if(audio.generation==_result.audio_generation) {
                if(audio.error!=Code::Ok) terminate(audio.error);
                else if(audio.ready && _audioReady!=audio.generation) {_audioReady=audio.generation;apply(RS_HANDHELD_VOICE_AUDIO_READY,audio.generation,1);}
                if(audio.dropped>=_workerDrops) _status.rxDrops+=audio.dropped-_workerDrops;
                _workerDrops=audio.dropped;
                _status.stackFree=audio.stackFree;_status.encodeUs=audio.encodeUs;_status.decodeUs=audio.decodeUs;
                if((_result.flags&4) && audio.receiving) _status.phase=Phase::Receiving;
            }
        }
    }
    if (_sequence>=0x7ffffffeu) terminate(Code::Busy);
    sendControl();
    if(_sessionLive && !_closing && (_result.flags&2)) {
        const uint32_t MediaWait=mediaAge(profileInfo(_status.profile));
        Encoded packet;
        if(_d.audio->take(packet) && packet.generation==_result.audio_generation && now()>=packet.bornMs && now()-packet.bornMs<MediaWait) {
            uint8_t raw[128];size_t length=0;
            const auto token=0x80000000u|(++_sequence);
            if(rs_handheld_voice_packet_encode(nullptr,0,2,packet.bytes,packet.length,raw,sizeof raw,&length)!=RS_HANDHELD_OK ||
                !_d.links->sendVoice(_link,raw,length,token,packet.bornMs,MediaWait)) mediaOutcome(token,false);
        }
    }
    if(_closing) {
        if((!_signalCount && !_controlToken) || now()>=_closeAt) {
            if(_link.valid()) {const auto old=_link;_link={};_d.links->closeVoice(old);}
            _signalCount=0;_controlToken=0;_identify=false;
            if(!_d.audio || _d.audio->drained()) {_sessionLive=false;_closing=false;_status.phase=Phase::Ended;std::memset(_session,0,sizeof _session);std::memset(_publicKey,0,sizeof _publicKey);}
        }
    }
    if(active(_status.phase)) _status.elapsedMs=uint32_t(now()-_born);
}
void RustVoiceEngine::onVoiceLink(RustLinkManager::Handle h,bool incoming,uint8_t iface,uint8_t hops) {
    if(incoming) {
        if(!admitVoice(iface,hops) || _status.generation==UINT32_MAX) {_d.links->closeVoice(h);return;}
        const auto generation=_status.generation+1;const auto capabilities=_status.capabilities;
        _status={};_status.generation=generation;_status.capabilities=uint8_t(capabilities);_status.volume=_settings.volume;
        _born=now();_sequence=0;
        if(!initialise(true,iface,hops)) {_d.links->closeVoice(h);return;}
        _link=h;
    }
    if(!same(h)) return;
    apply(RS_HANDHELD_VOICE_LINK_READY);
}
void RustVoiceEngine::onVoiceIdentity(RustLinkManager::Handle h,const uint8_t publicKey[64]) {
    if(!same(h) || _status.verified || _closing) return;
    uint8_t peer[16];
    if(rs_handheld_voice_peer_destination(publicKey,peer)!=RS_HANDHELD_OK) {terminate(Code::IdentityUnknown);return;}
    if(_status.incoming && _settings.contactsOnly && (!_d.contact || !_d.contact(_d.contactContext,peer))) {terminate(Code::NotContact);return;}
    if(!_status.incoming && std::memcmp(publicKey,_publicKey,64)) {terminate(Code::IdentityUnknown);return;}
    std::memcpy(_status.peer,peer,16);_status.verified=true;
    apply(RS_HANDHELD_VOICE_PEER_VERIFIED);
}
void RustVoiceEngine::onVoicePacket(RustLinkManager::Handle h,const uint8_t* bytes,size_t length) {
    if(!same(h) || _closing) return;
    rs_handheld_voice_packet_t packet{};
    if(rs_handheld_voice_packet_view(bytes,length,&packet)!=RS_HANDHELD_OK) return;
    for(uint32_t n=0;n<packet.signal_count && !_closing;++n) apply(RS_HANDHELD_VOICE_SIGNAL,packet.signals[n]);
    if(_closing || !_status.verified) return;
    for(uint32_t n=0;n<packet.frame_count;++n) {
        const auto frame=packet.frames[n];
        const auto shape=profileInfo(_status.profile);
        const size_t expected=shape.packet_bytes;
        if(frame.codec!=2 || frame.length!=expected || frame.offset+frame.length>length || bytes[frame.offset]!=shape.mode) {++_status.rxDrops;continue;}
        apply(RS_HANDHELD_VOICE_PROFILE_MEDIA,_status.profile);
        if(!(_result.flags&4)) continue;
        Encoded media;media.generation=_result.audio_generation;media.bornMs=now();media.length=frame.length;
        std::memcpy(media.bytes,bytes+frame.offset,frame.length);
        if(!_d.audio->receive(media)) ++_status.rxDrops;
        _lastInbound=now();
    }
}
void RustVoiceEngine::onVoiceClosed(RustLinkManager::Handle h) {if(same(h)) {_link={};terminate(Code::RouteLost);}}
void RustVoiceEngine::onVoiceTransmit(RustLinkManager::Handle h,uint32_t token,bool started) {
    if(!same(h)) return;
    if(token&0x80000000u) {mediaOutcome(token,started);return;}
    if(_controlToken==token) {_controlToken=0;if(!started && !_closing)terminate(Code::RouteLost);}
}
void RustVoiceEngine::mediaOutcome(uint32_t token,bool started) {
    // A rejected offer may deliver an inline receipt before returning false.
    // Each token contributes once; deliberate input cancellation is not congestion.
    if(token==_lastMediaOutcome) return;
    _lastMediaOutcome=token;
    if(started) {_mediaFailures=0;return;}
    ++_status.txDrops;
    if(voiceMediaEligible(_link,token) && ++_mediaFailures>=3) terminate(Code::ChannelBusy);
}
void RustVoiceEngine::stop() {_accepting=false;if(active(_status.phase))terminate(Code::Local);if(_d.ctx)rs_handheld_voice_enable(_d.ctx,0);}
bool RustVoiceEngine::drained() const {return !active(_status.phase) && (!_d.audio || _d.audio->drained());}
void RustVoiceEngine::end() {stop();if(_d.links)_d.links->setVoiceSink(nullptr);}

bool RustVoiceEngine::voiceMediaEligible(RustLinkManager::Handle h,uint32_t) const {
    return same(h) && !_closing && (_result.flags&2) && _view && _d.audio && _inputEpoch==_d.audio->cancellationEpoch();
}
