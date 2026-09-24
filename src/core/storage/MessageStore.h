#pragma once

#include <Arduino.h>
#include "config/Config.h"
#include "storage/FlashStore.h"
#include "storage/SDStore.h"
#include "reticulum/LXMFMessage.h"
#include <vector>
#include <string>
#include <set>
#include <utility>
#include <array>
#include "storage/WriteQueue.h"
#include "storage/ConversationView.h"
namespace handheld::storage { class MessageTransactions; }

class MessageStore {
public:
    MessageStore();
    ~MessageStore();
    using Request = handheld::storage::Request;
    using Result = handheld::storage::Result;
    using Ticket = handheld::storage::Ticket;
    using Submission = handheld::storage::Submission;
    using RecordKey = handheld::storage::RecordKey;
    using StoredRecordHeader = handheld::storage::StoredRecordHeader;
    using HistoryEntry = handheld::storage::HistoryEntry;
    using HistoryDirection = handheld::storage::HistoryDirection;
    static constexpr size_t ConversationPageCapacity = STORAGE_ASYNC_WRITES ? 16 : 64;
    bool begin(FlashStore* flash, SDStore* sd = nullptr, bool externalStorageEnabled = false);
    bool setExternalStorageEnabled(bool enabled);
    bool externalStorageEnabled() const { return _externalStorageEnabled; }

    Submission requestSave(const LXMFMessage&, uint32_t identityGeneration = 0, uint32_t peerGeneration = 0);
    // Validated create metadata plus borrowed spans, copied into the owned slot
    // before return. No title/content allocation is required by the producer.
    Submission requestSave(const Request&, const void* title, const void* content);
    Submission requestStatus(const RecordKey&, LXMFStatus, uint32_t identityGeneration = 0, uint32_t peerGeneration = 0);
    Submission requestMarkRead(const std::string& peer, uint32_t identityGeneration = 0, uint32_t peerGeneration = 0);
    Submission requestDelete(const std::string& peer, uint32_t identityGeneration = 0, uint32_t peerGeneration = 0);
    Submission requestRecord(const RecordKey&, uint32_t offset = 0, uint16_t capacity = 4096);
    Submission requestPending(const RecordKey& after = {});
    Submission requestHistoryPage(const std::string& peer, HistoryEntry cursor = {}, uint8_t limit = 48,
                                  HistoryDirection direction = HistoryDirection::Before);
    Submission requestConversationPage(handheld::storage::ConversationCursor cursor = {}, bool hasCursor = false,
        handheld::storage::ConversationOrder order = handheld::storage::ConversationOrder::Recent,
        handheld::storage::ConversationDirection direction = handheld::storage::ConversationDirection::After,
        uint8_t limit = ConversationPageCapacity);
    Submission requestConversation(const handheld::storage::ConversationSelector&);
    Submission requestTrim(const std::string& peer);
    void poll();
    bool peekResult(Ticket, Result&, Request* request = nullptr) const;
    bool readPayload(Ticket, void*, size_t length, size_t offset = 0) const;
    bool releaseResult(Ticket);
    bool cancel(Ticket ticket) { return _writeQueue.cancel(ticket); }
    bool adoptOwner() { return _writeQueue.adoptOwner(); }
    void requestStop() { _writeQueue.requestStop(); }
    bool finishStop();

    uint32_t totalConversations() const { return _totalConversations; }

    bool saveMessage(LXMFMessage& msg);
    int messageCount(const std::string& peerHex) const;
    bool deleteConversation(const std::string& peerHex);
    std::vector<std::string> startupRecentMessageIds(size_t maxIds) const;
    // Outgoing recovery uses typed metadata queries. Only the bounded optional
    // recent-ID hint is retained until the backend has consumed it.
    void releaseStartupSeeds() {
        _startupRecentMessageIds.clear();
        _startupRecentMessageIds.shrink_to_fit();
    }
    bool markConversationRead(const std::string& peerHex);
    bool updateMessageStatus(const std::string& peerHex, double timestamp, bool incoming, LXMFStatus newStatus);
    bool updateMessageStatusByCounter(const std::string& peerHex, uint32_t counter, bool incoming, LXMFStatus newStatus);

    int totalUnreadCount() const;
    uint32_t revision() const { return _revision; }
    // Structural history changes only; MAX means freshness cannot be inferred
    // by equality. Adapters must expose unavailable freshness/explicit refresh.
    uint32_t historyRevision() const { return _historyRevision; }
    bool historyRevisionExhausted() const { return _historyRevision == UINT32_MAX; }

    // Diagnostics accessor (HEART writeQ token)
    bool deferredIO() const { return _writeQueue.deferred(); }
    WriteQueue& writeQueue() { return _writeQueue; }

private:
    bool loadStartupMetadata();
    void bumpRevision();
    Submission submit(Request, const WriteQueue::PayloadPart* = nullptr, size_t count = 0, size_t capacity = 0);
    bool consumeImmediate(Submission, Result&);
    void settle(const Request&, const Result&) noexcept;

    FlashStore* _flash = nullptr;
    SDStore* _sd = nullptr;
    bool _externalStorageEnabled = false;
    std::vector<std::string> _startupRecentMessageIds;
    // Boot-seed collection bounds (consumers take <=100 ids / <=queue-cap pending)
    static constexpr size_t STARTUP_RECENT_CAP = 128;
    WriteQueue _writeQueue;
    handheld::storage::MessageTransactions& transactions() const;
    alignas(std::max_align_t) mutable uint8_t _transactionState[64 +
        handheld::storage::Budget::conversationSummaryBytes(WriteQueue::CompactProfile)];
    Ticket _deleteFence;
    uint8_t _fencedPeer[16] = {};
    uint64_t _settledThrough = 0;
    uint32_t _totalConversations = 0;
    int _totalUnread = 0;
    uint32_t _revision = 0;
    uint32_t _historyRevision = 0;
};

static_assert(sizeof(MessageStore) - sizeof(WriteQueue) <= 320 +
              handheld::storage::Budget::conversationSummaryBytes(WriteQueue::CompactProfile),
              "MessageStore fixed state exceeds its descriptor allowance");
static_assert(handheld::storage::Budget::StoreBookkeeping >= 320 + 56 + 84,
              "Store state, queue metadata and filesystem mutex need explicit accounting");
