#pragma once
#include <lvgl.h>
#include "input/KeyEvent.h"
#include "voice/MemoUi.h"
namespace handheld {class ServiceClient;}
class LvVoicePanel {
public:
    void begin(handheld::ServiceClient*);
    void poll(bool foregroundAllowed);
    void start(const char* peerHex);
    void startMessage(const char* peerHex,uint32_t counter,bool incoming);
    void show();
    void hide();
    void closeConversation();
    bool takeIncoming() {return false;}
    bool visible() const {return _model.visible();}
    bool active() const {return _model.active();}
    bool handleKey(const KeyEvent&);
private:
    void build();
    void render();
    void action(handheld::memo::Ui::Choice);
    handheld::ServiceClient* _service=nullptr;
    handheld::memo::Ui _model;
    lv_obj_t *_root=nullptr,*_title=nullptr,*_label=nullptr,*_peer=nullptr,*_detail=nullptr,*_hint=nullptr,*_quieter=nullptr,*_louder=nullptr,*_back=nullptr;
    lv_obj_t* _buttons[3]{};
    uint32_t _pressedLayout=0;
    handheld::memo::Ui::Choice _pressed=handheld::memo::Ui::Choice::None;
    uint32_t _painted=0;
};
