#pragma once
#include "protocol/RrcTypes.h"
#include "protocol/RrcPreferences.h"
#include "protocol/RrcDirectory.h"
#include "protocol/RustLinkManager.h"
#include "storage/RrcRecord.h"

class MessageStore;
class RustClock;
class RustInterfacePump;

// One participant session on the existing protocol owner. No task, transcript
// cache or independent storage executor. Link handles and storage tickets own
// every asynchronous completion; UI receives copied views and generation fences.
class RustRrcEngine final : public RustLinkManager::RrcSink {
public:
    struct Deps {
        rs_handheld_rns_t* ctx = nullptr;
        RustClock* clock = nullptr;
        RustInterfacePump* pump = nullptr;
        RustLinkManager* links = nullptr;
        MessageStore* store = nullptr;
        const uint8_t* identity = nullptr;
    };
    void begin(const Deps&);
    void stop();
    void end();
    void loop();
    bool drained() const;
    void learn(const rs_handheld_announce_event_t&);
    handheld::rrc::Status status() const;
    size_t hubs(handheld::rrc::HubView*, size_t capacity) const;
    size_t rooms(handheld::rrc::RoomView*, size_t capacity) const;
    size_t people(const char* room, handheld::rrc::PersonView*, size_t capacity) const;
    size_t directory(handheld::rrc::DirectoryView*, size_t capacity, size_t offset = 0) const;
    handheld::rrc::Code command(const handheld::rrc::Command&, const uint8_t* text, size_t length);
    bool context(const uint8_t hub[16], const char* room, const uint8_t* participant,
                 handheld::storage::rrc::Context&) const;

    void onRrcPacket(RustLinkManager::Handle, const uint8_t*, size_t, const uint8_t[32]) override;
    void onRrcClosed(RustLinkManager::Handle) override;
    void onRrcTransmit(RustLinkManager::Handle, uint32_t token, bool started) override;

private:
    using Handle = RustLinkManager::Handle;
    using Code = handheld::rrc::Code;
    using Record = handheld::storage::rrc::Record;
    using Ticket = handheld::storage::Ticket;
    struct Hub {
        handheld::rrc::HubView view;
        uint8_t publicKey[64]{};
        uint64_t seen = 0;
        bool used = false;
    };
    struct Room {
        handheld::rrc::RoomView view;
        uint64_t deadline = 0;
        bool used = false, wanted = false;
    };
    struct Receive {
        Ticket ticket;
        Handle link;
        uint8_t hash[32]{}, file[16]{};
        uint32_t counter = 0, revision = 0;
        uint8_t room = UINT8_MAX;
        bool confirming = false, statusPending = false, proofPending = false, newRecord = false;
    };
    struct Send {
        Ticket ticket;
        uint8_t packet[handheld::rrc::PacketCapacity]{}, file[16]{}, conversation[16]{};
        size_t length = 0;
        uint64_t born = 0, deadline = 0;
        uint32_t token = 0, counter = 0, revision = 0, waitMs = 0;
        handheld::storage::rrc::Status status = handheld::storage::rrc::Status::Pending;
        bool admitted = false, started = false, statusDirty = false, statusWrite = false, statusRead = false, confirmed = false;
    };
    struct Control {
        uint8_t packet[handheld::rrc::PacketCapacity]{};
        size_t length = 0;
        uint64_t born = 0;
        uint32_t token = 0, waitMs = 0;
        uint8_t kind = 0, room = UINT8_MAX;
        bool admitted = false;
    };
    uint64_t now() const;
    uint32_t wait(uint32_t packets = 4) const;
    bool live(Handle) const;
    void changed();
    void notice(const char*);
    void recover(const char*);
    void disconnect();
    Room* findRoom(const char*);
    const Room* findRoom(const char*) const;
    Hub* findHub(const uint8_t[16]);
    void participant(const uint8_t[16], const uint8_t* nickname, size_t length, uint8_t room, bool remove = false);
    bool known(const uint8_t file[16]) const;
    void remember(const uint8_t file[16]);
    bool encode(uint64_t kind, const char* room, const uint8_t* body, size_t length,
                uint8_t bodyKind, const uint8_t* participant, uint8_t*, size_t&,
                const uint8_t* messageId = nullptr) const;
    Code control(uint8_t kind, const char* room, const uint8_t* body, size_t length, uint8_t bodyKind = 1);
    Code send(const handheld::rrc::Command&, const uint8_t*, size_t);
    void pollSend();
    void pollReceive();
    void applyRoomControl(const uint8_t*, size_t, const rs_handheld_rrc_view_t&, Room&);
    bool roomStatus(Room&, const uint8_t*, size_t);
    void wipeControl();
    enum class PreferenceStep : uint8_t { Idle, ReadRoot, WriteRoot, ReadHub, WriteHub, ReadJoin, WriteRoom, ReadMute, ReadForgetKey, WriteDraft, ReadUnread, MarkRead, ClearHistory };
    struct Preferences {
        Record record;
        Ticket ticket;
        PreferenceStep step = PreferenceStep::Idle;
        handheld::rrc::Action action = handheld::rrc::Action::SaveHub;
        uint32_t revision = 0, roomRevision = 0;
        uint8_t room = UINT8_MAX;
        bool write = false, ready = false, corrupt = false;
    };
    void pollPreferences();
    void preferenceDone(handheld::rrc::Code);
    void loadPreferences(PreferenceStep, const handheld::storage::rrc::Context&);
    void saveRoot(const handheld::rrc::SavedIndex&, const char* nickname = nullptr);
    void saveHubRooms();
    void joined(Room&);
    Code preferenceCommand(const handheld::rrc::Command&, const uint8_t*, size_t);
    Code startJoin(Room&, const uint8_t*, size_t, bool remember);

    Deps _d;
    handheld::rrc::Status _status;
    Hub _hubs[handheld::rrc::HubCapacity];
    Room _rooms[handheld::rrc::RoomCapacity];
    handheld::rrc::PersonView _people[handheld::rrc::PeopleCapacity];
    uint8_t _recent[handheld::rrc::DedupCapacity][16]{};
    size_t _recentCount = 0, _recentNext = 0;
    Receive _receive[2];
    Send _send;
    Control _control, _pong;
    Preferences _preferences;
    handheld::rrc::SavedIndex _bookmarks, _savedRooms;
    uint8_t _joinPreference[handheld::rrc::RoomPreferenceHeader + handheld::rrc::SealedKeyCapacity]{};
    uint16_t _joinPreferenceLength = 0;
    uint8_t _joinRoom = UINT8_MAX;
    uint8_t _invalidKeyRoom = UINT8_MAX;
    bool _joinConfirmed = false, _nicknameDirty = false, _joinFromStored = false;
    handheld::rrc::Directory _directory;
    Handle _link;
    uint64_t _deadline = 0, _retryAt = 0, _onlineAt = 0, _directoryDeadline = 0, _identifyBorn = 0;
    uint32_t _sequence = 0, _rateCount = 0, _rateLimit = 0;
    uint64_t _rateWindow = 0;
    uint8_t _interface = UINT8_MAX, _failures = 0;
    bool _stopped = true, _pathRequested = false;
};
static_assert(sizeof(RustRrcEngine) <= (STORAGE_ASYNC_WRITES ? 10240 : 18432),
              "Review session residency before adding RRC state");
