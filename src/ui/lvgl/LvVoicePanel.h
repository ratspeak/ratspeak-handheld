#pragma once
#include <lvgl.h>
#include "input/KeyEvent.h"
#include "voice/VoiceUi.h"
namespace handheld {class ServiceClient;}
class LvVoicePanel {
public:
    void begin(handheld::ServiceClient*);
    void poll(bool foregroundAllowed);
    void start(const char* peerHex);
    void show();
    void hide();
    bool takeIncoming() {return _model.takeIncoming();}
    bool visible() const {return _model.visible();}
    bool handleKey(const KeyEvent&);
private:
    void build();
    void render();
    void action(handheld::voice::Action);
    handheld::ServiceClient* _service=nullptr;
    handheld::voice::VoiceUi _model;
    lv_obj_t *_root=nullptr,*_label=nullptr,*_peer=nullptr,*_route=nullptr,*_hold=nullptr,*_holdLabel=nullptr,*_accept=nullptr,*_end=nullptr,*_endLabel=nullptr,*_hint=nullptr,*_return=nullptr;
    uint32_t _painted=0;
};
