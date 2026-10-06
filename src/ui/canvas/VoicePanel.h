#pragma once
#include "Screen.h"
#include "voice/MemoUi.h"
class ProtocolBackend;
class VoicePanel : public Screen {
public:
    void begin(ProtocolBackend*);
    void poll(bool foregroundAllowed);
    void start(const char* peerHex);
    void startMessage(const char* peerHex,uint32_t counter,bool incoming);
    void show() {_model.show();}
    void hide() {_model.hide();}
    void closeConversation() {_model.closeConversation();}
    bool takeIncoming() {return false;}
    bool visible() const {return _model.visible() && !_model.inlinePlayback();}
    handheld::memo::Ui& model() {return _model;}
    bool active() const {return _model.active();}
    void render(M5Canvas&) override;
    bool handleKey(const KeyEvent&) override;
    const char* title() const override {return "Voice message";}
private:
    ProtocolBackend* _backend=nullptr;
    handheld::memo::Ui _model;
};
