#pragma once

#include "protocol/RrcTypes.h"
#include <cstring>

namespace handheld::ui {

// Presentation state only. Navigating never calls the protocol owner. Both
// native renderers dispatch the same explicitly selected actions separately.
class RrcNavigation {
public:
    enum class Page : uint8_t { Hubs, Channels, ActiveHub, Hub, Room, People, Person, Directory,
        Address, RoomName, RoomKey, Nickname, Advanced, SwitchHub, Disconnect, Leave,
        ClearHistory, HubInfo, RoomInfo, Identity, Help, Inbox, SavedRooms };
    enum class Choice : uint8_t { Connect, SaveHub, ForgetHub, BrowseHubs, HubInfo, Notices, Disconnect,
        JoinByName, Directory, SavedRooms, Inbox, Nickname, Advanced, Help, OpenChat,
        Join, JoinWithKey, Leave, People, RoomInfo, Mute, MarkRead, ClearHistory,
        Mention, PrivateNotice, Identity, OpenDirect, Refresh, Back, Confirm };
    struct Item { Choice choice; const char* label; };
    static constexpr Item HubActions[] = {
        {Choice::BrowseHubs,"Browse other hubs"}, {Choice::HubInfo,"Hub details"},
        {Choice::Notices,"Hub notices"}, {Choice::Inbox,"Private notices"},
        {Choice::Nickname,"My nickname"}, {Choice::Advanced,"Advanced command"},
        {Choice::Help,"Help"}, {Choice::Disconnect,"Disconnect hub"},
        {Choice::SavedRooms,"Saved channels"}, {Choice::Back,"Back"}
    };
    static constexpr Item RoomActions[] = {
        {Choice::OpenChat,"Open conversation"}, {Choice::Join,"Join channel"},
        {Choice::JoinWithKey,"Join with key"}, {Choice::People,"Known people"},
        {Choice::RoomInfo,"Channel info"}, {Choice::Mute,"Notifications"},
        {Choice::MarkRead,"Mark read"}, {Choice::Advanced,"Advanced command"},
        {Choice::Leave,"Leave channel"}, {Choice::ClearHistory,"Clear local history"}, {Choice::Back,"Back"}
    };
    static constexpr Item PersonActions[] = {
        {Choice::Mention,"Mention"}, {Choice::PrivateNotice,"Private notice"},
        {Choice::Identity,"Full identity"}, {Choice::OpenDirect,"Open LXMF"}, {Choice::Back,"Back"}
    };

