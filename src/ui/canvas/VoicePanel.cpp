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
    char peer[33];handheld::storage::encodeHex(s.peer,16,peer);
    canvas.setTextColor(Theme::TEXT_SECONDARY);canvas.drawString(peer,6,21);
    canvas.setTextColor(s.phase==Phase::Talking?Theme::ERROR:Theme::TEXT_PRIMARY);
    const char* text=_model.text();
    char line[38]{};strncpy(line,text,37);canvas.drawString(line,6,39);
    if(strlen(text)>37) canvas.drawString(text+37,6,49);
    char detail[48];snprintf(detail,sizeof detail,"%s  %lus  Vol %u%%",s.iface==0?"LoRa":"WiFi/TCP",(unsigned long)(s.elapsedMs/1000),s.volume);
    canvas.setTextColor(Theme::TEXT_SECONDARY);canvas.drawString(active()?detail:"",6,65);
    if(s.phase==Phase::Incoming) {canvas.drawString("Enter: accept",6,86);canvas.drawString("Esc: decline",6,102);}
    else if(active()) {
        canvas.drawString(s.iface==0?"Hold Space: talk (max 10s)":"Hold Space: talk (max 30s)",6,85);
        canvas.drawString("E: end   +/-: volume",6,101);canvas.drawString("Esc: back   Ctrl+V: return",6,117);
    } else canvas.drawString("Enter / Esc: close",6,102);
}
bool VoicePanel::handleKey(const KeyEvent& e) {
    if(!visible()) {if(active() && e.ctrl && (e.character=='v' || e.character=='V')) {show();return true;}return false;}
    if(e.repeat) return true;
    const auto phase=_model.status().phase;
    if(e.escape || e.backspace) {if(phase==Phase::Incoming)_model.action(Action::Decline);else hide();}
    else if(e.enter) {if(phase==Phase::Incoming)_model.action(Action::Accept);else if(!handheld::voice::active(phase))hide();}
    else if(e.character=='e' || e.character=='E') _model.action(Action::End);
    else if(e.character=='+') _model.volume(10);
    else if(e.character=='-') _model.volume(-10);
    return true;
}
