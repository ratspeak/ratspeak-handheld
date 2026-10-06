#include "LvVoicePanel.h"
#include "runtime/ServiceClient.h"
#include "storage/Hex.h"
#include "Theme.h"
#include "LvTheme.h"
#include "fonts/fonts.h"
using namespace handheld::memo;
void LvVoicePanel::begin(handheld::ServiceClient* service) {
    _service=service;
    _model.begin(this,[](void* context,const Command& command,uint32_t serial) {
        auto& self=*static_cast<LvVoicePanel*>(context);
        return self._service && self._service->memoCommand(command,[&self,serial](const handheld::Result& result) {
            self._model.acknowledge(serial,result.outcome==handheld::Outcome::Ok?Code::Ok:result.next?Code(result.next):Code::Busy);
        })!=0;
    });
}
void LvVoicePanel::start(const char* hex) {startMessage(hex,0,false);}
void LvVoicePanel::startMessage(const char* hex,uint32_t counter,bool incoming) {
    if(!_service || !hex) return;
    uint8_t peer[16];if(!handheld::storage::decodeHex(hex,strlen(hex),peer,16)) return;
    _model.open(peer,counter,incoming);build();render();
}
void LvVoicePanel::show() {_model.show();build();render();}
void LvVoicePanel::hide() {
    _model.hide();_pressed=Ui::Choice::None;
    if(_root) {lv_obj_del(_root);_root=nullptr;}
}
void LvVoicePanel::closeConversation() {
    _model.closeConversation();_pressed=Ui::Choice::None;
    if(_root) {lv_obj_del(_root);_root=nullptr;}
}
void LvVoicePanel::action(Ui::Choice choice) {_pressed=Ui::Choice::None;_model.choose(choice);render();}
void LvVoicePanel::build() {
    if(_root || !_model.visible()) return;
    _root=lv_obj_create(lv_layer_top());lv_obj_set_size(_root,Theme::SCREEN_W,Theme::SCREEN_H);lv_obj_center(_root);
    lv_obj_add_style(_root,LvTheme::styleModal(),0);lv_obj_set_style_pad_all(_root,10,0);lv_obj_clear_flag(_root,LV_OBJ_FLAG_SCROLLABLE);
    auto label=[&](const char* text,int y) {
        auto* l=lv_label_create(_root);lv_obj_set_width(l,Theme::SCREEN_W-24);lv_obj_set_height(l,lv_font_get_line_height(&lv_font_rsdeck_14));lv_obj_set_pos(l,0,y);
        lv_obj_set_style_text_font(l,&lv_font_rsdeck_14,0);lv_obj_set_style_text_color(l,lv_color_hex(Theme::TEXT_PRIMARY),0);
        lv_label_set_long_mode(l,LV_LABEL_LONG_DOT);lv_label_set_text(l,text);return l;
    };
    _title=label("Voice message",0);lv_obj_set_width(_title,Theme::SCREEN_W-100);
    _peer=label("",24);_label=label("",48);_detail=label("",71);_hint=label("",Theme::SCREEN_H-36);
    lv_obj_set_style_text_color(_peer,lv_color_hex(Theme::ACCENT),0);
    for(auto* l:{_detail,_hint}) {lv_obj_set_style_text_font(l,&lv_font_rsdeck_12,0);lv_obj_set_style_text_color(l,lv_color_hex(Theme::TEXT_SECONDARY),0);}
    auto button=[&](const char* text,int x,int y,int w,int h) {
        auto* b=lv_btn_create(_root);lv_obj_add_style(b,LvTheme::styleBtn(),0);lv_obj_add_style(b,LvTheme::styleBtnPressed(),LV_STATE_PRESSED);lv_obj_set_pos(b,x,y);lv_obj_set_size(b,w,h);
        auto* l=lv_label_create(b);lv_label_set_text(l,text);lv_obj_set_style_text_font(l,&lv_font_rsdeck_14,0);lv_obj_center(l);return b;
    };
    const int width=Theme::SCREEN_W-24;
    _back=button("Back",width-60,-3,60,26);
    lv_obj_add_event_cb(_back,[](lv_event_t* e) {auto& self=*static_cast<LvVoicePanel*>(lv_event_get_user_data(e));self._pressed=Ui::Choice::None;self._model.back();self.render();},LV_EVENT_CLICKED,this);
    for(unsigned i=0;i<3;++i) {
        _buttons[i]=button("",i==2?(width+6)/2:0,Theme::SCREEN_H-(i?80:130),i?(width-6)/2:width,i?32:40);
        lv_obj_add_event_cb(_buttons[i],[](lv_event_t* e) {
            auto& self=*static_cast<LvVoicePanel*>(lv_event_get_user_data(e));
            unsigned index=0;while(index<3 && self._buttons[index]!=lv_event_get_target(e)) ++index;
            if(index==3) return;
            const auto code=lv_event_get_code(e);
            if(code==LV_EVENT_PRESSED) {self._pressed=self._model.choice(index);self._pressedLayout=self._model.layout();}
            else if(code==LV_EVENT_PRESS_LOST || code==LV_EVENT_DELETE) self._pressed=Ui::Choice::None;
            else if(code==LV_EVENT_CLICKED) {
                const auto choice=self._pressed;self._pressed=Ui::Choice::None;
                if(choice!=Ui::Choice::None && self._pressedLayout==self._model.layout() && choice==self._model.choice(index)) self.action(choice);
            }
        },LV_EVENT_ALL,this);
    }
    _quieter=button("-",width-68,64,30,24);_louder=button("+",width-30,64,30,24);
    lv_obj_add_event_cb(_quieter,[](lv_event_t* e) {auto& self=*static_cast<LvVoicePanel*>(lv_event_get_user_data(e));self._model.volume(-10);self.render();},LV_EVENT_CLICKED,this);
    lv_obj_add_event_cb(_louder,[](lv_event_t* e) {auto& self=*static_cast<LvVoicePanel*>(lv_event_get_user_data(e));self._model.volume(10);self.render();},LV_EVENT_CLICKED,this);
    lv_obj_set_width(_detail,width-76);
}
void LvVoicePanel::render() {
    if(!_root) return;
    const auto& s=_model.status();
    lv_obj_set_style_text_color(_title,lv_color_hex(Theme::TEXT_PRIMARY),0);
    lv_obj_set_style_text_color(_peer,lv_color_hex(Theme::ACCENT),0);
    for(auto* label:{_detail,_hint}) lv_obj_set_style_text_color(label,lv_color_hex(Theme::TEXT_SECONDARY),0);
    char peer[33];handheld::storage::encodeHex(_model.peer(),16,peer);
    const auto name=_service?_service->nodes.lookupName(peer):std::string();
    lv_label_set_text(_peer,name.empty()?peer:name.c_str());lv_label_set_text(_label,_model.text());
    lv_obj_set_style_text_color(_label,lv_color_hex(s.phase==Phase::Recording?Theme::ERROR_CLR:Theme::TEXT_PRIMARY),0);
    const auto ms=(s.phase==Phase::Recording || s.phase==Phase::Playing || s.phase==Phase::Stopping)?s.frames*40:uint32_t(s.length)*10;
    const bool running=s.phase==Phase::Recording || s.phase==Phase::Playing || s.phase==Phase::Stopping;
    char info[64];snprintf(info,sizeof info,"0:%02lu%s  Volume %u%%",(unsigned long)((ms+(running?0:999))/1000),s.phase==Phase::Recording?" / 0:15":"",s.volume);
    if(s.reason==Code::UnsupportedAudio || s.reason==Code::AudioUnavailable) snprintf(info,sizeof info,"Volume %u%%",s.volume);
    lv_label_set_text(_detail,info);lv_label_set_text(_hint,_model.guidance());
    auto shown=[](lv_obj_t* object,bool show) {if(show) lv_obj_clear_flag(object,LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(object,LV_OBJ_FLAG_HIDDEN);};
    bool backInActions=false;
    const bool confirmation=_model.choice(0)==Ui::Choice::ConfirmReplace || _model.choice(0)==Ui::Choice::ConfirmDiscard;
    const int width=Theme::SCREEN_W-24;
    for(unsigned i=0;i<3;++i) {
        const auto choice=_model.choice(i);shown(_buttons[i],choice!=Ui::Choice::None);
        // Confirmations are one equal row, Delete left and Cancel right.
        lv_obj_set_pos(_buttons[i],confirmation?int(i)*(width+6)/2:i==2?(width+6)/2:0,
            confirmation?Theme::SCREEN_H-100:Theme::SCREEN_H-(i?80:130));
        lv_obj_set_size(_buttons[i],confirmation || i?(width-6)/2:width,confirmation?40:i?32:40);
        backInActions|=choice==Ui::Choice::Back;
        lv_label_set_text(lv_obj_get_child(_buttons[i],0),Ui::label(choice));
        const bool selected=i==_model.focus();
        const bool destructive=choice==Ui::Choice::Stop || choice==Ui::Choice::ConfirmDiscard || choice==Ui::Choice::ConfirmReplace;
        const auto color=destructive?Theme::ERROR_CLR:Theme::PRIMARY;
        lv_obj_set_style_text_color(lv_obj_get_child(_buttons[i],0),lv_color_hex(destructive?Theme::ERROR_CLR:Theme::TEXT_PRIMARY),0);
        lv_obj_set_style_border_color(_buttons[i],lv_color_hex(selected?color:Theme::BORDER),0);
        lv_obj_set_style_bg_color(_buttons[i],lv_color_hex(selected?Theme::PRIMARY_SUBTLE:Theme::BG_SURFACE),0);
    }
    shown(_back,!backInActions && _model.count());
    shown(_quieter,(s.capabilities&2) && _model.count());shown(_louder,(s.capabilities&2) && _model.count());
    if(s.volume==0) lv_obj_add_state(_quieter,LV_STATE_DISABLED);else lv_obj_clear_state(_quieter,LV_STATE_DISABLED);
    if(s.volume==100) lv_obj_add_state(_louder,LV_STATE_DISABLED);else lv_obj_clear_state(_louder,LV_STATE_DISABLED);
}
void LvVoicePanel::poll(bool allowed) {
    if(!_service) return;
    _model.update(_service->status().memo,allowed);
    if(_model.visible()) {build();if(millis()-_painted>=100) {_painted=millis();render();}}
    else if(_root) {lv_obj_del(_root);_root=nullptr;}
}
bool LvVoicePanel::handleKey(const KeyEvent& event) {
    if(!_model.visible()) return false;
    if(event.repeat) return true;
    // Mixing touch with keyboard navigation must require a fresh touch press,
    // even if the user later returns to the same confirmation/layout.
    _pressed=Ui::Choice::None;
    if(event.character==27 || event.del) _model.back();
    else if(event.left || event.up) _model.move(-1);
    else if(event.right || event.down || event.tab) _model.move(1);
    else if(event.enter) _model.choose(_model.choice(_model.focus()));
    else if(event.character=='+') _model.volume(10);
    else if(event.character=='-') _model.volume(-10);
    render();return true;
}
