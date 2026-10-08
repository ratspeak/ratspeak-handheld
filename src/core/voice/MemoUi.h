#pragma once
#include "MemoTypes.h"
#include "VoiceWorker.h"
#include <cstring>

namespace handheld::memo {
// UI-only state. The controller owns audio/storage; callbacks carry a serial
// so a completion from a previous view cannot change the current controls.
class Ui {
public:
    enum class Choice : uint8_t { None, Record, Stop, Play, Send, More, Back, Replace, Discard, Cancel, ConfirmReplace, ConfirmDiscard, Retry, StopPlayback, RetryDelivery };
    using Submit=bool(*)(void*,const Command&,uint32_t);
    void begin(void* context,Submit submit) {_context=context;_submit=submit;}
    void open(const uint8_t peer[16],uint32_t counter=0,bool incoming=false);
    void toggleMessage(const uint8_t peer[16],uint32_t counter,bool incoming);
    void retryMessage(const uint8_t peer[16],uint32_t counter,bool incoming);
    bool inlinePlayback() const {return _inline;}
    // One durable send completion, consumed only by its still-open chat.
    bool takeSent(const uint8_t peer[16]) {
        if(!_sent || std::memcmp(peer,_status.peer,16)) return false;
        _sent=false;return true;
    }
    uint32_t elapsedFrames() const {return _elapsed;}
    Code error() const {return _error==Code::Ok?_status.reason:_error;}
    void update(const Status&,bool foregroundAllowed);
    void acknowledge(uint32_t serial,Code);
    void hide();
    void closeConversation();
    void back();
    void show() {if(_status.view) _visible=true;}
    void choose(Choice);
    void move(int delta);
    void volume(int delta);
    Choice choice(unsigned index) const;
    static const char* label(Choice);
    unsigned count() const;
    unsigned focus() const {return _focus;}
    bool visible() const {return _visible;}
    bool active() const {return busy(_status.phase);}
    const Status& status() const {return _status;}
    const uint8_t* peer() const {return _openAfterEnd?_nextOpen.peer:_status.peer;}
    const char* text() const;
    const char* guidance() const;
    uint32_t layout() const;
private:
    enum class Menu : uint8_t { Main, More, Replace, Discard };
    enum class Deletion : uint8_t { None, RecordAgain, Close };
    void requestOpen(const uint8_t peer[16],uint32_t counter,bool incoming,bool inlinePlayback,Action afterOpen=Action::Close);
    void send(Action);
    void stop(bool close);
    void changed(uint32_t before);
    void* _context=nullptr;
    Submit _submit=nullptr;
    Status _status;
    // Keep the accepted owner while a replacement Open is awaiting admission.
    // A rejected panel reopen must not orphan the conversation's temporary clip.
    Status _owner;
    Command _nextOpen;
    uint32_t _serial=0,_anchor=0,_deleteRevision=0,_elapsed=0;
    Code _error=Code::Ok;
    Action _action=Action::Close,_openAction=Action::Close,_nextAction=Action::Close;
    Menu _menu=Menu::Main;
    Deletion _deletion=Deletion::None;
    uint8_t _focus=0;
    bool _visible=false,_pending=false,_close=false,_stop=false;
    bool _inline=false,_nextInline=false,_openAfterClose=false;
    bool _end=false,_endAccepted=false,_openAfterEnd=false,_sent=false;
};
}
