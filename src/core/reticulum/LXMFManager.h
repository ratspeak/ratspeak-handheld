#pragma once

#include <functional>
#include <string>
#include <vector>

#include "LXMFMessage.h"
#include "storage/MessageStore.h"

// Store-backed LXMF read surface. Wire
// protocol lives in the Rust FFI; send, queueing, links, and delivery proofs
// live in the backend engines behind the ProtocolBackend facade.
class LXMFManager {
public:
    using MessageCallback = std::function<void(const LXMFMessage&)>;
    using StatusCallback = std::function<void(const std::string& peerHex, double timestamp,
                                              uint32_t savedCounter, LXMFStatus status)>;

    bool beginStoreOnly(MessageStore* store);

    // Engine-owned queue state stays behind the ProtocolBackend facade.
    int queuedCount() const { return 0; }

    uint32_t storeRevision() const { return _store ? _store->revision() : 0; }
    const std::vector<std::string>& conversations() const;
    std::vector<LXMFMessage> getMessages(const std::string& peerHex) const;
    std::vector<LXMFMessage> getRecentMessages(const std::string& peerHex, size_t maxMessages) const;
    int unreadCount(const std::string& peerHex = "") const;
    void markRead(const std::string& peerHex);
    bool deleteConversation(const std::string& peerHex);
    const ConversationSummary* getConversationSummary(const std::string& peerHex) const;

private:
    MessageStore* _store = nullptr;
};
