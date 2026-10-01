#include "VoicePanel.h"
#include "protocol/ProtocolBackend.h"
#include "storage/Hex.h"
#include "Theme.h"
using namespace handheld::voice;
void VoicePanel::begin(ProtocolBackend* backend) {
    _backend=backend;
    _model.begin(this,[](void* context,const Command& c) {
        auto& self=*static_cast<VoicePanel*>(context);
        const auto code=self._backend?self._backend->voiceCommand(c):Code::Off;
        if(code!=Code::Ok) self._model.failed(code);
        return true; // Synchronous failure is already displayed precisely.
    });
}
void VoicePanel::poll(bool foregroundAllowed) {if(_backend)_model.update(_backend->voiceStatus(),foregroundAllowed);}
void VoicePanel::start(const char* hex) {
    uint8_t peer[16];if(hex && handheld::storage::decodeHex(hex,strlen(hex),peer,16)) _model.start(peer);
}
void VoicePanel::render(M5Canvas& canvas) {
    const auto& s=_model.status();
    if(!visible()) {
        if(active()) {canvas.fillRect(110,0,130,14,Theme::PRIMARY_SUBTLE);canvas.setTextColor(Theme::PRIMARY);canvas.drawString("Voice  Ctrl+V",116,2);}
        return;
    }
    canvas.fillRect(0,0,240,135,Theme::BG);Theme::useSmallFont(canvas);
    canvas.setTextColor(Theme::ACCENT);canvas.drawString("LIVE VOICE",6,5);
    char peer[33];handheld::storage::encodeHex(_model.peer(),16,peer);
    canvas.setTextColor(Theme::TEXT_SECONDARY);canvas.drawString(peer,6,21);
    canvas.setTextColor(s.phase==Phase::Talking?Theme::PRIMARY:Theme::TEXT_PRIMARY);
    auto lines=[&](const char* text,int y) {
        char line[39]{};strncpy(line,text,38);canvas.drawString(line,6,y);
        if(strlen(text)>38) {strncpy(line,text+38,38);line[38]=0;canvas.drawString(line,6,y+10);}
    };
    lines(_model.text(),39);
    const bool ready=_model.ready() && !_model.pending();
    char detail[48]{};
    if(ready && s.iface!=UINT8_MAX) snprintf(detail,sizeof detail,"%s  %lu:%02lu  Volume %u%%",
        s.iface==0?"LoRa":"WiFi/TCP",(unsigned long)(s.elapsedMs/60000),(unsigned long)(s.elapsedMs/1000%60),s.volume);
    canvas.setTextColor(Theme::TEXT_SECONDARY);
    if(ready) canvas.drawString(detail,6,65);else lines(_model.guidance(),65);
    if(s.phase==Phase::Incoming) {
        canvas.setTextColor(Theme::PRIMARY);canvas.drawString("Enter: accept",6,96);
        canvas.setTextColor(Theme::TEXT_SECONDARY);canvas.drawString("Esc: decline",6,113);
    } else if(ready) {
        canvas.fillRoundRect(4,81,232,15,3,Theme::PRIMARY_SUBTLE);
        canvas.setTextColor(Theme::PRIMARY);
        canvas.drawString((s.capabilities&1)?(s.iface==0?"Hold Space: talk (up to 10s)":"Hold Space: talk (up to 30s)"):"Listen only: no microphone",8,84);
        canvas.setTextColor(Theme::TEXT_SECONDARY);
        canvas.drawString("E: end   +/-: volume",6,101);canvas.drawString("Esc: back   Ctrl+V: return",6,117);
    } else if(active()) {
        canvas.drawString("E: cancel   Esc: back",6,113);
    } else {
        if(_model.canRetry()) {canvas.setTextColor(Theme::PRIMARY);canvas.drawString("Enter: retry voice",6,101);}
        canvas.setTextColor(Theme::TEXT_SECONDARY);canvas.drawString("Esc: back",6,117);
    }
}
bool VoicePanel::handleKey(const KeyEvent& e) {
    if(!visible()) {if(active() && e.ctrl && (e.character=='v' || e.character=='V')) {show();return true;}return false;}
    if(e.repeat) return true;
    const auto phase=_model.status().phase;
    if(e.escape || e.backspace) {if(phase==Phase::Incoming)_model.action(Action::Decline);else hide();}
    else if(e.enter) {if(phase==Phase::Incoming)_model.action(Action::Accept);else if(_model.canRetry())_model.retry();else if(!handheld::voice::active(phase))hide();}
    else if(e.character=='e' || e.character=='E') _model.action(Action::End);
    else if(e.character=='+') _model.volume(10);
    else if(e.character=='-') _model.volume(-10);
    return true;
}
