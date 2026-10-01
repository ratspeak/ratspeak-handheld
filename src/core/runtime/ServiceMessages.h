#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include "storage/ConversationView.h"
#include "config/PropagationSettings.h"
#include "protocol/RrcTypes.h"

namespace handheld {

enum class Operation : uint8_t {
    Nodes, PeerName, ConversationPage, ConversationDetail, HistoryPage, ReadRecord, HistoryStatus, Settings, Identities, Scan, PropagationNodes,
    RrcHubs, RrcRooms, RrcChannels, RrcPeople, RrcDirectory, RrcContext, RrcHistoryPage, RrcHistoryRecord, RrcHistoryStatus, RrcDraft, RrcDetail, RrcSavedRooms, RrcInbox,
    Send, MarkRead, DeleteConversation, SaveContact, DeleteContact, RenameContact,
    ApplySettings, Announce, CreateIdentity, ImportIdentity, Diagnostics,
    PropagationSync, RrcCommand, HomeReady, Restart, PowerOff, SwitchIdentity, FormatSD, WipeSD, FactoryReset,
    EnableSDAndRestart, ClearOldDataAndRestart
};
enum class Admission : uint8_t { Admitted, Busy, NotReady, Invalid };
enum class Outcome : uint8_t { Ok, Failed, Stale, Invalid, NotReady, Cancelled };
enum class ServiceState : uint8_t { Starting, Running, Quiescing, Stopped, Failed };

constexpr bool lifecycleOperation(Operation op) {
    return op >= Operation::Restart;
}
constexpr bool queryOperation(Operation op) {
    return op <= Operation::RrcInbox;
}

struct Request {
    uint32_t id = 0;
    uint32_t generation = 0;
    uint32_t admittedAt = 0;
    uint32_t query = 0;
    uint32_t revision = 0;
    uint32_t statusRevision = 0;
    uint32_t offset = 0;
    uint32_t argument = 0;
    Operation operation = Operation::Nodes;
    char peer[33] = {};
    bool incoming = false; // HistoryPage boundary / ReadRecord stable selector.
    storage::HistoryDirection historyDirection = storage::HistoryDirection::Before;
};

// Summary selectors travel in the existing payload arena. Avoid enlarging every
// mailbox request just to carry one query's full double timestamp and options.
struct ConversationQuery {
    storage::ConversationSelector selector;
    storage::ConversationOrder order = storage::ConversationOrder::Recent;
    storage::ConversationDirection direction = storage::ConversationDirection::After;
    uint8_t hasCursor = 0;
    uint8_t reserved[5] = {};
};
static_assert(sizeof(ConversationQuery) == 40 && std::is_trivially_copyable<ConversationQuery>::value,
              "Conversation IPC uses a bounded copied envelope");

struct Result {
    Outcome outcome = Outcome::Failed;
    storage::RecordKey key;
    storage::Error storageError = storage::Error::None;
    uint32_t revision = 0;
    uint32_t statusRevision = 0;
    uint32_t next = 0;
    uint32_t total = 0;
    uint16_t length = 0;
    bool more = false;
    bool txSuppressed = false;
    bool settingsCommitted = false; // Survives a later UI snapshot/copy failure.
    uint8_t retainedOwners = 0; // Immutable maintenance failure snapshot.
    char detail[80] = {};
};

struct Status {
    propagation::SyncView propagation;
    rrc::Status rrc;
    uint32_t generation = 1;
    uint32_t storeRevision = 0;
    uint32_t historyRevision = 0; // Structural invalidation only; MAX is exhausted.
    uint32_t statusRevision = 0; // Outgoing network/status-persistence overlay.
    uint32_t nodeRevision = 0;
    uint32_t configRevision = 1;
    uint32_t identityRevision = 1;
    uint32_t incomingRevision = 0;
    uint32_t lastAnnounce = 0;
    uint32_t heartbeat = 0;
    uint32_t serviceMaxMs = 0;
    uint32_t stackFree = 0;
    uint32_t flashUsed = 0;
    uint32_t flashTotal = 0;
    uint32_t frequency = 0;
    uint32_t bandwidth = 0;
    uint32_t paths = 0;
    uint32_t links = 0;
    int32_t queued = 0;
    int32_t unread = 0;
    int16_t autoPeers = -1;
    uint8_t tcpUp = 0;
    uint8_t tcpTotal = 0;
    uint8_t sf = 0;
    int8_t txPower = 0;
    uint8_t resources = 0;
    ServiceState state = ServiceState::Starting;
    bool ready = false;
    bool transport = false;
    bool radio = false;
    bool lora = false;
    bool wifiEnabled = false;
    bool wifi = false;
    bool ap = false;
    bool sd = false;
    bool flash = false;
    bool scanRunning = false;
    bool gpsFix = false;
    char identity[33] = {};
    char identityHex[33] = {}; // Full identity; identity above is display-only.
    char destination[33] = {};
    char publicKey[129] = {};
    char notice[80] = {};
    uint32_t noticeRevision = 0;
    uint8_t maintenancePending = 0;
    uint8_t maintenanceFailedPending = 0;
    storage::Error maintenanceError = storage::Error::None;
};

static_assert(std::is_trivially_copyable<Request>::value, "Request must be a value record");
static_assert(std::is_trivially_copyable<Result>::value, "Result must be a value record");
static_assert(std::is_trivially_copyable<Status>::value && sizeof(Status) <= 1024,
              "Status must remain a bounded value snapshot");

} // namespace handheld
