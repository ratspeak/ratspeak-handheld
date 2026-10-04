#include "voice/AudioDevice.h"
#include "config/Config.h"
#include <Arduino.h>
#include <cstring>
#if defined(RSCARDPUTER)
#include <M5Unified.h>
#elif defined(RSDECK) || defined(RATPAGER)
#include <Wire.h>
#include <driver/i2s.h>
#include <freertos/queue.h>
#ifdef RSDECK
#include "../../boards/tdeck/audio/es7210/es7210.h"
#else
#include "hal/Power.h"
#endif
#endif

namespace handheld::voice {
#if defined(RSDECK) || defined(RATPAGER)
namespace {
bool install(bool input, void*& events) {
#ifdef RSDECK
    const auto port=input?I2S_NUM_1:I2S_NUM_0;
#else
    const auto port=I2S_NUM_0;
#endif
    i2s_config_t cfg{};
    cfg.mode=i2s_mode_t(I2S_MODE_MASTER|(input?I2S_MODE_RX:I2S_MODE_TX));
    cfg.sample_rate=16000;cfg.bits_per_sample=I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format=I2S_CHANNEL_FMT_ONLY_LEFT;cfg.communication_format=I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags=ESP_INTR_FLAG_LEVEL1;cfg.dma_buf_count=input?8:4;cfg.dma_buf_len=160;
    cfg.tx_desc_auto_clear=true;cfg.mclk_multiple=I2S_MCLK_MULTIPLE_256;
    QueueHandle_t queue=nullptr;
    if(i2s_driver_install(port,&cfg,input?32:0,input?&queue:nullptr)!=ESP_OK) return false;
    events=queue;
    i2s_pin_config_t pins{};
#ifdef RSDECK
    pins.mck_io_num=input?48:I2S_PIN_NO_CHANGE;
    pins.bck_io_num=input?47:I2S_BCK;pins.ws_io_num=input?21:I2S_WS;
    pins.data_in_num=input?14:I2S_PIN_NO_CHANGE;
#else
    pins.mck_io_num=I2S_MCLK;pins.bck_io_num=I2S_BCK;pins.ws_io_num=I2S_WS;
    pins.data_in_num=input?I2S_DIN:I2S_PIN_NO_CHANGE;
#endif
    pins.data_out_num=input?I2S_PIN_NO_CHANGE:I2S_DOUT;
    if(i2s_set_pin(port,&pins)!=ESP_OK) {i2s_driver_uninstall(port);events=nullptr;return false;}
    i2s_zero_dma_buffer(port);return true;
}
#ifdef RATPAGER
bool reg(uint8_t address,uint8_t value) {
    Wire.beginTransmission(0x18);Wire.write(address);Wire.write(value);return Wire.endTransmission()==0;
}
bool codec(bool capture) {
    // Same 16 kHz / 256*MCLK register contract as the board's qualified
    // notification owner. Only the active direction is powered/unmuted.
    const uint8_t setup[][2]={{0x00,0x80},{0x01,0x3f},{0x02,0x00},{0x03,0x10},{0x04,0x20},
        {0x05,0x00},{0x06,0x03},{0x07,0x00},{0x08,0xff},{0x09,0x0c},{0x0a,0x0c},
        {0x0b,0x00},{0x0c,0x00},{0x0d,0x01},{0x10,0x1f},{0x11,0x7f},{0x12,0x00},
        {0x13,0x10},{0x14,0x1a},{0x15,0x40},{0x16,0x24},{0x17,0xbf},{0x1b,0x0a},
        {0x1c,0x6a},{0x32,0xbf},{0x37,0x08},{0x44,0x58},{0x45,0x00}};
    bool ok=true;for(const auto& r:setup) ok=reg(r[0],r[1])&&ok;
    ok=reg(0x0e,capture?0x02:0x6a)&&ok;
    ok=reg(0x31,capture?0x60:0x00)&&ok;
    return Power::setSpeakerPower(!capture)&&ok;
}
#endif
}
#endif
bool AudioDevice::prepare(uint8_t value, bool record) {
    _volume=value;_capture=false;_rate.reset();_error=Code::AudioUnavailable;
#if defined(RSCARDPUTER)
    M5.Mic.end();M5.Speaker.end();
    auto cfg=M5.Speaker.config();cfg.sample_rate=16000;cfg.dma_buf_count=4;cfg.dma_buf_len=160;M5.Speaker.config(cfg);
    if(record) {_ready=true;return capture(true);}
    _ready=_tx=M5.Speaker.begin();M5.Speaker.setVolume(255);
#elif defined(RSDECK)
    Wire.beginTransmission(ES7210_ADDR);
    if(Wire.endTransmission()!=0) return false;
    if(record) {_ready=true;return capture(true);}
    _ready=_tx=install(false,_events);
#elif defined(RATPAGER)
    if(record) {_ready=true;return capture(true);}
    _tx=install(false,_events);_ready=_tx&&codec(false);
#else
    _ready=false;
#endif
    return _ready;
}
bool AudioDevice::capture(bool enabled) {
    if(!_ready) return false;
    if(enabled==_capture) return true;
    // Drain/stop one clock owner before enabling the other; no duplex assumption.
#if defined(RSCARDPUTER)
    if(_rx) {M5.Mic.end();_rx=false;}
    if(_tx) {M5.Speaker.stop();M5.Speaker.end();_tx=false;}
    if(enabled) {
        auto cfg=M5.Mic.config();cfg.sample_rate=16000;cfg.over_sampling=1;cfg.dma_buf_count=8;cfg.dma_buf_len=160;M5.Mic.config(cfg);
        _rx=M5.Mic.begin();_ready=_rx;
        _readBuffer=_readHalf=0;std::memset(_io,0,sizeof _io);
        if(_ready) _ready=M5.Mic.record(_io,640,16000,false) && M5.Mic.record(_io+640,640,16000,false);
        // The vendor task publishes its running flag just after selecting the
        // first buffer. Wait for that publication before treating zero as a gap.
        if(_ready) {
            const uint32_t started=millis();
            while(!M5.Mic.isRecording() && millis()-started<100) delay(1);
            _ready=M5.Mic.isRecording()!=0;
        }
    } else {_tx=M5.Speaker.begin();M5.Speaker.setVolume(255);_ready=_tx;}
#elif defined(RSDECK)
    if(_rx) {es7210_adc_ctrl_state(AUDIO_HAL_CODEC_MODE_ENCODE,AUDIO_HAL_CTRL_STOP);i2s_stop(I2S_NUM_1);i2s_driver_uninstall(I2S_NUM_1);_rx=false;}
    if(_tx) {i2s_stop(I2S_NUM_0);i2s_driver_uninstall(I2S_NUM_0);_tx=false;}
    if(enabled) {
        audio_hal_codec_config_t cfg{};cfg.adc_input=AUDIO_HAL_ADC_INPUT_ALL;cfg.codec_mode=AUDIO_HAL_CODEC_MODE_ENCODE;
        cfg.i2s_iface.mode=AUDIO_HAL_MODE_SLAVE;cfg.i2s_iface.fmt=AUDIO_HAL_I2S_NORMAL;
        cfg.i2s_iface.samples=AUDIO_HAL_16K_SAMPLES;cfg.i2s_iface.bits=AUDIO_HAL_BIT_LENGTH_16BITS;
        _rx=install(true,_events);
        _ready=_rx && es7210_adc_init(&Wire,&cfg)==ESP_OK && es7210_adc_config_i2s(cfg.codec_mode,&cfg.i2s_iface)==ESP_OK &&
            es7210_adc_set_gain(es7210_input_mics_t(ES7210_INPUT_MIC1|ES7210_INPUT_MIC2),GAIN_0DB)==ESP_OK &&
            es7210_adc_set_gain(es7210_input_mics_t(ES7210_INPUT_MIC3|ES7210_INPUT_MIC4),GAIN_37_5DB)==ESP_OK &&
            es7210_adc_ctrl_state(cfg.codec_mode,AUDIO_HAL_CTRL_START)==ESP_OK;
    } else {_events=nullptr;_ready=_tx=install(false,_events);}
#elif defined(RATPAGER)
    Power::setSpeakerPower(false);reg(0x31,0x60);reg(0x0e,0x6a);
    if(_rx || _tx) {i2s_stop(I2S_NUM_0);i2s_driver_uninstall(I2S_NUM_0);_rx=_tx=false;}
    _events=nullptr;
    const bool installed=install(enabled,_events);_rx=enabled&&installed;_tx=!enabled&&installed;
    _ready=installed&&codec(enabled);
#else
    return false;
#endif
    _capture=enabled;_rate.reset();
#if defined(RSDECK) || defined(RATPAGER)
    if(enabled && _ready) {
#ifdef RSDECK
        const auto port=I2S_NUM_1;
#else
        const auto port=I2S_NUM_0;
#endif
        // Codec configuration can outlast the RX queue. Discard only this
        // startup pre-roll before publishing capture, then observe every event.
        size_t bytes=0;
        for(unsigned n=0;n<16;++n) {bytes=0;if(i2s_read(port,_io,sizeof _io,&bytes,0)!=ESP_OK || !bytes)break;}
        if(_events)xQueueReset(static_cast<QueueHandle_t>(_events));
    }
    std::memset(_io,0,sizeof _io);
#endif
    return _ready;
}
bool AudioDevice::inputHealthy() {
#if defined(RSDECK) || defined(RATPAGER)
    i2s_event_t event{};
    while(_events && xQueueReceive(static_cast<QueueHandle_t>(_events),&event,0)==pdTRUE) {
        if(event.type==I2S_EVENT_RX_Q_OVF || event.type==I2S_EVENT_DMA_ERROR) _error=Code::CaptureOverflow;
    }
#endif
    return _error!=Code::CaptureOverflow;
}
size_t AudioDevice::read(int16_t* pcm,size_t samples) {
    if(!_ready || !_capture || samples!=160) return 0;
    size_t bytes=0;
#if defined(RSCARDPUTER)
    if(!_readHalf) {
        const uint32_t started=millis();
        while(M5.Mic.isRecording()>1 && millis()-started<100) delay(1);
        const auto queued=M5.Mic.isRecording();
        // Both buffers drained means capture ran out of destinations. The M5
        // task resets its input cursor after idling, so never publish a gapped clip.
        if(!queued) {_error=Code::CaptureOverflow;return 0;}
        if(queued>1) return 0;
    }
    auto* buffer=_io+_readBuffer*640;
    _rate.down(buffer+_readHalf*320,pcm,160);
    if(++_readHalf==2) {
        if(!M5.Mic.isRecording()) {_error=Code::CaptureOverflow;return 0;}
        std::memset(buffer,0,640*sizeof(int16_t));
        if(!M5.Mic.record(buffer,640,16000,false)) return 0;
        _readHalf=0;_readBuffer^=1;
    }
    return samples;
#elif defined(RSDECK)
    if(!inputHealthy()) return 0;
    if(i2s_read(I2S_NUM_1,_io,640,&bytes,pdMS_TO_TICKS(40))!=ESP_OK) return 0;
#elif defined(RATPAGER)
    if(!inputHealthy()) return 0;
    if(i2s_read(I2S_NUM_0,_io,640,&bytes,pdMS_TO_TICKS(40))!=ESP_OK) return 0;
#endif
    if(bytes!=640 || !inputHealthy()) return 0;
    _rate.down(_io,pcm,160);
    std::memset(_io,0,sizeof _io);return samples;
}
size_t AudioDevice::write(const int16_t* pcm,size_t samples) {
    if(!_ready || _capture || samples!=160) return 0;
    _rate.up(pcm,_io,samples,_volume);
#if defined(RSCARDPUTER)
    if(!M5.Speaker.playRaw(_io,320,16000,false,1,0,true)) return 0;
    const uint32_t started=millis();while(M5.Speaker.isPlaying(0) && millis()-started<40) delay(1);
    if(M5.Speaker.isPlaying(0)) {M5.Speaker.stop();return 0;}
    return samples;
#elif defined(RSDECK) || defined(RATPAGER)
    size_t bytes=0;
    if(i2s_write(I2S_NUM_0,_io,640,&bytes,pdMS_TO_TICKS(40))!=ESP_OK || bytes!=640) return 0;
    return samples;
#else
    return 0;
#endif
}
void AudioDevice::flush() {
#if defined(RSCARDPUTER)
    if(_tx) M5.Speaker.stop();
#elif defined(RSDECK) || defined(RATPAGER)
    if(_tx) {i2s_stop(I2S_NUM_0);i2s_zero_dma_buffer(I2S_NUM_0);i2s_start(I2S_NUM_0);}
#endif
    _rate.reset();
}
void AudioDevice::end() {
#if defined(RSCARDPUTER)
    if(_rx) M5.Mic.end();
    if(_tx) {M5.Speaker.stop();M5.Speaker.end();}
#elif defined(RSDECK)
    if(_rx) {es7210_adc_ctrl_state(AUDIO_HAL_CODEC_MODE_ENCODE,AUDIO_HAL_CTRL_STOP);i2s_stop(I2S_NUM_1);i2s_driver_uninstall(I2S_NUM_1);}
    if(_tx) {i2s_stop(I2S_NUM_0);i2s_driver_uninstall(I2S_NUM_0);}
#elif defined(RATPAGER)
    reg(0x31,0x60);reg(0x0e,0x6a);Power::setSpeakerPower(false);
    if(_rx || _tx) {i2s_stop(I2S_NUM_0);i2s_driver_uninstall(I2S_NUM_0);}
#endif
    _rx=_tx=_ready=_capture=false;_events=nullptr;std::memset(_io,0,sizeof _io);_rate.reset();
}
}
