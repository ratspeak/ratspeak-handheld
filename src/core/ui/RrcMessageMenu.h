#pragma once
#include "RrcCompose.h"
#include "history/RrcHistory.h"
#include <cstdio>

namespace handheld::ui {
// Only the selected message's identity/status is retained. Each text action
// reads its exact immutable counter through the existing query owner, then
// inserts into the foreground editor. No second body cache is introduced.
struct RrcMessageMenu {
    enum class State : uint8_t { Closed, Loading, Failed, Menu, Identity, Copy };
    enum class Action : uint8_t { Read, Quote, Mention, Private, Identity, Direct, Copy, Back };
    State state=State::Closed;
    uint8_t selected=0,kind=0,status=0;
    uint32_t counter=0, revision=0;
    uint8_t source[16]{};
    char nickname[33]{};
    bool visible() const {return state!=State::Closed;}
    void close() {state=State::Closed;}
    void begin(uint32_t value) {const auto next=revision+1;*this={};revision=next;counter=value;state=State::Loading;}
    bool accept(const rrc::MessageDetail& detail) {
        if(state!=State::Loading || detail.counter!=counter) return false;
        memcpy(source,detail.source,16);memcpy(nickname,detail.nickname,sizeof nickname);
        kind=detail.kind;status=detail.status;state=State::Menu;selected=0;return true;
    }
    size_t count() const {
        if(state==State::Identity) return 5;
        if(state==State::Copy) return 3;
        if(state==State::Failed || (state==State::Menu && !kind)) return 2;
        return state==State::Menu?8:1;
    }
    Action action() const {return !kind && selected==1?Action::Back:Action(selected);}
    void move(int direction) {const auto n=count();selected=uint8_t((selected+n+direction)%n);}
    static void hex(const uint8_t* bytes,size_t length,char* out) {
        static constexpr char digits[]="0123456789abcdef";
        for(size_t n=0;n<length;++n) {out[n*2]=digits[bytes[n]>>4];out[n*2+1]=digits[bytes[n]&15];}
        out[length*2]=0;
    }
    const char* title() const {
        return state==State::Identity?"Sender identity":state==State::Copy?"Reuse message?":"Message actions";
    }
    void label(size_t row,char* out,size_t capacity) const {
        if(!capacity) return;
        const char* value="Back";
        if(state==State::Loading) value="Loading... Back";
        else if(state==State::Failed) value=row?"Back":"Read failed; retry";
        else if(state==State::Identity) {
            if(row<2) {char part[17];hex(source+row*8,8,part);snprintf(out,capacity,"%s",part);return;}
            value=row==2?"Identity reported":row==3?"by this hub":"Back";
        } else if(state==State::Copy) value=row==0?"May duplicate a prior send":row==1?"Copy to draft":"Cancel";
        else {
            static constexpr const char* names[]={"Read full","Quote","Mention","Private notice","Full identity","Open LXMF","Reuse text","Back"};
            value=names[!kind && row==1?7:std::min(row,size_t(7))];
        }
        snprintf(out,capacity,"%s",value);
    }
    static size_t mention(const rrc::MessageDetail& detail,char (&out)[36]) {
        out[0]='@';size_t n=strnlen(detail.nickname,33);
        if(n && n<=32) memcpy(out+1,detail.nickname,n);
        else {hex(detail.source,16,out+1);n=32;}
        out[1+n]=' ';out[2+n]=0;return n+2;
    }
    static size_t quote(const rrc::MessageDetail& detail,char* out,size_t capacity,bool& shortened) {
        shortened=false;if(capacity<8 || !detail.length) return 0;
        // Explicit local quotation, with UTF-8-safe truncation and a visible
        // ellipsis. Newlines stay ordinary text; this is not server threading.
        out[0]='>';out[1]=' ';
        const auto n=rrc_input::textPrefix(detail.text,detail.length,capacity-7);
        memcpy(out+2,detail.text,n);size_t used=2+n;
        shortened=n<detail.length;
        if(shortened) {memcpy(out+used,"...",3);used+=3;}
        out[used++]='\n';out[used]=0;return used;
    }
};
static_assert(sizeof(RrcMessageMenu)<=64,"Message tools retain metadata only");
}
