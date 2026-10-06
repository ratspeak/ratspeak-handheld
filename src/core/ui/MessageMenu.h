#pragma once
#include "history/HistoryWindow.h"
#include "util/DisplayText.h"
#include "MessageAudio.h"
#include "reticulum/LXMFStatus.h"
#include <cstring>

namespace handheld::ui {
// No selected body cache. Actions retain the immutable selector and read the
// complete content through one bounded storage response only when requested.
struct MessageMenu {
    enum class State:uint8_t {Closed,Menu,Confirm,Copying,Deleting};
    enum class Action:uint8_t {Read,Copy,Retry,Delete,Back,Confirm,Cancel,None};
    storage::RecordKey key;
    uint8_t local[16]{};
    uint32_t revision=0,serial=0;
    const char* notice=nullptr;
    State state=State::Closed;
    uint8_t selected=0;
    bool more=false,copyable=false,retryable=false;
    bool visible() const {return state!=State::Closed;}
    bool busy() const {return state==State::Copying || state==State::Deleting;}
    void close() {state=State::Closed;if(serial!=UINT32_MAX) ++serial;}
    bool begin(const history::HistoryWindow& window,size_t index,const uint8_t identity[16]) {
        const auto* row=window.span(index);if(!row || row->unavailable() || serial==UINT32_MAX) return false;
        ++serial;memcpy(key.peer,window.peer(),16);key.counter=row->counter;key.incoming=row->incoming();
        memcpy(local,identity,16);revision=row->recordRevision;more=window.mode()==history::HistoryWindow::Mode::Chat && row->more();copyable=row->textLength && !generatedAudioText(*row,window.text(index));
        retryable=row->hasAudio() && !row->incoming() && window.statusReady() &&
            !(row->flags&(history::HistoryWindow::Span::StatusPending|history::HistoryWindow::Span::StatusUnavailable)) && messaging::retryableStatus(row->status);
        state=State::Menu;selected=0;notice=nullptr;return true;
    }
    unsigned count() const {return busy()?1:state==State::Confirm?2:unsigned(more)+unsigned(copyable)+unsigned(retryable)+2;}
    Action action(unsigned index) const {
        if(index>=count()) return Action::None;
        if(busy()) return Action::Back;
        if(state==State::Confirm) return index?Action::Cancel:Action::Confirm;
        if(more) {if(!index) return Action::Read;--index;}
        if(retryable) {if(!index) return Action::Retry;--index;}
        if(copyable) {if(!index) return Action::Copy;--index;}
        return index?Action::Back:Action::Delete;
    }
    const char* title() const {return state==State::Confirm?"Delete this message?":busy()?state==State::Deleting?"Deleting...":"Copying...":"Message actions";}
    const char* hint() const {return state==State::Confirm?"Only on this device":notice?notice:"";}
    const char* label(unsigned index) const {
        switch(action(index)) {
        case Action::Retry:return "Retry delivery";case Action::Read:return "Read full";case Action::Copy:return "Copy to composer";
        case Action::Delete:return "Delete message";case Action::Back:return "Back";
        case Action::Confirm:return "Delete";case Action::Cancel:return "Cancel";case Action::None:return "";
        }return "";
    }
    void move(int delta) {const auto n=int(count());selected=uint8_t((int(selected)+delta%n+n)%n);}
    void confirm() {state=State::Confirm;selected=1;notice=nullptr;}
    void fail(const char* text) {state=State::Menu;selected=0;notice=text;}
    bool matches(const storage::StoredRecordHeader& header) const {
        return key.counter==header.counter && key.incoming==header.incoming && header.revision>=revision &&
            !memcmp(key.peer,header.incoming?header.source:header.destination,16) &&
            !memcmp(local,header.incoming?header.destination:header.source,16);
    }
    // A full copy or no change. Never use a shortened preview as the source.
    bool copy(const storage::StoredRecordHeader& header,const uint8_t* bytes,size_t length,char* out,size_t capacity) const {
        const size_t total=size_t(header.titleLength)+header.contentLength;
        const size_t separator=header.titleLength && header.contentLength?1:0;
        if(!matches(header) || !bytes || length!=total || !total || total+separator>=capacity) return false;
        for(size_t at=0;at<length;) {bool escape=false;auto n=display::codepoint(bytes+at,length-at,true,escape);if(!n || escape) return false;at+=n;}
        memcpy(out,bytes,header.titleLength);size_t at=header.titleLength;
        if(separator) out[at++]='\n';
        memcpy(out+at,bytes+header.titleLength,header.contentLength);out[total+separator]=0;return true;
    }
};
static_assert(sizeof(MessageMenu)<=64,"Message actions retain bounded metadata only");
}
