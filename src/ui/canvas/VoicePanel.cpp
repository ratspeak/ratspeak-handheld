#include "VoicePanel.h"
#include "protocol/ProtocolBackend.h"
#include "reticulum/AnnounceManager.h"
#include "storage/Hex.h"
#include "util/DisplayText.h"
#include "Theme.h"
using namespace handheld::memo;
void VoicePanel::begin(ProtocolBackend* backend,AnnounceManager* announces) {
    _backend=backend;_announces=announces;
    _model.begin(this,[](void* context,const Command& c,uint32_t serial) {
        auto& self=*static_cast<VoicePanel*>(context);
        self._model.acknowledge(serial,self._backend?self._backend->memoCommand(c):Code::AudioUnavailable);
        return true;
    });
}
void VoicePanel::poll(bool allowed) {if(_backend)_model.update(_backend->memoStatus(),allowed);}
void VoicePanel::start(const char* hex) {startMessage(hex,0,false);}
void VoicePanel::startMessage(const char* hex,uint32_t counter,bool incoming) {
    uint8_t peer[16];if(hex && handheld::storage::decodeHex(hex,strlen(hex),peer,16)) _model.open(peer,counter,incoming);
}
void VoicePanel::render(M5Canvas& canvas) {
    if(!visible()) return;
    const auto& s=_model.status();
    canvas.fillRect(0,0,240,135,Theme::BG);Theme::useSmallFont(canvas);
    canvas.setTextColor(Theme::ACCENT);canvas.drawString("VOICE MESSAGE",6,5);
    char peer[33];handheld::storage::encodeHex(_model.peer(),16,peer);
    auto name=_announces?_announces->lookupName(peer):std::string();
    if(name.empty()) name=peer;
    if(canvas.textWidth(name.c_str())>228) {
        // Truncate whole UTF-8 scalars to the actual font width, with no retained
        // name copy or extra name cache. The existing lookup owns its bounds.
        const int dots=canvas.textWidth("..");
        while(!name.empty() && canvas.textWidth(name.c_str())+dots>228) {
            size_t at=name.size()-1;
            while(at && handheld::display::continuation(uint8_t(name[at]))) --at;
            name.resize(at);
        }
        name+="..";
    }
    canvas.setTextColor(Theme::TEXT_SECONDARY);canvas.drawString(name.c_str(),6,19);
    canvas.setTextColor(s.phase==Phase::Recording?Theme::ERROR:Theme::TEXT_PRIMARY);
    canvas.drawString(_model.text(),6,36);
    const auto ms=(s.phase==Phase::Recording || s.phase==Phase::Playing || s.phase==Phase::Stopping)?s.frames*40:uint32_t(s.length)*10;
    const bool running=s.phase==Phase::Recording || s.phase==Phase::Playing || s.phase==Phase::Stopping;
    char detail[48];snprintf(detail,sizeof detail,"0:%02lu%s   Volume %u%%",(unsigned long)((ms+(running?0:999))/1000),
        s.phase==Phase::Recording?" / 0:15":"",s.volume);
    if(s.reason==Code::UnsupportedAudio || s.reason==Code::AudioUnavailable) snprintf(detail,sizeof detail,"Volume %u%%",s.volume);
    canvas.setTextColor(Theme::TEXT_SECONDARY);canvas.drawString(detail,6,51);
    canvas.drawString(_model.guidance(),6,66);
    const int count=int(_model.count()),width=(228-(count-1)*4)/(count?count:1);
    for(int i=0;i<count;++i) {
        const bool selected=unsigned(i)==_model.focus();
        const auto choice=_model.choice(i);
        const int x=6+i*(width+4);
        canvas.fillRoundRect(x,83,width,21,3,selected?Theme::PRIMARY_SUBTLE:Theme::BG_SURFACE);
        canvas.drawRoundRect(x,83,width,21,3,selected?Theme::PRIMARY:Theme::BORDER);
        canvas.setTextColor(choice==Ui::Choice::Stop || choice==Ui::Choice::ConfirmDiscard || choice==Ui::Choice::ConfirmReplace?Theme::ERROR:selected?Theme::PRIMARY:Theme::TEXT_PRIMARY);
        const auto* label=Ui::label(choice);
        canvas.drawString(label,x+(width-int(strlen(label))*6)/2,90);
    }
    canvas.setTextColor(Theme::TEXT_SECONDARY);
    if(count) {
        canvas.drawString("Arrows: choose  Enter: select",6,111);
        canvas.drawString("Esc: back      +/-: volume",6,123);
    }
}
bool VoicePanel::handleKey(const KeyEvent& e) {
    if(!visible()) return false;
    if(e.repeat) return true;
    if(e.escape || e.backspace) _model.back();
    else if(e.navPrevious()) _model.move(-1);
    else if(e.navNext() || e.tab) _model.move(1);
    else if(e.enter) _model.choose(_model.choice(_model.focus()));
    else if(e.character=='+') _model.volume(10);
    else if(e.character=='-') _model.volume(-10);
    return true;
}
