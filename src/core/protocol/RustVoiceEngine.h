#pragma once
#include "protocol/RustLinkManager.h"
#include "voice/VoiceTypes.h"
class RustClock;
class RustKeyMap;
class RustInterfacePump;

// One serialized telephone session. No PCM, codec or driver work on this owner.
class RustVoiceEngine final : public RustLinkManager::VoiceSink {
public:
    struct Deps {
        rs_handheld_rns_t* ctx=nullptr;
        RustClock* clock=nullptr;
        RustInterfacePump* pump=nullptr;
        RustLinkManager* links=nullptr;
        RustKeyMap* keys=nullptr;
        handheld::voice::AudioPort* audio=nullptr;
        void* contactContext=nullptr;
        bool (*contact)(void*, const uint8_t[16])=nullptr;
    };
    void begin(const Deps&);
    void configure(const handheld::voice::Settings&);
    handheld::voice::Status status() const { return _status; }
    handheld::voice::Code command(const handheld::voice::Command&, uint32_t cancellationEpoch);
    void loop(uint32_t cancellationEpoch);
    void stop();
    bool drained() const;
    void end();
    bool voiceMediaEligible(RustLinkManager::Handle,uint32_t) const override;
    bool admitVoice(uint8_t,uint8_t) const override;
    void onVoiceLink(RustLinkManager::Handle,bool,uint8_t,uint8_t) override;
    void onVoiceIdentity(RustLinkManager::Handle,const uint8_t[64]) override;
    void onVoicePacket(RustLinkManager::Handle,const uint8_t*,size_t) override;
    void onVoiceClosed(RustLinkManager::Handle) override;
    void onVoiceTransmit(RustLinkManager::Handle,uint32_t,bool) override;
private:
    bool same(RustLinkManager::Handle) const;
    bool initialise(bool incoming,uint8_t iface,uint8_t hops);
    uint32_t allowedProfiles(uint8_t iface,uint8_t hops) const;
    bool profileFits(uint8_t profile,uint8_t iface,uint8_t hops) const;
    void apply(uint32_t operation,uint32_t argument=0,uint32_t extra=0);
    void terminate(handheld::voice::Code);
    void signal(uint32_t);
    void sendControl();
    void findRoute();
    uint64_t now() const;
    void publish();
    void mediaOutcome(uint32_t token,bool started);
    Deps _d;
    handheld::voice::Settings _settings;
    handheld::voice::Status _status;
    handheld::voice::Code _routeFailure=handheld::voice::Code::Timeout;
    RustLinkManager::Handle _link;
    alignas(8) uint8_t _session[160]{};
    rs_handheld_voice_result_t _result{};
    uint8_t _destination[16]{},_publicKey[64]{};
    uint32_t _signals[8]{};
    uint32_t _view=0,_inputEpoch=0,_blockedEpoch=0,_audioReady=0,_controlToken=0,_sequence=0;
    uint32_t _lastMediaOutcome=0,_workerDrops=0;
    uint8_t _signalCount=0,_hops=0,_mediaFailures=0;
    bool _sessionLive=false,_audioWanted=false,_identify=false,_closing=false,_accepting=true,_configured=false;
    uint64_t _born=0,_lastRequest=0,_controlBorn=0,_closeAt=0,_talkBorn=0,_lastInbound=0;
};
