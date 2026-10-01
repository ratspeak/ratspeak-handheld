#include "LvVoicePanel.h"
#include "runtime/ServiceClient.h"
#include "storage/Hex.h"
#include "Theme.h"
#include "LvTheme.h"
#include "fonts/fonts.h"
using namespace handheld::voice;
void LvVoicePanel::begin(handheld::ServiceClient* service) {
    _service=service;
    _model.begin(this,[](void* context,const Command& command) {
        auto& self=*static_cast<LvVoicePanel*>(context);
        const auto generation=self._model.status().generation;
        return self._service && self._service->voiceCommand(command,[&self,generation](const handheld::Result& result) {
            if(result.outcome!=handheld::Outcome::Ok && self._model.status().generation==generation)
                self._model.failed(result.next?Code(result.next):Code::Busy);
        })!=0;
    });
}
void LvVoicePanel::start(const char* hex) {
    if(!_service || !hex) return;
    uint8_t peer[16];if(!handheld::storage::decodeHex(hex,strlen(hex),peer,16)) return;
    _model.start(peer);build();render();
}
void LvVoicePanel::show() {_model.show();build();render();}
void LvVoicePanel::hide() {
    _model.hide();
    if(_root) {lv_obj_del(_root);_root=nullptr;}
}
void LvVoicePanel::action(Action action) {_model.action(action);render();}
void LvVoicePanel::build() {
    if(_root || !_model.visible()) return;
    _root=lv_obj_create(lv_layer_top());lv_obj_set_size(_root,Theme::SCREEN_W,Theme::SCREEN_H);lv_obj_center(_root);
    lv_obj_add_style(_root,LvTheme::styleModal(),0);lv_obj_set_style_pad_all(_root,10,0);lv_obj_clear_flag(_root,LV_OBJ_FLAG_SCROLLABLE);
    auto label=[&](const char* text,int y) {
        auto* l=lv_label_create(_root);lv_obj_set_width(l,Theme::SCREEN_W-24);lv_obj_set_height(l,lv_font_get_line_height(&lv_font_rsdeck_14));lv_obj_set_pos(l,0,y);
        lv_obj_set_style_text_font(l,&lv_font_rsdeck_14,0);lv_obj_set_style_text_color(l,lv_color_hex(Theme::TEXT_PRIMARY),0);
        lv_label_set_long_mode(l,LV_LABEL_LONG_DOT);lv_label_set_text(l,text);return l;
    };
    label("Live voice",0);_peer=label("",23);_label=label("",46);_route=label("",70);
    lv_obj_set_style_text_color(_peer,lv_color_hex(Theme::ACCENT),0);
    lv_obj_set_style_text_font(_route,&lv_font_rsdeck_12,0);
    lv_label_set_long_mode(_route,LV_LABEL_LONG_WRAP);lv_obj_set_height(_route,28);
    lv_obj_set_style_text_color(_route,lv_color_hex(Theme::TEXT_SECONDARY),0);
    auto button=[&](const char* text,int x,int y,int w,int h) {
        auto* b=lv_btn_create(_root);lv_obj_add_style(b,LvTheme::styleBtn(),0);lv_obj_add_style(b,LvTheme::styleBtnPressed(),LV_STATE_PRESSED);lv_obj_set_pos(b,x,y);lv_obj_set_size(b,w,h);
        auto* l=lv_label_create(b);lv_label_set_text(l,text);lv_obj_set_style_text_font(l,&lv_font_rsdeck_14,0);lv_obj_center(l);return b;
    };
    const int width=Theme::SCREEN_W-24;
    _hold=button("Hold to talk",0,100,width,44);_holdLabel=lv_obj_get_child(_hold,0);
    lv_obj_add_event_cb(_hold,[](lv_event_t* e) {
        auto& self=*static_cast<LvVoicePanel*>(lv_event_get_user_data(e));
        const auto code=lv_event_get_code(e);
        if(code==LV_EVENT_PRESSED && self.visible() && self._model.ready()) VoiceInput::instance().edge(VoiceInput::Touch,true);
        else if(code==LV_EVENT_RELEASED || code==LV_EVENT_PRESS_LOST || code==LV_EVENT_DELETE) VoiceInput::instance().edge(VoiceInput::Touch,false);
        else if(code==LV_EVENT_CLICKED && self._model.canRetry()) {self._model.retry();self.render();}
    },LV_EVENT_ALL,this);
    for(auto* primary:{_hold}) {
        lv_obj_set_style_bg_color(primary,lv_color_hex(Theme::PRIMARY_SUBTLE),0);
        lv_obj_set_style_border_color(primary,lv_color_hex(Theme::PRIMARY),0);
    }
    _accept=button("Accept",0,100,width,44);
    lv_obj_set_style_border_color(_accept,lv_color_hex(Theme::PRIMARY),0);
    lv_obj_set_style_bg_color(_accept,lv_color_hex(Theme::PRIMARY_SUBTLE),0);
    lv_obj_add_event_cb(_accept,[](lv_event_t* e) {static_cast<LvVoicePanel*>(lv_event_get_user_data(e))->action(Action::Accept);},LV_EVENT_CLICKED,this);
    _quieter=button("-",0,151,40,32);
    _louder=button("+",46,151,40,32);
    lv_obj_add_event_cb(_quieter,[](lv_event_t* e) {auto& self=*static_cast<LvVoicePanel*>(lv_event_get_user_data(e));self._model.volume(-10);self.render();},LV_EVENT_CLICKED,this);
    lv_obj_add_event_cb(_louder,[](lv_event_t* e) {auto& self=*static_cast<LvVoicePanel*>(lv_event_get_user_data(e));self._model.volume(10);self.render();},LV_EVENT_CLICKED,this);
    _end=button("End",92,151,(width-98)/2,32);_endLabel=lv_obj_get_child(_end,0);
    lv_obj_add_event_cb(_end,[](lv_event_t* e) {
        auto& self=*static_cast<LvVoicePanel*>(lv_event_get_user_data(e));
        self.action(self._model.status().phase==Phase::Incoming?Action::Decline:Action::End);
    },LV_EVENT_CLICKED,this);
    _back=button("Back",92+(width-98)/2+6,151,(width-98)/2,32);
    lv_obj_add_event_cb(_back,[](lv_event_t* e) {static_cast<LvVoicePanel*>(lv_event_get_user_data(e))->hide();},LV_EVENT_CLICKED,this);
    _hint=label("",187);lv_obj_set_style_text_font(_hint,&lv_font_rsdeck_12,0);
}
void LvVoicePanel::render() {
    if(!_root) return;
    const auto& s=_model.status();
    char peer[33];handheld::storage::encodeHex(_model.peer(),16,peer);
    const auto name=_service?_service->nodes.lookupName(peer):std::string();
    lv_label_set_text(_peer,name.empty()?peer:name.c_str());lv_label_set_text(_label,_model.text());
    const bool incoming=s.phase==Phase::Incoming && !_model.pending();
    const bool ready=_model.ready() && !_model.pending();
    const bool live=active(s.phase);
    const bool retry=_model.canRetry();
    const bool volume=ready && (s.capabilities&2);
    char info[80]{};
    if(ready && s.iface!=UINT8_MAX) snprintf(info,sizeof info,"%s  %lu:%02lu  Volume %u%%",
        s.iface==0?"LoRa":"WiFi / TCP",(unsigned long)(s.elapsedMs/60000),(unsigned long)(s.elapsedMs/1000%60),s.volume);
    else snprintf(info,sizeof info,"%s",_model.guidance());
    lv_label_set_text(_route,info);
    auto shown=[](lv_obj_t* object,bool show) {
        if(show) lv_obj_clear_flag(object,LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(object,LV_OBJ_FLAG_HIDDEN);
    };
    shown(_accept,incoming);shown(_hold,(ready && (s.capabilities&1)) || retry);
    shown(_quieter,volume);shown(_louder,volume);shown(_end,live);shown(_back,!incoming);
    lv_label_set_text(_holdLabel,retry?"Retry voice":s.phase==Phase::Talking?"Talking - release to stop":"Hold to talk");
    lv_label_set_text(_endLabel,incoming?"Decline":ready?"End":"Cancel");
    // Outside a live session Back remains a real, full-width exit. During
    // connection/acceptance there are only two actions; volume joins when ready.
    const int width=Theme::SCREEN_W-24;
    const int start=volume?92:0;
    const int actionWidth=live && !incoming?(width-start-6)/2:width;
    lv_obj_set_pos(_end,start,151);lv_obj_set_width(_end,actionWidth);
    lv_obj_set_pos(_back,live?start+actionWidth+6:0,151);lv_obj_set_width(_back,actionWidth);
    if(s.volume==0) lv_obj_add_state(_quieter,LV_STATE_DISABLED);else lv_obj_clear_state(_quieter,LV_STATE_DISABLED);
    if(s.volume==100) lv_obj_add_state(_louder,LV_STATE_DISABLED);else lv_obj_clear_state(_louder,LV_STATE_DISABLED);
#ifdef RATPAGER
    lv_label_set_text(_hint,incoming?"Enter: accept   Esc: decline":ready?
        "Hold Space: talk   E: end   +/-: volume":retry?"Enter: retry   Esc: back":"Esc: back");
#else
    lv_label_set_text(_hint,ready?(s.iface==0?"Hold to talk - up to 10 seconds":"Hold to talk - up to 30 seconds"):"");
#endif
    if(ready && !(s.capabilities&1)) lv_label_set_text(_hint,"Listen only: no microphone");
    else if(ready && !(s.capabilities&2)) lv_label_set_text(_hint,"No speaker: replies cannot be heard");
}
void LvVoicePanel::poll(bool allowed) {
    if(!_service) return;
    _model.update(_service->status().voice,allowed);
    if(_model.visible()) build();
    if(_root && (millis()-_painted>=100)) {_painted=millis();render();}
    if(!_model.visible() && active(_model.status().phase) && !_return) {
        _return=lv_btn_create(lv_layer_top());lv_obj_add_style(_return,LvTheme::styleBtn(),0);lv_obj_set_size(_return,64,26);lv_obj_align(_return,LV_ALIGN_TOP_RIGHT,-2,2);
        auto* text=lv_label_create(_return);lv_label_set_text(text,"Voice");lv_obj_center(text);
        lv_obj_add_event_cb(_return,[](lv_event_t* e) {static_cast<LvVoicePanel*>(lv_event_get_user_data(e))->show();},LV_EVENT_CLICKED,this);
    } else if((_model.visible() || !active(_model.status().phase)) && _return) {lv_obj_del(_return);_return=nullptr;}
}
bool LvVoicePanel::handleKey(const KeyEvent& event) {
    if(!_model.visible()) {
        if(event.alt && !event.repeat && (event.character=='v' || event.character=='V') && active(_model.status().phase)) {show();return true;}
        return false;
    }
    if(event.repeat) return true;
    const auto phase=_model.status().phase;
    if(event.character==27 || event.del) {if(phase==Phase::Incoming)action(Action::Decline);else hide();}
    else if(event.enter) {if(phase==Phase::Incoming)action(Action::Accept);else if(_model.canRetry()) {_model.retry();render();}else if(!active(phase))hide();}
    else if(event.character=='e' || event.character=='E') action(Action::End);
    else if(event.character=='+') _model.volume(10);
    else if(event.character=='-') _model.volume(-10);
    // Space never becomes a synthetic press. The physical key edge owner is
    // the only authority on Pager; Deck requires its release-aware touch panel.
    return true;
}
