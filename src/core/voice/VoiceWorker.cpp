#ifdef ARDUINO
#include "voice/VoiceWorker.h"
#include "voice/VoiceProfile.h"
#include "voice/AudioDevice.h"
#include "voice/AudioCoordinator.h"
#include "runtime/ResourceBudget.h"
#include "ratspeak_protocol.h"
#include "config/Config.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <new>
#include <cstring>

namespace handheld::voice {
namespace {
constexpr uint32_t StackBytes=24576, DriverAllowance=8192, StackReserve=6144, PcmBytes=640;
#ifdef RSCARDPUTER
constexpr size_t FreeFloor=ResourceBudget::CardInternalFree, LargestFloor=ResourceBudget::CardLargestBlock;
#else
constexpr size_t FreeFloor=ResourceBudget::LargeInternalFree, LargestFloor=ResourceBudget::LargeLargestBlock;
#endif
bool memorySafe() {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)>=FreeFloor &&
           heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)>=LargestFloor;
}

uint64_t nowMs() { return uint64_t(esp_timer_get_time())/1000; }
void clear(void* p,size_t n) { volatile uint8_t* b=static_cast<uint8_t*>(p);while(n--) *b++=0; }
}
struct VoiceWorker::Impl {
    portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
    std::atomic<bool> stop{false}, done{false}, talk{false};
    std::atomic<uint32_t> talkEpoch{0}, flushEpoch{0};
    std::atomic<uint8_t> volume{70};
    AudioDevice device;
    AudioStatus status;
    Encoded rx[3], tx[3];
    uint8_t rxHead=0,rxCount=0,txHead=0,txCount=0;
    uint8_t profile=0;
    uint32_t generation=0;
    uint8_t* codec=nullptr;
    int16_t* pcm=nullptr;
    TaskHandle_t task=nullptr;
    bool transmitting() const {
        if(talk.load() && uint32_t(nowMs())-VoiceWorker::_inputHeartbeat.load()>250) VoiceWorker::emergencyStop();
        return talk.load() && talkEpoch.load()==VoiceWorker::_stopEpoch.load() && talkEpoch.load()!=UINT32_MAX && !stop.load(); }
    void update(bool ready, bool capturing, bool receiving, Code error=Code::Ok) {
        const auto stackFree=uxTaskGetStackHighWaterMark(nullptr);
        portENTER_CRITICAL(&mux);
        status.ready=ready; status.capturing=capturing;status.receiving=receiving;status.error=error;
        status.stackFree=stackFree;
        portEXIT_CRITICAL(&mux);
    }
    void queues() {
        portENTER_CRITICAL(&mux);
        clear(rx,sizeof rx);clear(tx,sizeof tx);rxHead=rxCount=txHead=txCount=0;
        portEXIT_CRITICAL(&mux);
    }
};
void VoiceWorker::keepInputAlive() { _inputHeartbeat=uint32_t(nowMs()); }
uint8_t VoiceWorker::capabilities() const {
#if defined(RSDECK) || defined(RATPAGER) || defined(RSCARDPUTER)
    return 3;
#else
    return 0;
#endif
}
Code VoiceWorker::prepare(uint32_t generation,uint8_t profile,uint8_t volumeValue) {
    if (!capabilities()) return Code::AudioUnavailable;
    if (!generation || !profileInfo(profile).native_samples) return Code::ProfileUnsupported;
    if (_impl && !_impl->stop.load() && _impl->generation==generation && _impl->profile==profile) return Code::Ok;
    if (_impl) { stop(); if(!drained()) return Code::Busy; }
#ifdef RSCARDPUTER
    const uint32_t bufferCaps=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT;
    constexpr size_t floor=ResourceBudget::CardInternalFree, largest=ResourceBudget::CardLargestBlock;
#else
    const uint32_t bufferCaps=MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT;
    constexpr size_t floor=ResourceBudget::LargeInternalFree, largest=ResourceBudget::LargeLargestBlock;
#endif
    const auto codecBytes=rs_handheld_voice_codec_size();
    const size_t retained=codecBytes+PcmBytes;
    const size_t internal=StackBytes+DriverAllowance+sizeof(Impl)+(bufferCaps & MALLOC_CAP_INTERNAL ? retained : 0);
    if (internal>65536 || heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<internal+floor ||
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<StackBytes+1024) return Code::NoMemory;
    if (!(bufferCaps & MALLOC_CAP_INTERNAL) && heap_caps_get_free_size(bufferCaps)<retained+ResourceBudget::PsramFree)
        return Code::NoMemory;
    auto* memory=heap_caps_malloc(sizeof(Impl),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if (!memory) return Code::NoMemory;
    auto* i=new(memory) Impl;
    i->codec=static_cast<uint8_t*>(heap_caps_calloc(1,codecBytes,bufferCaps));
    i->pcm=static_cast<int16_t*>(heap_caps_calloc(320,sizeof(int16_t),bufferCaps));
    if (!i->codec || !i->pcm || reinterpret_cast<uintptr_t>(i->codec)%rs_handheld_voice_codec_align()) {
        heap_caps_free(i->codec);heap_caps_free(i->pcm);i->~Impl();heap_caps_free(i);return Code::NoMemory;
    }
    i->generation=generation;i->status.generation=generation;i->profile=profile;i->volume=volumeValue;
    if (!AudioCoordinator::instance().request()) {
        heap_caps_free(i->codec);heap_caps_free(i->pcm);i->~Impl();heap_caps_free(i);return Code::Busy;
    }
    _impl=i;
    if (xTaskCreate(run,"voice-audio",StackBytes,i,2,&i->task)!=pdPASS) {
        i->stop=true;i->done=true;
        // UI may be between suspend and publication. drained() waits for its handoff.
        drained();return Code::NoMemory;
    }
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<floor+DriverAllowance ||
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<largest) {
        stop();return Code::NoMemory;
    }
    return Code::Ok;
}
void VoiceWorker::stop() { if (_impl) { _impl->talk=false;_impl->stop=true; } }
bool VoiceWorker::drained() {
    auto* i=_impl;
    if (!i) return true;
    if (!i->done.load() || !AudioCoordinator::instance().release()) return false;
    rs_handheld_voice_codec_clear(i->codec);clear(i->pcm,PcmBytes);
    heap_caps_free(i->codec);heap_caps_free(i->pcm);
    i->~Impl();clear(i,sizeof(Impl));heap_caps_free(i);_impl=nullptr;return true;
}
AudioStatus VoiceWorker::status() const {
    if (!_impl) return {};
    portENTER_CRITICAL(&_impl->mux);const auto result=_impl->status;portEXIT_CRITICAL(&_impl->mux);return result;
}
void VoiceWorker::capture(bool value,uint32_t epoch) {
    if (!_impl) return;
    _impl->talkEpoch=epoch;_impl->talk=value;
}
void VoiceWorker::flush() { if(_impl) { ++_impl->flushEpoch;_impl->queues(); } }
void VoiceWorker::volume(uint8_t v) { if(_impl) _impl->volume=v>100?100:v; }
bool VoiceWorker::receive(const Encoded& packet) {
    auto* i=_impl;
    if(!i || packet.generation!=i->generation || packet.length>81 || i->transmitting() || i->stop) return false;
    portENTER_CRITICAL(&i->mux);
    if(i->rxCount==3) { i->rxHead=(i->rxHead+1)%3;--i->rxCount;++i->status.dropped; }
    i->rx[(i->rxHead+i->rxCount)%3]=packet;++i->rxCount;
    portEXIT_CRITICAL(&i->mux);return true;
}
bool VoiceWorker::take(Encoded& packet) {
    auto* i=_impl;if(!i) return false;
    portENTER_CRITICAL(&i->mux);
    const bool have=i->txCount;
    if(have) {packet=i->tx[i->txHead];clear(&i->tx[i->txHead],sizeof packet);i->txHead=(i->txHead+1)%3;--i->txCount;}
    portEXIT_CRITICAL(&i->mux);return have;
}
void VoiceWorker::run(void* pointer) {
    auto& i=*static_cast<Impl*>(pointer);
    bool owned=false, ready=false, capture=false;
    size_t used=0, frames=0;
    const auto shape=profileInfo(i.profile);
    const size_t nativeSamples=shape.native_samples, nativeBytes=shape.native_bytes;
    const size_t packetFrames=shape.packet_frames;
    const uint8_t mode=uint8_t(shape.mode);
    const uint32_t MediaAge=mediaAge(shape);
    uint32_t flush=i.flushEpoch;
    unsigned overruns=0;
    Encoded encoded;encoded.generation=i.generation;encoded.bytes[0]=mode;
    Code failure=Code::Ok;
    const uint64_t start=nowMs();
    auto timing=[&](int64_t started,bool encode) {
        const uint32_t elapsed=uint32_t(esp_timer_get_time()-started);
        portENTER_CRITICAL(&i.mux);
        auto& maximum=encode?i.status.encodeUs:i.status.decodeUs;
        if(elapsed>maximum) maximum=elapsed;
        portEXIT_CRITICAL(&i.mux);
        overruns=elapsed>nativeSamples*125/2?overruns+1:0;
        return elapsed<=nativeSamples*125 && overruns<3;
    };
    while(!i.stop && !owned) {
        owned=AudioCoordinator::instance().claim();
        if(!owned && nowMs()-start>=2000) {failure=Code::AudioUnavailable;break;}
        if(!owned) vTaskDelay(1);
    }
    if(owned && !i.stop) {
        ready=rs_handheld_voice_codec_init(i.codec,rs_handheld_voice_codec_size(),i.profile)==RS_HANDHELD_OK && i.device.prepare(i.volume);
        if(!ready) failure=Code::AudioUnavailable;
    }
    if(ready && !memorySafe()) {ready=false;failure=Code::NoMemory;}
    i.update(ready,false,false,failure);
    while(ready && !i.stop) {
        if(!memorySafe() || uxTaskGetStackHighWaterMark(nullptr)<StackReserve) {failure=Code::NoMemory;break;}
        const auto epoch=i.flushEpoch.load();
        if(epoch!=flush) {flush=epoch;used=frames=0;clear(i.pcm,PcmBytes);clear(encoded.bytes+1,80);i.device.flush();}
        const bool talk=i.transmitting();
        if(talk!=capture) {
            if(!i.device.capture(talk)) {failure=Code::AudioUnavailable;break;}
            if(!memorySafe()) {failure=Code::NoMemory;break;}
            capture=talk;used=frames=0;clear(i.pcm,PcmBytes);clear(encoded.bytes+1,80);i.update(true,capture,false);
        }
        i.device.volume(i.volume);
        if(capture) {
            if(!used && !frames) encoded.bornMs=nowMs();
            const size_t got=i.device.read(i.pcm+used,160);
            if(got!=160) {failure=Code::AudioUnavailable;break;}
            if(!i.transmitting() || flush!=i.flushEpoch) {used=frames=0;clear(i.pcm,PcmBytes);continue;}
            used+=got;
            if(used==nativeSamples) {
                const auto started=esp_timer_get_time();
                const auto code=rs_handheld_voice_codec_encode_frame(i.codec,i.pcm,nativeSamples,encoded.bytes+1+nativeBytes*frames,nativeBytes);
                const bool timely=timing(started,true);
                used=0;clear(i.pcm,PcmBytes);
                if(code!=RS_HANDHELD_OK || !timely) {failure=timely?Code::AudioUnavailable:Code::SlowCodec;break;}
                ++frames;
                if(frames==packetFrames) {
                    encoded.length=uint16_t(1+nativeBytes*frames);frames=0;
                    if(i.transmitting() && flush==i.flushEpoch && nowMs()-encoded.bornMs<MediaAge) {
                        portENTER_CRITICAL(&i.mux);
                        if(i.txCount==3) {i.txHead=(i.txHead+1)%3;--i.txCount;++i.status.dropped;}
                        i.tx[(i.txHead+i.txCount)%3]=encoded;++i.txCount;
                        portEXIT_CRITICAL(&i.mux);
                    } else {
                        portENTER_CRITICAL(&i.mux);++i.status.dropped;portEXIT_CRITICAL(&i.mux);
                    }
                    clear(encoded.bytes+1,80);
                }
            }
        } else {
            Encoded packet;bool have=false;
            portENTER_CRITICAL(&i.mux);
            if(i.rxCount) {packet=i.rx[i.rxHead];clear(&i.rx[i.rxHead],sizeof packet);i.rxHead=(i.rxHead+1)%3;--i.rxCount;have=true;}
            portEXIT_CRITICAL(&i.mux);
            if(!have) {i.update(true,false,false);vTaskDelay(1);continue;}
            if(packet.generation!=i.generation || packet.length!=1+packetFrames*nativeBytes || packet.bytes[0]!=mode ||
                nowMs()<packet.bornMs || nowMs()-packet.bornMs>=MediaAge) {clear(&packet,sizeof packet);continue;}
            i.update(true,false,true);
            for(size_t frame=0;frame<packetFrames && !i.stop && !i.transmitting() && flush==i.flushEpoch; ++frame) {
                const auto started=esp_timer_get_time();
                const auto code=rs_handheld_voice_codec_decode_frame(i.codec,packet.bytes+1+frame*nativeBytes,nativeBytes,i.pcm,320);
                const bool timely=timing(started,false);
                if(code!=RS_HANDHELD_OK || !timely) {failure=timely?Code::AudioUnavailable:Code::SlowCodec;break;}
                for(size_t offset=0;offset<nativeSamples && !i.stop && !i.transmitting() && flush==i.flushEpoch && nowMs()-packet.bornMs<MediaAge;offset+=160) {
                    if(i.device.write(i.pcm+offset,160)!=160) {failure=Code::AudioUnavailable;break;}
                }
                clear(i.pcm,PcmBytes);
                if(failure!=Code::Ok || nowMs()-packet.bornMs>=MediaAge) break;
            }
            clear(i.pcm,PcmBytes);clear(&packet,sizeof packet);
            if(failure!=Code::Ok) break;
        }
    }
    // Hardware and sensitive buffers retire before readiness/completion is published.
    if(owned) i.device.end();
    i.queues();clear(i.pcm,PcmBytes);clear(&encoded,sizeof encoded);rs_handheld_voice_codec_clear(i.codec);
    i.update(false,false,false,failure);
    while(!AudioCoordinator::instance().release()) vTaskDelay(1);
    i.done.store(true);
    vTaskDelete(nullptr);
}
}

#endif
