#pragma once
#include "Screen.h"
#include "voice/VoiceUi.h"
class ProtocolBackend;
class VoicePanel : public Screen {
public:
    void begin(ProtocolBackend*);
    void poll(bool foregroundAllowed);
    void start(const char* peerHex);
    void show() {_model.show();}
    void hide() {_model.hide();}
    bool takeIncoming() {return _model.takeIncoming();}
    bool visible() const {return _model.visible();}
    bool active() const {return handheld::voice::active(_model.status().phase);}
    void render(M5Canvas&) override;
    bool handleKey(const KeyEvent&) override;
    const char* title() const override {return "Live voice";}
private:
    ProtocolBackend* _backend=nullptr;
    handheld::voice::VoiceUi _model;
};
