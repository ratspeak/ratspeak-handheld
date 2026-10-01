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
        auto* l=lv_label_create(_root);lv_obj_set_width(l,Theme::SCREEN_W-24);lv_obj_set_pos(l,0,y);
        lv_obj_set_style_text_font(l,&lv_font_rsdeck_14,0);lv_obj_set_style_text_color(l,lv_color_hex(Theme::TEXT_PRIMARY),0);
        lv_label_set_long_mode(l,LV_LABEL_LONG_DOT);lv_label_set_text(l,text);return l;
    };
    label("Live voice",0);_peer=label("",23);_label=label("",47);_route=label("",72);
    auto button=[&](const char* text,int x,int y,int w,int h) {
        auto* b=lv_btn_create(_root);lv_obj_add_style(b,LvTheme::styleBtn(),0);lv_obj_add_style(b,LvTheme::styleBtnPressed(),LV_STATE_PRESSED);lv_obj_set_pos(b,x,y);lv_obj_set_size(b,w,h);
        auto* l=lv_label_create(b);lv_label_set_text(l,text);lv_obj_set_style_text_font(l,&lv_font_rsdeck_14,0);lv_obj_center(l);return b;
    };
    const int width=Theme::SCREEN_W-24;
    _hold=button("Hold to talk",0,98,width,44);_holdLabel=lv_obj_get_child(_hold,0);
    lv_obj_add_event_cb(_hold,[](lv_event_t* e) {
        auto& self=*static_cast<LvVoicePanel*>(lv_event_get_user_data(e));
        const auto code=lv_event_get_code(e);
        if(code==LV_EVENT_PRESSED && self.visible()) VoiceInput::instance().edge(VoiceInput::Touch,true);
        else if(code==LV_EVENT_RELEASED || code==LV_EVENT_PRESS_LOST || code==LV_EVENT_DELETE) VoiceInput::instance().edge(VoiceInput::Touch,false);
    },LV_EVENT_ALL,this);
    _accept=button("Accept",0,98,width,44);
    lv_obj_add_event_cb(_accept,[](lv_event_t* e) {static_cast<LvVoicePanel*>(lv_event_get_user_data(e))->action(Action::Accept);},LV_EVENT_CLICKED,this);
    _end=button("End",0,148,(width-8)/2,28);_endLabel=lv_obj_get_child(_end,0);
    lv_obj_add_event_cb(_end,[](lv_event_t* e) {
        auto& self=*static_cast<LvVoicePanel*>(lv_event_get_user_data(e));
        self.action(self._model.status().phase==Phase::Incoming?Action::Decline:Action::End);
    },LV_EVENT_CLICKED,this);
    auto* close=button("Back",(width+8)/2,148,(width-8)/2,28);
    lv_obj_add_event_cb(close,[](lv_event_t* e) {static_cast<LvVoicePanel*>(lv_event_get_user_data(e))->hide();},LV_EVENT_CLICKED,this);
    _hint=label("",183);lv_obj_set_style_text_font(_hint,&lv_font_rsdeck_12,0);
}
void LvVoicePanel::render() {
    if(!_root) return;
    const auto& s=_model.status();
    char peer[33];handheld::storage::encodeHex(s.peer,16,peer);
    const auto name=_service?_service->nodes.lookupName(peer):std::string();
    lv_label_set_text(_peer,name.empty()?peer:name.c_str());lv_label_set_text(_label,_model.text());
    char info[80];snprintf(info,sizeof info,"%s  %lus  Vol %u%%",s.iface==0?"LoRa":"WiFi / TCP",(unsigned long)(s.elapsedMs/1000),s.volume);
    lv_label_set_text(_route,active(s.phase)?info:"");
    const bool incoming=s.phase==Phase::Incoming;
    const bool ready=s.phase==Phase::Ready || s.phase==Phase::Receiving || s.phase==Phase::Talking;
    if(incoming) lv_obj_clear_flag(_accept,LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(_accept,LV_OBJ_FLAG_HIDDEN);
    if(incoming) lv_obj_add_flag(_hold,LV_OBJ_FLAG_HIDDEN);else lv_obj_clear_flag(_hold,LV_OBJ_FLAG_HIDDEN);
    if(ready && (s.capabilities&1)) lv_obj_clear_state(_hold,LV_STATE_DISABLED);else lv_obj_add_state(_hold,LV_STATE_DISABLED);
    lv_label_set_text(_holdLabel,s.phase==Phase::Talking?"Talking - release to stop":"Hold to talk");
    lv_label_set_text(_endLabel,incoming?"Decline":"End");
    if(active(s.phase)) lv_obj_clear_state(_end,LV_STATE_DISABLED);else lv_obj_add_state(_end,LV_STATE_DISABLED);
#ifdef RATPAGER
    lv_label_set_text(_hint,incoming?"Enter: accept   Esc: decline":"Space: talk  E: end  Alt+V: return  +/-: vol");
#else
    lv_label_set_text(_hint,s.iface==0?"Touch and hold - max 10 seconds":"Touch and hold - max 30 seconds");
#endif
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
    else if(event.enter) {if(phase==Phase::Incoming)action(Action::Accept);else if(!active(phase))hide();}
    else if(event.character=='e' || event.character=='E') action(Action::End);
    else if(event.character=='+') _model.volume(10);
    else if(event.character=='-') _model.volume(-10);
    // Space never becomes a synthetic press. The physical key edge owner is
    // the only authority on Pager; Deck requires its release-aware touch panel.
    return true;
}
