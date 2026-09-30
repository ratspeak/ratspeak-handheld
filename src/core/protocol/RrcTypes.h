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
enum class RoomPhase : uint8_t { Saved, Joining, Joined, Leaving, Recovering, NeedsKey, Error };
enum class Code : uint8_t { Ok, Busy, Stale, Offline, Invalid, TooLong, Unsupported, Full, Storage, NotJoined };
enum class SendPhase : uint8_t { Idle, Saving, Sending, Awaiting, Confirmed, Transmitted, NotSent, Unconfirmed };
enum class Action : uint8_t { Connect, Disconnect, Join, Leave, Message, PrivateNotice, Emote,
    Directory, Who, Topic, Nickname, Advanced, Retry, Mute, SaveHub, ForgetHub, Draft, ClearHistory, MarkRead };
struct Command {
    uint32_t generation = 0, revision = 0;
    uint8_t hub[16]{}, participant[16]{};
    char room[65]{};
    Action action = Action::Connect;
    uint8_t flags = 0; // remember key / enabled, defined by action
};
struct HubView {
    uint8_t address[16]{};
    char name[33]{};
    uint32_t ageSeconds = 0;
    uint8_t hops = 0, interface = UINT8_MAX;
    bool saved = false, reachable = false, active = false;
};
struct RoomView {
    char name[65]{};
    char topic[97]{}, modes[17]{};
    uint8_t key[16]{};
    uint32_t unread = 0, revision = 0;
    RoomPhase phase = RoomPhase::Saved;
    bool muted = false, membersComplete = false, needsKey = false;
    bool keyRemembered = false, unreadKnown = false;
    uint8_t registered = 0; // unknown=0, registered=1, unregistered=2
};
struct PersonView {
    uint8_t identity[16]{};
    char nickname[33]{};
    uint8_t rooms = 0;
};
struct DirectoryView { char name[65]{}, topic[97]{}; };
struct Status {
    uint32_t generation = 0, revision = 0, dropped = 0, sendRevision = 0;
    uint8_t hub[16]{}, identity[16]{};
    char name[33]{}, nickname[33]{}, notice[129]{};
    uint16_t bodyLimit = 350, roomLimit = 64;
    uint8_t nicknameLimit = 32, roomsLimit = RoomCapacity, capabilities = 0;
    Phase phase = Phase::Disconnected;
    SendPhase sending = SendPhase::Idle;
    bool busy = false, directoryPending = false, directoryPartial = false;
    uint32_t preferenceRevision = 0;
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
