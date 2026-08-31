#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace handheld {

enum class Operation : uint8_t {
    Nodes, Conversations, History, MessageBody, Settings, Identities, Scan,
    Send, MarkRead, DeleteConversation, SaveContact, DeleteContact, RenameContact,
    ApplySettings, Announce, CreateIdentity, ImportIdentity, Diagnostics,
    HomeReady, CloseHistory, Restart, PowerOff, SwitchIdentity, FormatSD, WipeSD, FactoryReset,
    EnableSDAndRestart, ClearOldDataAndRestart
};
enum class Admission : uint8_t { Admitted, Busy, NotReady, Invalid };
enum class Outcome : uint8_t { Ok, Failed, Stale, Invalid, NotReady };
enum class ServiceState : uint8_t { Starting, Running, Quiescing, Stopped, Failed };

constexpr bool lifecycleOperation(Operation op) {
    return op >= Operation::Restart;
}
constexpr bool queryOperation(Operation op) {
    return op <= Operation::Scan;
}

struct Request {
    uint32_t id = 0;
    uint32_t generation = 0;
    uint32_t query = 0;
    uint32_t revision = 0;
    uint32_t offset = 0;
    uint32_t argument = 0;
    Operation operation = Operation::Nodes;
    char peer[33] = {};
};

struct Result {
    Outcome outcome = Outcome::Failed;
    uint32_t revision = 0;
    uint32_t next = 0;
    uint32_t total = 0;
    uint16_t length = 0;
    bool more = false;
    char detail[80] = {};
};

struct Status {
    uint32_t generation = 1;
    uint32_t storeRevision = 0;
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
};

static_assert(std::is_trivially_copyable<Request>::value, "Request must be a value record");
static_assert(std::is_trivially_copyable<Result>::value, "Result must be a value record");
static_assert(std::is_trivially_copyable<Status>::value && sizeof(Status) <= 1024,
              "Status must remain a bounded value snapshot");

} // namespace handheld
