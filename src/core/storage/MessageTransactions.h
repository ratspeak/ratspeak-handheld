#pragma once

#include "FlashStore.h"
#include "SDStore.h"
#include "WriteQueue.h"
#include "MessageRecord.h"
#include "StorageLease.h"
#include "ConversationView.h"
#include <vector>
#include <string>
#include <utility>

namespace handheld::storage {

// Storage-only semantic executor. All record reads, revisions, counters and
// medium selection happen under one filesystem lease on the executor task.
class MessageTransactions final : public WriteQueue::Executor {
public:
    bool begin(FlashStore* flash, SDStore* sd, bool external, bool deferred);
    void execute(const Request&, uint8_t*, size_t length, size_t capacity, Result&) override;
    void setExternal(bool enabled) { _external = enabled; clearSummaries(); } // owner, quiescent only
    bool external() const { return _external; }

    struct Cursor {
        File directory;
        uint8_t peer[16] = {};
        uint32_t cutoff = 0;
        uint8_t medium = 0;
        bool opened = false;
        Error error = Error::None;
    };
    // Caller holds an explicit filesystem lease. No retained filename vector.
    void beginRecords(Cursor&, const uint8_t peer[16], MessageDocument* scratch = nullptr);
    bool nextRecord(Cursor&, RecordKey&);
    bool nextPeer(Cursor&, char peerHex[33]); // unordered unique peers, one pass per root
    bool nextPeer(const char* afterHex, char peerHex[33], Error&);
    Error load(const RecordKey&, MessageDocument&, StoredRecordHeader&);
    uint32_t deletedThrough(const uint8_t peer[16]);
    uint32_t nextCounter() const { return _nextCounter; }
    uint64_t mutationEpoch() const { return _mutationEpoch; } // caller holds lease
    using RecentIds = std::vector<std::pair<uint32_t, std::string>>;
    // Shared aggregate semantics. Optional bounded startup hints
    // run on the boot owner only; ordinary queries pass no heap-owning outputs.
    Error summarize(const uint8_t peer[16], ConversationView&, ConversationSelector&,
        RecentIds* recent = nullptr, size_t recentCapacity = 0, uint32_t* latestRevision = nullptr);

private:
    struct Summary {
        ConversationView row;
        uint32_t counter = 0, revision = 0, used = 0;
        bool incoming = false;
    };
    static_assert(sizeof(Summary) == Budget::ConversationSummaryBytes, "Review summary retention budget");
    static constexpr size_t SummaryCapacity = Budget::conversationSummaryCapacity(WriteQueue::CompactProfile);
    void clearSummaries();
    void invalidateSummary(const uint8_t peer[16]);
    Summary* findSummary(const uint8_t peer[16]);
    void rememberSummary(const ConversationView&, const ConversationSelector&, uint32_t revision);
    void checkSummaryMedia();
    struct Medium {
        FlashStore* flash = nullptr;
        SDStore* sd = nullptr;
        bool isReady() const { return flash ? flash->isReady() : sd && sd->isReady(); }
        bool exists(const char* path) { return flash ? flash->exists(path) : sd && sd->exists(path); }
        bool remove(const char* path) { return flash ? flash->remove(path) : sd && sd->remove(path); }
        bool rename(const char* from, const char* to) { return flash ? flash->rename(from, to) : sd && sd->rename(from, to); }
        bool ensureDir(const char* path) { return flash ? flash->ensureDir(path) : sd && sd->ensureDir(path); }
        File open(const char* path, const char* mode = "r") { return flash ? flash->openFile(path, mode) : sd ? sd->openFile(path, mode) : File(); }
        Error write(const char* path, const AtomicSource& source) { return flash ? flash->writeAtomic(path, source) : sd ? sd->writeAtomic(path, source) : Error::Unavailable; }
    };
    Medium medium(unsigned index) const { return index == 0 ? Medium{_flash, nullptr} : Medium{nullptr, _external ? _sd : nullptr}; }
    void path(const RecordKey&, unsigned medium, char output[128], const char* suffix = "") const;
    void directory(const uint8_t peer[16], unsigned medium, char output[96]) const;
    Error openDirectory(unsigned medium, const char* path, bool required, File&);
    static bool filename(const char*, uint32_t&, bool&);
    Error inspect(const RecordKey&, unsigned medium, const char* suffix, MessageDocument&, StoredRecordHeader&,
                  bool* parsed = nullptr);
    Error commit(const RecordKey&, MessageDocument&, bool creating, Result&);
    void create(const Request&, uint8_t*, Result&);
    void update(const Request&, Result&);
    void markRead(const Request&, Result&);
    void erase(const Request&, Result&);
    void read(const Request&, uint8_t*, size_t, Result&);
    void history(const Request&, uint8_t*, size_t, Result&);
    void pending(const Request&, uint8_t*, size_t, Result&);
    void conversationPage(const Request&, uint8_t*, size_t, Result&);
    void conversation(const Request&, uint8_t*, size_t, Result&);
    void trim(const Request&, Result&);
    bool reserveCounter(uint32_t&);
    bool retainedCopy(const RecordKey&, unsigned medium, MessageDocument&);
    bool initializeCounter();
    uint32_t deletedThrough(const uint8_t peer[16], MessageDocument&, uint8_t* markerState = nullptr,
                            Error* markerError = nullptr);
    FlashStore* _flash = nullptr;
    SDStore* _sd = nullptr;
    bool _external = false, _deferred = false;
    uint8_t _blockedMedia = 0, _preferBackup = 0;
    uint32_t _nextCounter = 1, _reservedThrough = 0;
    uint64_t _mutationEpoch = 1;
    uint32_t _summaryClock = 0;
    uint8_t _summaryMedia = 0;
    Summary _summaries[SummaryCapacity];
};

} // namespace handheld::storage
