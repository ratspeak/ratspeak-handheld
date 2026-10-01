#pragma once
#include <cstddef>
#include <cstdint>

namespace handheld::rrc {
#if STORAGE_ASYNC_WRITES
constexpr size_t HubCapacity = 8, RoomCapacity = 4, PeopleCapacity = 24, DedupCapacity = 64;
#else
constexpr size_t HubCapacity = 16, RoomCapacity = 8, PeopleCapacity = 64, DedupCapacity = 128;
#endif
constexpr size_t PacketCapacity = 431, DraftCapacity = 400, KeyCapacity = 160;
constexpr size_t HubViewCapacity = HubCapacity * 2;
enum class Phase : uint8_t { Disconnected, Finding, Connecting, Identifying, Greeting, Online, Recovering };
enum class RoomPhase : uint8_t { Saved, Joining, Joined, Leaving, Recovering, NeedsKey, Error, Available };
enum class Code : uint8_t { Ok, Busy, Stale, Offline, Invalid, TooLong, Unsupported, Full, Storage, NotJoined };
enum class SendPhase : uint8_t { Idle, Saving, Sending, Awaiting, Confirmed, Transmitted, NotSent, Unconfirmed };
enum class Action : uint8_t { Connect, Disconnect, Join, Leave, Message, PrivateNotice, Emote,
    Directory, Who, Topic, Nickname, Advanced, Retry, Mute, SaveHub, ForgetHub, Draft, ClearHistory, MarkRead, ForgetRoom, ForgetKey };
// A local view binding. Empty room/participant means hub notices. Participant
// notices have their own history and never become a Direct conversation.
struct Conversation {
    uint8_t hub[16]{}, participant[16]{};
    char room[65]{};
    bool privateNotice() const {
        uint8_t any = 0; for (auto byte : participant) any |= byte; return any != 0;
    }
};
struct DraftView { uint32_t revision = 0, storageRevision = 0; uint16_t length = 0; bool uncertain = false; char text[DraftCapacity + 1]{}; };
struct MessageDetail {
    uint8_t source[16]{};
    uint32_t counter = 0, timestamp = 0;
    char nickname[33]{};
    uint16_t length = 0;
    uint8_t kind = 0, status = 0;
    bool invitation = false; // Exact hub-source, room-context invitation grammar.
    char text[PacketCapacity + 1]{};
};
inline const char* codeName(Code code) {
    switch (code) {
    case Code::Ok: return "";
    case Code::Busy: return "Hub busy; try again";
    case Code::Stale: return "Hub changed; reopen this view";
    case Code::Offline: return "Hub unavailable";
    case Code::Invalid: return "Check the entered value";
    case Code::TooLong: return "Too long for this hub or radio packet";
    case Code::Unsupported: return "Hub does not support this action";
    case Code::Full: return "Device limit reached";
    case Code::Storage: return "Could not save; try again";
    case Code::NotJoined: return "Join this channel first";
    }
    return "Hub action failed";
}
struct Command {
    uint32_t generation = 0, revision = 0, counter = 0; // MarkRead uses counter; revision identifies the request.
    uint8_t hub[16]{}, participant[16]{};
    char room[65]{};
    Action action = Action::Connect;
    uint8_t flags = 0; // remember key / enabled; Draft bit0=CAS using counter, bit1=durable send intent
};
struct HubView {
    uint8_t address[16]{};
    char name[33]{};
    uint32_t ageSeconds = 0;
    uint8_t hops = 0, interface = UINT8_MAX;
    bool saved = false, reachable = false, active = false;
};
// Persisted HRP1 values retain the original 0=All / 1=Muted meaning.
enum class Notifications : uint8_t { All=0, Muted=1, Mentions=2 };
enum class Speaking : uint8_t { Unknown, Allowed, Denied };
enum class Restriction : uint8_t { None, Key, InviteOnly, Kicked, Banned, Missing };
inline const char* restrictionName(Restriction value) {
    switch(value) {
    case Restriction::Key: return "Channel key rejected";
    case Restriction::InviteOnly: return "Fresh invitation may be needed";
    case Restriction::Kicked: return "Removed by hub; rejoin manually";
    case Restriction::Banned: return "Banned from this channel";
    case Restriction::Missing: return "Channel unavailable";
    default: return "No observed join restriction";
    }
}
inline const char* notificationName(Notifications value) {
    return value==Notifications::Muted ? "Muted" : value==Notifications::Mentions ? "Mentions" : "All";
}
struct RoomView {
    char name[65]{};
    char topic[97]{}, modes[17]{};
    uint8_t key[16]{};
    uint32_t unread = 0, revision = 0;
    RoomPhase phase = RoomPhase::Saved;
    Notifications notifications = Notifications::All;
    bool muted = false, membersComplete = false, needsKey = false;
    bool keyRemembered = false, unreadKnown = false;
    uint8_t registered = 0; // unknown=0, registered=1, unregistered=2
    Speaking speaking = Speaking::Unknown;
    Restriction restriction = Restriction::None;
};
struct PersonView {
    uint8_t identity[16]{};
    char nickname[33]{};
    uint8_t rooms = 0;
};
struct PrivateView {
    uint8_t identity[16]{};
    uint32_t counter=0, unread=0;
};
struct DirectoryView { char name[65]{}, topic[97]{}; };
struct Status {
    uint32_t generation = 0, revision = 0, dropped = 0, sendRevision = 0, alertRevision = 0;
    uint8_t hub[16]{}, identity[16]{};
    char name[33]{}, nickname[33]{}, notice[129]{};
    uint16_t bodyLimit = 350, roomLimit = 64;
    uint8_t nicknameLimit = 32, roomsLimit = RoomCapacity, capabilities = 0;
    Phase phase = Phase::Disconnected;
    SendPhase sending = SendPhase::Idle;
    bool busy = false, directoryPending = false, directoryPartial = false;
    bool directoryKnown = false, directoryStale = false, directoryFailed = false;
    uint8_t directoryCount = 0;
    bool sendSaved = false, sendSettled = true;
    uint32_t preferenceRevision = 0;
    uint32_t preferenceRecordRevision = 0;
    Code preferenceResult = Code::Ok;
};
inline const char* phaseName(Phase phase) {
    switch (phase) {
    case Phase::Disconnected: return "Disconnected";
    case Phase::Finding: return "Finding hub";
    case Phase::Connecting: return "Connecting";
    case Phase::Identifying: return "Identifying";
    case Phase::Greeting: return "Waiting for hub";
    case Phase::Online: return "Connected";
    case Phase::Recovering: return "Reconnecting";
    }
    return "Offline";
}
}
