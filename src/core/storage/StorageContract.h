#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include "AlignedStorageMemory.h"

namespace handheld::storage {

// Admission owns its payload and terminal-result credit until the owner releases
// the ticket. This contract is independent of immediate/deferred execution.
struct Ticket {
    uint64_t sequence = 0;
    uint8_t slot = UINT8_MAX;
    constexpr bool valid() const { return sequence != 0 && slot != UINT8_MAX; }
};
constexpr bool operator==(Ticket a, Ticket b) {
    return a.sequence == b.sequence && a.slot == b.slot;
}
constexpr bool operator!=(Ticket a, Ticket b) { return !(a == b); }

enum class Operation : uint8_t {
    CreateIncoming, CreateOutgoing, UpdateStatus, MarkRead, DeleteConversation,
    ReadRecord, ReadHistoryPage, ReadPending, ReadConversationPage, ReadConversation, Trim
};
enum class Rejection : uint8_t {
    None, Busy, Invalid, TooLarge, NoMemory, Unavailable, Fenced, Exhausted
};
enum class Outcome : uint8_t { Committed, Failed, Cancelled };
enum class Error : uint8_t {
    None, Unavailable, Allocation, InvalidRecord, CounterExhausted,
    RevisionExhausted, Read, Write, Verify, Rename, Stale, Cancelled, Internal
};
enum class State : uint8_t { Free, Queued, CancelQueued, Running, Ready, Retired };
enum class HistoryDirection : uint8_t { Before, After };
enum class ConversationOrder : uint8_t { Recent, Peer };
enum class ConversationDirection : uint8_t { Before, After };

struct Submission {
    Ticket ticket;
    Rejection rejection = Rejection::Unavailable;
    constexpr bool accepted() const { return rejection == Rejection::None && ticket.valid(); }
    // Deliberately no bool conversion: accepted never means durably committed.
};

struct RecordKey {
    uint8_t peer[16] = {};
    uint32_t counter = 0;
    bool incoming = false;
};

// Scalars are copied at admission. Title/content occupy the slot's separate
// bounded byte buffer, with explicit lengths (embedded NUL bytes are preserved).
struct Request {
    // ReadPending: exclusive after (counter, peer), outgoing only.
    // ReadHistoryPage: peer plus exclusive (counter, incoming) in historyDirection.
    // Counter zero starts Before at newest, After at oldest; never discard a
    // nonzero cursor's tie-breaker. ReadPending also starts at counter zero.
    RecordKey key;
    uint32_t identityGeneration = 0;
    uint32_t peerGeneration = 0;
    uint32_t offset = 0; // ReadRecord byte offset; not a query cursor
    uint8_t source[16] = {};
    uint8_t destination[16] = {};
    uint8_t messageId[32] = {};
    double timestamp = 0;
    uint16_t titleLength = 0;
    uint16_t contentLength = 0;
    uint16_t readCapacity = 0;
    Operation operation = Operation::CreateIncoming;
    uint8_t status = 0; // persisted LXMFStatus numeric value
    bool read = false;
    bool hasMessageId = false;
    HistoryDirection historyDirection = HistoryDirection::Before;
    // Conversation pages use (timestamp, key.peer), while a detail query binds
    // that timestamp to key.counter/incoming. These scalars reuse request space.
    ConversationOrder conversationOrder = ConversationOrder::Recent;
    ConversationDirection conversationDirection = ConversationDirection::After;
    bool hasConversationCursor = false;
};

struct MediumResult {
    bool attempted = false;
    bool committed = false;
    Error error = Error::None;
};

// The owner applies deltas in execution order before publishing a ready result.
// Consumers may only inspect their own ticket; release invalidates its view.
// Read results use the same owned payload bytes, never a borrowed File/String.
struct Result {
    // Query continuation: selected pending record; oldest history entry for
    // Before, newest for After. An empty query result has counter zero (terminal,
    // not a cursor to submit again unless restarting). Both history directions
    // return ascending (counter, incoming); more refers only to that direction.
    // Pending records ascend in (counter, peer). History total counts all visible records.
    RecordKey key;
    uint32_t revision = 0;
    uint32_t nextOffset = 0; // ReadRecord byte continuation only
    uint32_t total = 0;
    int32_t unreadDelta = 0;
    int32_t countDelta = 0;
    int8_t conversationDelta = 0;
    uint16_t length = 0;
    uint8_t oldStatus = 0;
    uint8_t newStatus = 0;
    Outcome outcome = Outcome::Failed;
    Error error = Error::None;
    MediumResult flash;
    MediumResult sd;
    bool duplicate = false;
    bool more = false;
};

// A bounded history-page payload contains these complete record selectors.
struct HistoryEntry {
    uint32_t counter = 0;
    bool incoming = false;
};
static_assert(sizeof(HistoryEntry) == 8, "History page entry budget changed");

// Every ReadRecord response starts with this fixed header, followed by the
// requested slice of title||content. Lengths and offsets count bytes, including
// embedded NUL. No JSON or full legacy body is copied into the mailbox.
struct StoredRecordHeader {
    uint8_t source[16] = {}, destination[16] = {}, messageId[32] = {};
    double timestamp = 0;
    uint32_t counter = 0, revision = 0, titleLength = 0, contentLength = 0;
    uint8_t status = 0;
    bool incoming = false, read = false, hasMessageId = false;
};
static_assert(sizeof(StoredRecordHeader) <= 104, "Stored record response metadata grew");

struct Slot {
    Request request;
    Result result;
    uint64_t sequence = 0;
    std::atomic<State> state{State::Free};
};

struct Budget {
    static constexpr size_t NormalSlots = 4;
    static constexpr size_t ReservedSlots = 2;
    static constexpr size_t SlotCount = NormalSlots + ReservedSlots;
    static constexpr size_t SmallPayload = 512;
    static constexpr size_t LargePayload = 4096;
    static constexpr size_t MaxMessageBody = 3659;
    static constexpr size_t MaxJsonFile = 32768;
    static constexpr size_t LegacyMetadataAllowance = 256;
    static constexpr size_t MaxStoredFile = MaxJsonFile + LegacyMetadataAllowance;
    static constexpr size_t MaxNewJson = 6 * MaxMessageBody + 512;
    static constexpr size_t CardPayloadBytes = 3 * SmallPayload + LargePayload;
    static constexpr size_t LargeBoardPayloadBytes = NormalSlots * LargePayload;
    static constexpr size_t SlotBytes = SlotCount * sizeof(Slot);
    static constexpr size_t SlotCeiling = 1536;
    // Owned ArduinoJson strings retain explicit length (linked variants do not
    // preserve embedded NULs). Includes one <=3659-byte compact body copy.
    static constexpr size_t JsonAllocator = 7168;
    static constexpr size_t IoScratch = 1024;
    static constexpr size_t WorkerStack = 8192;
    static constexpr size_t RtosBookkeeping = 640; // Worker queue/task costs and reserve
    // Both profiles: MessageStore state216 + queue metadata56 + FS mutex84,
    // plus156 bytes for allocator/descriptor reserve (measured Xtensa sizes),
    // including the normal arena's aligned-allocation padding/pointer.
    static constexpr size_t StoreBookkeeping = 512;
    // Derived conversation summaries, owned by the storage executor. No full
    // messages or paths; every mutation invalidates the affected peer.
    static constexpr size_t ConversationSummaryBytes = 88;
    static constexpr size_t conversationSummaryCapacity(bool compact) { return compact ? 4 : 16; }
    static constexpr size_t conversationSummaryBytes(bool compact) {
        return ConversationSummaryBytes * conversationSummaryCapacity(compact);
    }
    static constexpr size_t IncomingRows = 4 * 80;
    static constexpr size_t ProofContexts = 12 * 160;
    static constexpr size_t CardHeapFloor = 12288;
    static constexpr size_t CardLargestBlockFloor = 8192;
    // ArduinoJson7 parses strings by ownership even from mutable input. Reserve
    // one arena and stream the file; no complete input/output JSON copy.
    static constexpr size_t LegacyScratch = 56 * 1024;
    static constexpr size_t LegacyAllocation = LegacyScratch + AlignedStorageOverhead;