    Page page() const { return _page; }
    uint32_t revision() const { return _revision; }
    uint32_t session() const { return _session; }
    bool direct() const { return _direct; }
    bool connected() const { return _phase != rrc::Phase::Disconnected; }
    const char* secondLabel() const { return connected() ? "Channels" : "Hubs"; }
    const rrc::Conversation& conversation() const { return _conversation; }
    const uint8_t* selectedHub() const { return _selectedHub; }
    const char* hubName() const { return _hubName; }
    const char* personName() const { return _personName; }
    const uint8_t* person() const { return _person; }
    size_t offset() const { return _offset; }
    bool after() const { return _after; }
    size_t selected() const { return _selected; }
    void select(size_t index) { _selected = index; }
    void pageOffset(size_t offset) { _offset = offset; _selected = 0; _after=false; changed(); }
    void pageCursor(uint32_t cursor,bool after) { _offset=cursor;_selected=0;_after=after;changed(); }
    void direct(bool value) { if (_direct != value) { _direct = value; changed(); } }
    // Identity changes clear every browse target. A reconnect only updates the
    // active state; it cannot move the user out of a browser or open a dialog.
    void observe(const rrc::Status& status, uint32_t identityGeneration) {
        if (_identity != identityGeneration) {
            const auto previous = _revision;
            *this = {}; _revision = previous; _identity = identityGeneration; changed();
        }
        const bool sessionChanged = _session != status.generation || std::memcmp(_activeHub, status.hub, 16);
        const bool wasConnected = connected();
        _session = status.generation; _phase = status.phase;
        std::memcpy(_activeHub, status.hub, 16);
        if (_page == Page::Hubs && !_depth && connected()) root();
        else if (_page == Page::Channels && !connected()) root();
        if (sessionChanged || wasConnected != connected()) changed();
    }
    void root() {
        _page = connected() ? Page::Channels : Page::Hubs;
        _depth = 0; _offset = 0; _selected = 0; _after=false; changed();
    }
    bool push(Page page) {
        if (_depth == sizeof _back / sizeof _back[0] || _revision == UINT32_MAX) return false;
        if (_offset>UINT32_MAX || (_selected!=SIZE_MAX && _selected>=UINT16_MAX)) return false;
        _back[_depth++] = {uint32_t(_offset), uint16_t(_selected), _page, _after};
        _page = page; _offset = _selected = 0; _after=false; changed(); return true;
    }
    void back() {
        if (!_depth) { root(); return; }
        const auto frame = _back[--_depth]; _page = frame.page; _offset = frame.offset;
        _selected = frame.selected==UINT16_MAX ? SIZE_MAX : frame.selected;
        _after=frame.after;
        if (!_depth && (_page == Page::Hubs || _page == Page::Channels)) _page = connected() ? Page::Channels : Page::Hubs;
        changed();
    }
    bool browseHubs() { return push(Page::Hubs); }
    bool chooseHub(const rrc::HubView& hub) {
        if (!push(Page::Hub)) return false;
        std::memcpy(_selectedHub, hub.address, 16); copy(_hubName, hub.name); return true;
    }
    bool activeHub(const rrc::Status& status) {
        if (std::memcmp(status.hub, _activeHub, 16) || !connected()) return false;
        return push(Page::ActiveHub);
    }
    bool chooseRoom(const uint8_t hub[16], const char* room) {
        if (!hub || !room || !*room || strnlen(room, 65) > 64 || !push(Page::Room)) return false;
        // The target can be the saved-list binding itself. Copy before clearing.
        uint8_t target[16];std::memcpy(target,hub,16);
        _conversation = {}; std::memcpy(_conversation.hub, target, 16); copy(_conversation.room, room);
        _conversationSession = _session; return true;
    }
    bool savedRooms(bool selectedHub=false) {
        if (!push(Page::SavedRooms)) return false;
        _conversation={};std::memcpy(_conversation.hub,selectedHub?_selectedHub:_activeHub,16);
        _conversationSession=_session;return true;
    }
    bool inbox(bool selectedHub=false) {
        if(!push(Page::Inbox)) return false;
        _conversation={};std::memcpy(_conversation.hub,selectedHub?_selectedHub:_activeHub,16);
        _conversationSession=_session;return true;
    }
    bool conversationCurrent() const {
        return _conversationSession==_session && !std::memcmp(_conversation.hub,_activeHub,16);
    }
    bool choosePerson(const rrc::PersonView& person) {
        if (!push(Page::Person)) return false;
        std::memcpy(_person, person.identity, 16); copy(_personName, person.nickname); return true;
    }
    rrc::Conversation privateConversation() const {
        rrc::Conversation value; std::memcpy(value.hub, _conversation.hub, 16);
        std::memcpy(value.participant, _person, 16); return value;
    }
    // The returned value is an intent, not an execution. Confirmations name the
    // selected target, while active hub actions always bind the observed hub.
    rrc::Command command(rrc::Action action, bool selectedHub = false) const {
        rrc::Command value; value.action = action; value.generation = _session;
        std::memcpy(value.hub, selectedHub ? _selectedHub : _activeHub, 16);
        if (action == rrc::Action::Join || action == rrc::Action::Leave || action == rrc::Action::Who ||
            action == rrc::Action::Topic || action == rrc::Action::Mute || action == rrc::Action::ClearHistory ||
            action == rrc::Action::MarkRead || action == rrc::Action::Message || action == rrc::Action::Emote) {
            copy(value.room, _conversation.room);
            std::memcpy(value.hub, _conversation.hub, 16); value.generation = _conversationSession;
        }
        return value;
    }
    bool current(uint32_t view, uint32_t session, uint32_t identity) const {
        return view == _revision && session == _session && identity == _identity;
    }

private:
    template<size_t N> static void copy(char (&out)[N], const char* in) {
        const size_t n = in ? strnlen(in, N - 1) : 0;
        std::memset(out, 0, N); if (n) std::memcpy(out, in, n);
    }
    void changed() { if (_revision != UINT32_MAX) ++_revision; }
    struct Frame { uint32_t offset = 0; uint16_t selected = 0; Page page = Page::Hubs; bool after=false; };
    Frame _back[8]{};
    rrc::Conversation _conversation;
    uint8_t _activeHub[16]{}, _selectedHub[16]{}, _person[16]{};
    char _hubName[33]{}, _personName[33]{};
    uint32_t _revision = 1, _identity = 0, _session = 0, _conversationSession = 0;
    size_t _offset = 0, _selected = 0;
    Page _page = Page::Hubs;
    rrc::Phase _phase = rrc::Phase::Disconnected;
    uint8_t _depth = 0;
    bool _direct = true, _after=false;
};
static_assert(sizeof(RrcNavigation) <= 384, "Navigation retains bindings, never message bodies or node snapshots");
}
