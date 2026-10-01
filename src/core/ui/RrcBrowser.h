#pragma once

#include "RrcNavigation.h"
#include "RrcCompose.h"
#include <cstdio>
#include <functional>

namespace handheld::ui {

// Four visible metadata rows, never a transcript or a duplicate discovery table.
// The native host supplies snapshots on its appropriate owner (Service or board
// loop). All user actions carry the identity selected before an asynchronous read.
class RrcBrowser {
public:
    using Page = RrcNavigation::Page;
    using Choice = RrcNavigation::Choice;
    using Open = std::function<void(const rrc::Conversation&, const char*)>;
    using Send = std::function<rrc::Code(const rrc::Command&, const uint8_t*, size_t)>;
    static constexpr size_t Capacity = 4;
    struct Row {
        uint8_t id[16]{};
        char name[65]{};
        uint32_t unread = 0;
        uint32_t state = 0; // Row phase/flags, or the private inbox cursor.
    };
    RrcNavigation navigation;
    Send send;
    Open open;
    std::function<void(const char*)> openDirect;
    std::function<void(const char*)> notice;
    uint32_t revision() const { return _renderRevision; }

    void observe(const rrc::Status& status, uint32_t identity) {
        const auto old = navigation.revision();
        navigation.observe(status, identity);
        if (old != navigation.revision() || _status != status.revision) {
            _status = status.revision; _dirty = true;
            ++_renderRevision;
        }
    }
    void refresh() { _dirty = true; }
    void visible(bool visible) { _visible = visible; if (visible) refresh(); }
    bool needsRows() const {
        return _visible && !navigation.direct() && !_pending && _dirty &&
            (dataPage() || detailPage());
    }
    uint32_t beginRows() { _pending = navigation.revision(); _dirty = false; return _pending; }
    // The host also fences session and local identity before calling this.
    bool completeRows(uint32_t view, bool ok) {
        if (_pending != view) return false;
        _pending = 0;
        if (view != navigation.revision()) { _dirty = true; return false; }
        if (!ok) { _failed=true;_dirty=false;_count=0;_more=false;_rowsView=view;++_renderRevision;return false; }
        _failed=false;
        _previousCount = _rowsView == view ? _count : 0;
        _count = 0; _more = false; _rowsView = view; return true;
    }
    void cancelRows(uint32_t view) { if (_pending == view) { _pending = 0; _dirty = true; } }
    template<class T> void rows(const T* values, size_t count, bool following = false, bool sliced = false) {
        const auto prefix = navigation.page() == Page::Channels ? 1u : 0u;
        const auto selected = navigation.selected();
        const bool hadSelection = selected >= prefix && selected - prefix < _previousCount;
        const Row previous = hadSelection ? _rows[selected-prefix] : Row{};
        const auto offset = sliced ? 0 : navigation.offset();
        for (size_t n = offset; n < count && _count < Capacity; ++n) append(values[n]);
        _more = count > offset + Capacity || following;
        if (hadSelection) {
            size_t found = _count;
            for (size_t n=0;n<_count;++n) if (!std::memcmp(previous.id,_rows[n].id,16) &&
                !std::strcmp(previous.name,_rows[n].name)) { found=n;break; }
            // A disappearing row loses selection, never silently selects its successor.
            navigation.select(found == _count ? SIZE_MAX : found+prefix);
        } else if (_previousCount && selected >= _previousCount + prefix && _count != _previousCount) {
            navigation.select(SIZE_MAX);
        }
        ++_renderRevision;
    }
    void rows(const rrc::RoomView* values,size_t count,bool following=false,bool sliced=false) {
        if(!detailPage()) {rows<rrc::RoomView>(values,count,following,sliced);return;}
        if(count!=1) {_failed=true;_count=0;++_renderRevision;return;}
        static_assert(sizeof(rrc::RoomView)<=sizeof _rows,"Room detail reuses the visible metadata page");
        std::memcpy(_rows,values,sizeof *values);_count=1;++_renderRevision;
    }
    void rows(const rrc::PrivateView* values,size_t count,bool following=false,bool =true) {
        rows<rrc::PrivateView>(values,count,false,true);
        _newer=navigation.after()?following:navigation.offset()!=0;
        _more=count && (navigation.after()?navigation.offset()!=0:following);
    }
    bool ready() const { return !(dataPage() || detailPage()) || _rowsView == navigation.revision(); }
    bool form() const {
        const auto p = navigation.page();
        return p == Page::Address || p == Page::RoomName || p == Page::RoomKey || p == Page::Nickname || p == Page::Advanced;
    }
    size_t formLimit() const {
        switch (navigation.page()) {
        case Page::Address: return 32;
        case Page::RoomName: return 64;
        case Page::RoomKey: return rrc::KeyCapacity;
        case Page::Nickname: return 32;
        default: return rrc::DraftCapacity;
        }
    }
    const char* heading() const {
        switch (navigation.page()) {
        case Page::Hubs: return "Hubs";
        case Page::Channels: return "Channels";
        case Page::ActiveHub: return "Hub menu";
        case Page::Hub: case Page::SwitchHub: return navigation.hubName();
        case Page::Room: case Page::Leave: case Page::ClearHistory: return navigation.conversation().room;
        case Page::Address: return "Hub address (32 hex)";
        case Page::RoomName: return "Join channel by name";
        case Page::RoomKey: return "Channel key";
        case Page::Nickname: return "My hub nickname";
        case Page::Advanced: return "Advanced /command";
        case Page::People: return "Known people";
        case Page::Person: case Page::Identity: return navigation.personName();
        case Page::Directory: return "Public channels";
        case Page::Disconnect: return "Disconnect this hub?";
        case Page::HubInfo: return "Hub details";
        case Page::RoomInfo: case Page::RoomText: return navigation.conversation().room;
        case Page::Notifications: return "Channel notifications";
        case Page::ForgetRoom: return "Forget saved channel?";
        case Page::ForgetKey: return "Forget remembered key?";
        case Page::Help: return "Hub help";
        case Page::Inbox: return "Private notices";
        case Page::SavedRooms: return "Saved channels";
        }
        return "Hub";
    }
    size_t count() const {
        if (failed()) return 2;
        switch (navigation.page()) {
        case Page::Hubs: return _count + 2 + pagerCount(); // address, back
        case Page::Channels: return 1 + _count + 2 + pagerCount(); // hub, join, directory
        case Page::Directory: case Page::People: return _count + 2 + pagerCount(); // refresh, back
        case Page::SavedRooms: case Page::Inbox: return _count+2+pagerCount();
        case Page::ActiveHub: return sizeof RrcNavigation::HubActions / sizeof(RrcNavigation::Item);
        case Page::Room: return sizeof RrcNavigation::RoomActions / sizeof(RrcNavigation::Item);
        case Page::Person: return sizeof RrcNavigation::PersonActions / sizeof(RrcNavigation::Item);
        case Page::Hub: return 7;
        case Page::SwitchHub: case Page::Disconnect: case Page::Leave: case Page::ClearHistory: return 2;
        case Page::Identity: return 3;
        case Page::HubInfo: return 5;
        case Page::Help: return 5;
        case Page::RoomInfo: return 10;
        case Page::Notifications: return 4;
        case Page::ForgetRoom: case Page::ForgetKey: return 3;
        case Page::RoomText: return textLines()+1;
        default: return 1;
        }
    }
    void label(size_t index, const rrc::Status& status, char* out, size_t size) const {
        if (!size) return;
        out[0] = 0;
        if (failed()) { copy(out,size,index?"Back":"Could not load; retry");return; }
        const auto page = navigation.page();
        if(page==Page::RoomText) {
            if(index==textLines()) copy(out,size,"Back");else textLine(index,out,size);return;
        }
        if(page==Page::ForgetRoom || page==Page::ForgetKey) {
            copy(out,size,index==0 ? (page==Page::ForgetRoom?"History and membership kept":"History and membership kept") : index==1?"Confirm":"Cancel");return;
        }
        if(page==Page::RoomInfo || page==Page::Notifications) {
            rrc::RoomView room;std::memcpy(&room,_rows,sizeof room);
            if(page==Page::Notifications) {
                const rrc::Notifications choices[]={rrc::Notifications::All,rrc::Notifications::Mentions,rrc::Notifications::Muted};
                if(index<3) std::snprintf(out,size,"%s%s",room.notifications==choices[index]?"[x] ":"[ ] ",rrc::notificationName(choices[index]));
                else copy(out,size,"Back");return;
            }
            switch(index) {
            case 0: copy(out,size,room.phase==rrc::RoomPhase::Joined?"Joined":room.phase==rrc::RoomPhase::Joining?"Joining":
                room.phase==rrc::RoomPhase::Recovering?"Reconnecting":room.phase==rrc::RoomPhase::NeedsKey?"Key needed":
                room.phase==rrc::RoomPhase::Leaving?"Leaving":room.phase==rrc::RoomPhase::Error?"Join failed":"Not joined");return;
            case 1: std::snprintf(out,size,"Topic: %s",room.topic[0]?room.topic:"unknown");return;
            case 2: std::snprintf(out,size,"Modes: %s",room.modes[0]?room.modes:"unknown");return;
            case 3: std::snprintf(out,size,"Notifications: %s",rrc::notificationName(room.notifications));return;
            case 4: copy(out,size,room.keyRemembered?"Key remembered on this device":"No remembered key");return;
            case 5: copy(out,size,room.membersComplete?"People: complete at last update":"People: partial or unknown");return;
            case 6: copy(out,size,room.registered==1?"Registered channel":room.registered==2?"Unregistered channel":"Registration unknown");return;
            case 7: copy(out,size,"Forget saved channel");return;
            case 8: copy(out,size,"Forget remembered key");return;
            default: copy(out,size,"Back");return;
            }
        }
        if (page == Page::Channels && index == 0) {
            std::snprintf(out, size, "%s  > %s", status.name[0] ? status.name : "Current hub", rrc::phaseName(status.phase)); return;
        }
        size_t dataIndex = index - (page == Page::Channels ? 1 : 0);
        if (dataPage()) {
            if (dataIndex < _count) {
                const auto& row = _rows[dataIndex];
                if (page == Page::Channels) {
                    const char* state = row.state == uint8_t(rrc::RoomPhase::Joined) ? "" :
                        row.state == uint8_t(rrc::RoomPhase::Available) ? " [available]" :
                        row.state == uint8_t(rrc::RoomPhase::NeedsKey) ? " [key needed]" :
                        row.state == uint8_t(rrc::RoomPhase::Joining) ? " [joining]" :
                        row.state == uint8_t(rrc::RoomPhase::Recovering) ? " [reconnecting]" : " [saved]";
                    if (row.unread) std::snprintf(out, size, "%s [%lu]%s", row.name, static_cast<unsigned long>(row.unread), state);
                    else std::snprintf(out, size, "%s%s", row.name, state);
                } else if (page == Page::Hubs) std::snprintf(out, size, "%s%s", row.name, row.state & 1 ? " [saved]" : "");
                else if(page==Page::Inbox && row.unread) std::snprintf(out,size,"%s [%lu]",row.name,static_cast<unsigned long>(row.unread));
                else if (page == Page::People) std::snprintf(out, size, "%s [%02x%02x%02x]", row.name, row.id[0], row.id[1], row.id[2]);
                else copy(out, size, row.name);
                return;
            }
            dataIndex -= _count;
            if (previousPage()) { if (!dataIndex) { copy(out, size, "< Previous"); return; } --dataIndex; }
            if (_more) { if (!dataIndex) { copy(out, size, "Next >"); return; } --dataIndex; }
            if (page == Page::Channels && dataIndex == 1) {
                copy(out,size,status.directoryPending?"Public channels: loading...":status.directoryStale?"Public channels: stale; refresh":
                    status.directoryFailed?"Public channels: unavailable; retry":!status.directoryKnown?"Browse public channels":
                    status.directoryPartial?"Public channels: partial list":!status.directoryCount?"No public channels; refresh":"Refresh public channels");return;
            }
            copy(out, size, dataIndex == 0 ? page == Page::Hubs ? "Enter hub address" : page == Page::Channels ? "Join by name" : "Refresh" :
                 page == Page::Channels ? "Browse public channels" : "Back"); return;
        }
        if (page == Page::ActiveHub) { copy(out, size, RrcNavigation::HubActions[index].label); return; }
        if (page == Page::Room) { copy(out, size, RrcNavigation::RoomActions[index].label); return; }
        if (page == Page::Person) { copy(out, size, RrcNavigation::PersonActions[index].label); return; }
        if (page == Page::Hub) { static constexpr const char* items[] = {"Connect", "Save hub", "Forget bookmark", "Full address", "Saved channels", "Private notices", "Back"}; copy(out,size,items[index]); return; }
        if (page == Page::Identity || page == Page::HubInfo) {
            const uint8_t* address = page == Page::Identity ? navigation.person() : _browsedInfo ? navigation.selectedHub() : status.hub;
            if (index < 2) { for (size_t n = 0; n < 8 && n * 2 + 2 < size; ++n) std::snprintf(out + n*2, size - n*2, "%02x", address[index*8+n]); return; }
            if (page == Page::HubInfo && index == 2) { copy(out,size,_browsedInfo && std::memcmp(navigation.selectedHub(),status.hub,16)?"Not connected to this hub":rrc::phaseName(status.phase)); return; }
            if (page == Page::HubInfo && index == 3) { if (_browsedInfo && std::memcmp(navigation.selectedHub(),status.hub,16)) copy(out,size,"Connect to read hub limits");
                else std::snprintf(out,size,"Text limit: %u bytes",status.bodyLimit); return; }
        }
        if (page == Page::Help) {
            static constexpr const char* items[] = {"One live hub; browsing keeps it", "Private notices stay in this hub", "Room echo confirms hub receipt", "Unconfirmed sends are not retried", "Back"}; copy(out,size,items[index]); return;
        }
        if (page == Page::SwitchHub || page == Page::Disconnect || page == Page::Leave || page == Page::ClearHistory) {
            copy(out,size,index ? "Cancel" : page == Page::SwitchHub ? "Disconnect current; connect here" : page == Page::ClearHistory ? "Clear this local history" : "Confirm"); return;
        }
        copy(out,size,"Back");
    }
    void back() { navigation.back(); changed(); }
    void activate(size_t index, const rrc::Status& status) {
        if (index >= count() || !ready()) return;
        if (failed()) { if (index) back();else { _failed=false;refresh();++_renderRevision; } return; }
        if (status.generation != navigation.session()) { tell(rrc::codeName(rrc::Code::Stale)); return; }
        auto page = navigation.page();
        if(page==Page::Notifications) {
            if(index==3) navigation.back();
            else {auto command=navigation.command(rrc::Action::Mute);const uint8_t choices[]={0,2,1};command.flags=choices[index];
                if(execute(command)) navigation.back();}
            changed();return;
        }
        if(page==Page::ForgetRoom || page==Page::ForgetKey) {
            if(index==2) navigation.back();
            else if(index==1 && execute(navigation.command(page==Page::ForgetRoom?rrc::Action::ForgetRoom:rrc::Action::ForgetKey))) navigation.back();
            changed();return;
        }
        if(page==Page::RoomInfo) {
            if(index==9) navigation.back();
            else if(index==7 || index==8) navigation.push(index==7?Page::ForgetRoom:Page::ForgetKey);
            else if(index==3) navigation.push(Page::Notifications);
            else {char text[112];label(index,status,text,sizeof text);if(navigation.push(Page::RoomText)) {std::memset(_rows,0,sizeof _rows);std::memcpy(_rows,text,std::strlen(text)+1);}}
            changed();return;
        }
        if(page==Page::RoomText) {if(index==textLines()) {navigation.back();changed();}return;}
        if (page == Page::Channels && !index) { navigation.activeHub(status); changed(); return; }
        if (dataPage()) {
            size_t n = index - (page == Page::Channels ? 1 : 0);
            if (n < _count) {
                const auto row = _rows[n]; // immutable across navigation/callbacks
                if(page==Page::Inbox) {
                    rrc::Conversation binding;std::memcpy(binding.hub,navigation.conversation().hub,16);
                    std::memcpy(binding.participant,row.id,16);if(open) open(binding,nullptr);return;
                }
                if (page == Page::Hubs) { rrc::HubView hub; std::memcpy(hub.address,row.id,16); copy(hub.name,sizeof hub.name,row.name); navigation.chooseHub(hub); }
                else if (page == Page::People) { rrc::PersonView person; std::memcpy(person.identity,row.id,16); copy(person.nickname,sizeof person.nickname,row.name); navigation.choosePerson(person); }
                else navigation.chooseRoom(page==Page::SavedRooms?navigation.conversation().hub:status.hub,row.name);
                changed(); return;
            }
            n -= _count;
            if (previousPage()) { if (!n) {
                if (page==Page::Inbox) navigation.pageCursor(_count?_rows[0].state:0,_count!=0);
                else navigation.pageOffset(navigation.offset()-Capacity);changed();return;
            } --n; }
            if (_more) { if (!n) {
                if (page==Page::Inbox && _count) navigation.pageCursor(_rows[_count-1].state,false);
                else navigation.pageOffset(navigation.offset()+Capacity);changed();return;
            } --n; }
            if (page == Page::Hubs) { if (!n) navigation.push(Page::Address); else navigation.back(); }
            else if (page == Page::Channels) { navigation.push(n ? Page::Directory : Page::RoomName); if (n) execute(navigation.command(rrc::Action::Directory)); }
            else if (!n) { if(page==Page::Inbox) navigation.pageCursor(0,false);
                else if (page!=Page::SavedRooms) execute(navigation.command(page == Page::People ? rrc::Action::Who : rrc::Action::Directory)); }
            else navigation.back();
            changed(); return;
        }
        if (page == Page::ActiveHub) { choose(RrcNavigation::HubActions[index].choice,status); return; }
        if (page == Page::Room) { choose(RrcNavigation::RoomActions[index].choice,status); return; }
        if (page == Page::Person) { choose(RrcNavigation::PersonActions[index].choice,status); return; }
        if (page == Page::Hub) {
            if (index == 0) {
                if (navigation.connected() && std::memcmp(status.hub,navigation.selectedHub(),16)) navigation.push(Page::SwitchHub);
                else if (execute(navigation.command(rrc::Action::Connect,true))) navigation.root();
            } else if (index == 1 || index == 2) execute(navigation.command(index == 1 ? rrc::Action::SaveHub : rrc::Action::ForgetHub,true));
            else if (index == 3) { _browsedInfo = true; navigation.push(Page::HubInfo); }
            else if (index == 4) navigation.savedRooms(true);
            else if (index == 5) navigation.inbox(true);
            else navigation.back();
        } else if (page == Page::SwitchHub || page == Page::Disconnect || page == Page::Leave || page == Page::ClearHistory) {
            if (index) navigation.back();
            else if (execute(navigation.command(page == Page::SwitchHub ? rrc::Action::Connect : page == Page::Disconnect ?
                rrc::Action::Disconnect : page == Page::Leave ? rrc::Action::Leave : rrc::Action::ClearHistory, page == Page::SwitchHub))) navigation.root();
        } else if (index + 1 == count()) navigation.back();
        changed();
    }
    bool submit(const char* text, size_t length, const rrc::Status& status, bool remember = false) {
        const auto page = navigation.page();
        if (!form() || length > formLimit() || (length && (!text || std::memchr(text,0,length)))) return false;
        if (page == Page::Address) {
            rrc::HubView hub;
            if (!rrc_input::address(text,length,hub.address)) { tell("Enter exactly 32 hexadecimal digits"); return false; }
            std::snprintf(hub.name,sizeof hub.name,"Hub %02x%02x%02x",hub.address[0],hub.address[1],hub.address[2]);
            navigation.back(); navigation.chooseHub(hub); changed(); return true;
        }
        if (page == Page::RoomName) {
            char name[65]{};
            if (!rrc_input::room(text,length,name)) { tell("Enter a valid channel name"); return false; }
            navigation.back(); navigation.chooseRoom(status.hub,name); changed(); return true;
        }
        auto command = navigation.command(page == Page::RoomKey ? rrc::Action::Join : page == Page::Nickname ? rrc::Action::Nickname : rrc::Action::Advanced);
        if (page == Page::RoomKey) command.flags = remember;
        if (page == Page::Advanced) {
            if (_advancedRoom) { command = navigation.command(rrc::Action::Message); command.action = rrc::Action::Advanced; }
            if (!length || text[0] != '/') { tell("Advanced commands begin with /"); return false; }
        }
        if (!execute(command,reinterpret_cast<const uint8_t*>(text),length)) return false;
        navigation.back(); changed(); return true;
    }

private:
    void choose(Choice choice, const rrc::Status& status) {
        switch (choice) {
        case Choice::Back: navigation.back(); break;
        case Choice::BrowseHubs: navigation.browseHubs(); break;
        case Choice::SavedRooms: navigation.savedRooms(); break;
        case Choice::HubInfo: _browsedInfo = false; navigation.push(Page::HubInfo); break;
        case Choice::Disconnect: navigation.push(Page::Disconnect); break;
        case Choice::Leave: navigation.push(Page::Leave); break;
        case Choice::ClearHistory: navigation.push(Page::ClearHistory); break;
        case Choice::Nickname: navigation.push(Page::Nickname); break;
        case Choice::Advanced: _advancedRoom = navigation.page() == Page::Room; navigation.push(Page::Advanced); break;
        case Choice::Help: navigation.push(Page::Help); break;
        case Choice::JoinWithKey: navigation.push(Page::RoomKey); break;
        case Choice::Join: execute(navigation.command(rrc::Action::Join)); break;
        case Choice::People: if (navigation.push(Page::People)) execute(navigation.command(rrc::Action::Who)); break;
        case Choice::RoomInfo: navigation.push(Page::RoomInfo); break;
        case Choice::Identity: navigation.push(Page::Identity); break;
        case Choice::OpenChat: if (open) open(navigation.conversation(),nullptr); break;
        case Choice::PrivateNotice: if (open) open(navigation.privateConversation(),nullptr); break;
        case Choice::Notices: { rrc::Conversation hub; std::memcpy(hub.hub,status.hub,16); if (open) open(hub,nullptr); break; }
        case Choice::Mention: {
            char mention[36];
            if(navigation.personName()[0]) std::snprintf(mention,sizeof mention,"@%s ",navigation.personName());
            else {mention[0]='@';for(size_t n=0;n<16;++n) std::snprintf(mention+1+n*2,sizeof mention-1-n*2,"%02x",navigation.person()[n]);mention[33]=' ';mention[34]=0;}
            if (open) open(navigation.conversation(),mention); break;
        }
        case Choice::Mute: navigation.push(Page::Notifications); break;
        case Choice::Inbox: navigation.inbox(); break;
        case Choice::OpenDirect: {
            uint8_t destination[16];
            if(rs_handheld_lxmf_destination_for_identity(navigation.person(),destination)==RS_HANDHELD_OK && openDirect) {
                char hex[33];for(size_t n=0;n<16;++n) std::snprintf(hex+n*2,sizeof hex-n*2,"%02x",destination[n]);openDirect(hex);
            }
            else tell("Could not open this participant");break;
        }
        case Choice::MarkRead: if(open) open(navigation.conversation(),nullptr); break;
        default: break;
        }
        changed();
    }
    bool execute(const rrc::Command& command, const uint8_t* body = nullptr, size_t length = 0) {
        const auto code = send ? send(command,body,length) : rrc::Code::Offline;
        if (code != rrc::Code::Ok) tell(rrc::codeName(code));
        return code == rrc::Code::Ok;
    }
    bool dataPage() const { const auto p = navigation.page(); return p == Page::Hubs || p == Page::Channels || p == Page::People || p == Page::Directory || p == Page::SavedRooms || p == Page::Inbox; }
    bool detailPage() const {return navigation.page()==Page::RoomInfo || navigation.page()==Page::Notifications;}
    size_t textLine(size_t line,char* out=nullptr,size_t capacity=0) const {
        const auto* text=reinterpret_cast<const char*>(_rows);const auto length=strnlen(text,sizeof _rows);
        size_t begin=0;
        for(size_t n=0;begin<length;++n) {
            size_t end=std::min(begin+size_t(32),length);
            while(end<length && end>begin && (uint8_t(text[end])&0xc0)==0x80) --end;
            if(n==line) {if(out && capacity) {const auto bytes=std::min(end-begin,capacity-1);std::memcpy(out,text+begin,bytes);for(size_t i=0;i<bytes;++i) if(out[i]=='\n' || out[i]=='\r') out[i]=' ';out[bytes]=0;}return end;}
            begin=end;
        }
        return 0;
    }
    size_t textLines() const {size_t n=0;while(textLine(n)) ++n;return n;}
    bool failed() const { return (dataPage() || detailPage()) && _failed && _rowsView==navigation.revision(); }
    bool previousPage() const { return navigation.page()==Page::Inbox ? _newer : navigation.offset()!=0; }
    size_t pagerCount() const { return (previousPage() ? 1 : 0) + (_more ? 1 : 0); }
    void changed() { _count = 0; _more = false; _dirty = true; _failed=false; ++_renderRevision; }
    void tell(const char* text) { if (notice) notice(text); }
    static void copy(char* out, size_t capacity, const char* text) { if (capacity) std::snprintf(out,capacity,"%s",text ? text : ""); }
    void append(const rrc::HubView& value) {
        auto& row = _rows[_count++]; row = {}; std::memcpy(row.id,value.address,16);
        copy(row.name,sizeof row.name,value.name); row.state = value.saved;
        if (!row.name[0]) std::snprintf(row.name,sizeof row.name,"Hub %02x%02x%02x",row.id[0],row.id[1],row.id[2]);
    }
    void append(const rrc::RoomView& value) { auto& row = _rows[_count++]; row = {}; copy(row.name,sizeof row.name,value.name); row.unread = value.unread; row.state = uint8_t(value.phase); }
    void append(const rrc::DirectoryView& value) { auto& row = _rows[_count++]; row = {}; copy(row.name,sizeof row.name,value.name); }
    void append(const rrc::PersonView& value) { auto& row = _rows[_count++]; row = {}; copy(row.name,sizeof row.name,value.nickname); std::memcpy(row.id,value.identity,16); }
    void append(const rrc::PrivateView& value) {
        auto& row=_rows[_count++];row={};std::memcpy(row.id,value.identity,16);row.state=value.counter;row.unread=value.unread;
        std::snprintf(row.name,sizeof row.name,"%02x%02x%02x%02x:%02x%02x%02x%02x",row.id[0],row.id[1],row.id[2],row.id[3],row.id[4],row.id[5],row.id[6],row.id[7]);
    }
    Row _rows[Capacity]{};
    uint32_t _pending = 0, _rowsView = 0, _status = 0, _renderRevision = 1;
    uint8_t _count = 0, _previousCount = 0;
    bool _dirty = true, _visible = false, _more = false, _browsedInfo = false, _advancedRoom = false, _failed=false, _newer=false;
};
static_assert(sizeof(RrcBrowser) <= 1024, "One bounded metadata page and navigation bindings");
}
