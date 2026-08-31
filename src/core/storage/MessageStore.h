#pragma once

#include <Arduino.h>
#include "config/Config.h"
#include "storage/FlashStore.h"
#include "storage/SDStore.h"
#include "reticulum/LXMFMessage.h"
#include <vector>
#include <string>
#include <map>
#include <set>
#include <utility>
#include <array>
#if STORAGE_ASYNC_WRITES
#include "storage/WriteQueue.h"
#include <atomic>
#endif

struct ConversationSummary {
    double lastTimestamp = 0;
    std::string lastPreview;    // first ~20 chars of last message
    bool lastIncoming = false;
    int unreadCount = 0;
    int totalCount = 0;
    bool hasOutgoing = false;
    bool hasPending = false;
    bool hasFailed = false;
    LXMFStatus lastOutgoingStatus = LXMFStatus::DRAFT;
    uint32_t lastOutgoingCounter = 0;
    uint16_t pendingCount = 0;
    uint16_t failedCount = 0;
};

class MessageStore {
public:
    bool begin(FlashStore* flash, SDStore* sd = nullptr, bool externalStorageEnabled = false);
    void setExternalStorageEnabled(bool enabled) { _externalStorageEnabled = enabled; }
    bool externalStorageEnabled() const { return _externalStorageEnabled; }

    // Single-owner incremental history reader. Directory discovery retains only
    // the newest 40 filenames; bodies are loaded one at a time, not duplicated
    // as another whole conversation in the service.
    struct HistoryCursor {
        File directory;
        String path;
        std::string peer;
        uint32_t deletedThrough = 0;
        std::array<String, 40> filenames;
        size_t count = 0;
        uint32_t revision = 0;
        bool sd = false, ready = false;
    };
    enum class HistoryStep { Pending, Ready, Stale };
    void beginHistory(HistoryCursor& cursor, const std::string& peer) const;
    HistoryStep stepHistory(HistoryCursor& cursor) const;
    bool readHistory(const HistoryCursor& cursor, size_t index, LXMFMessage& message) const;
    bool saveMessage(LXMFMessage& msg);
    std::vector<LXMFMessage> loadConversation(const std::string& peerHex) const;
    std::vector<LXMFMessage> loadConversationTail(const std::string& peerHex, size_t maxMessages) const;
    const std::vector<std::string>& conversations() const { return _conversations; }
    void refreshConversations();
    int messageCount(const std::string& peerHex) const;
    bool deleteConversation(const std::string& peerHex);
    std::vector<LXMFMessage> loadPendingOutgoing() const;
    std::vector<std::string> loadRecentMessageIds(size_t maxIds) const;
    const std::vector<LXMFMessage>& startupPendingOutgoing() const { return _startupPendingOutgoing; }
    std::vector<std::string> startupRecentMessageIds(size_t maxIds) const;
    // Free the boot-seed vectors once the backend has consumed them (heap: they
    // otherwise sit in RAM for the process lifetime — matters on no-PSRAM boards).
    void releaseStartupSeeds() {
        _startupPendingOutgoing.clear();
        _startupPendingOutgoing.shrink_to_fit();
        _startupRecentMessageIds.clear();
        _startupRecentMessageIds.shrink_to_fit();
    }
    bool markConversationRead(const std::string& peerHex);
    bool updateMessageStatus(const std::string& peerHex, double timestamp, bool incoming, LXMFStatus newStatus);
    bool updateMessageStatusByCounter(const std::string& peerHex, uint32_t counter, bool incoming, LXMFStatus newStatus);

    const ConversationSummary* getSummary(const std::string& peerHex) const;
    int totalUnreadCount() const;
    uint32_t revision() const { return _revision; }

#if STORAGE_ASYNC_WRITES
    // Diagnostics accessor (HEART writeQ token)
    WriteQueue& writeQueue() { return _writeQueue; }
#endif

private:
    String conversationDir(const std::string& peerHex) const;
    String sdConversationDir(const std::string& peerHex) const;
    std::vector<String> messageFiles(const std::string& peer) const;
    uint32_t deletedThrough(const std::string& peer) const;
    String readMessageJson(const std::string& peer, const String& filename) const;
    bool writeMessageJson(const std::string& peer, const String& filename, const String& json);
    void enforceFlashLimit(const std::string& peerHex);
    void enforceSDLimit(const std::string& peerHex);
    void migrateFlashToSD();
    void migrateTruncatedDirs();
    void initReceiveCounter();
    void ensureConvDirs(const std::string& peerHex);
    bool sdWriteJson(const char* path, const String& data);
    bool flashWriteJson(const char* path, const String& data);
#if LEGACY_MSG_FILENAME_MIGRATION
    bool migrateOldFilenames();
#endif
#if LEGACY_READ_CTR_MIGRATION
    void migrateReadMarkers();
#endif
    void buildSummaries();
    void rebuildSummary(const std::string& peerHex);
    ConversationSummary buildSummaryForPeer(
        const std::string& peerHex,
        std::vector<LXMFMessage>* pendingOut = nullptr,
        std::vector<std::pair<uint32_t, std::string>>* recentIds = nullptr) const;
    void updateSummaryStatus(const std::string& peerHex, uint32_t counter, LXMFStatus oldStatus, LXMFStatus newStatus);
    void bumpRevision();

    FlashStore* _flash = nullptr;
    SDStore* _sd = nullptr;
    bool _externalStorageEnabled = false;
    std::vector<std::string> _conversations;
    std::map<std::string, ConversationSummary> _summaries;
    std::vector<LXMFMessage> _startupPendingOutgoing;
    std::vector<std::string> _startupRecentMessageIds;
    // Boot-seed collection bounds (consumers take <=100 ids / <=queue-cap pending)
    static constexpr size_t STARTUP_RECENT_CAP = 128;
    static constexpr size_t STARTUP_PENDING_CAP = 32;
    std::set<std::string> _ensuredDirs;  // Directory-creation cache
#if STORAGE_ASYNC_WRITES
    WriteQueue _writeQueue;
    std::atomic<uint32_t> _nextReceiveCounter{0};
    // Crash-safe counter reservation: NVS always holds a ceiling >= any counter a
    // file write has ever used (persisted ahead in blocks from the main task —
    // the 30s WriteQueue batch left a reuse window that overwrote messages).
    uint32_t _persistedCounterCeiling = 0;
    static constexpr uint32_t COUNTER_RESERVE = 32;
#else
    uint32_t _nextReceiveCounter = 0;
#endif
    uint32_t _revision = 0;
};