    static constexpr size_t payloadCapacity(bool cardputer, uint8_t slot) {
        return slot >= NormalSlots ? 0 :
            (cardputer && slot < NormalSlots - 1 ? SmallPayload : LargePayload);
    }
    static constexpr bool validBody(size_t title, size_t content) {
        return title <= MaxMessageBody && content <= MaxMessageBody - title;
    }
    static constexpr size_t storageWorkingSet(bool cardputer) {
        return (cardputer ? CardPayloadBytes : LargeBoardPayloadBytes) + SlotCeiling +
               JsonAllocator + IoScratch + StoreBookkeeping + WorkerStack + RtosBookkeeping +
               IncomingRows + ProofContexts + conversationSummaryBytes(cardputer);
    }
    static constexpr bool canReserveLegacy(size_t freeBytes, size_t largestBlock) {
        return freeBytes >= LegacyAllocation + CardHeapFloor && largestBlock >= LegacyAllocation;
    }
};

static_assert(sizeof(Slot) <= 256, "Storage slot descriptor/result exceeds its byte budget");
static_assert(Budget::SlotBytes <= Budget::SlotCeiling, "Storage slots exceed their pool budget");
static_assert(Budget::storageWorkingSet(true) == 27296, "Review Cardputer storage budget changes");
static_assert(Budget::storageWorkingSet(false) == 39104, "Review Deck/Pager storage budget changes");
static_assert(Budget::MaxNewJson <= Budget::MaxJsonFile, "New records must fit the retained schema");
static_assert(std::is_trivially_copyable<Request>::value && std::is_trivially_copyable<Result>::value,
              "Storage requests/results cannot retain heap or callback ownership");

} // namespace handheld::storage
