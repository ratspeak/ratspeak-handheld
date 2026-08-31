#pragma once

#if !defined(RSCARDPUTER)
#include "ServiceMailbox.h"
#include "config/UserConfig.h"
#include "reticulum/AnnounceManager.h"
#include "reticulum/IdentityManager.h"
#include "reticulum/LXMFManager.h"
#include <array>
#include <map>

namespace handheld {
class ServiceClient;
enum class HistoryState : uint8_t { Closed, Loading, Ready, Retrying };

class NodeView {
public:
    explicit NodeView(ServiceClient& client) : _client(client) {}
    const std::vector<DiscoveredNode>& nodes() const { return _nodes; }
    int nodeCount() const { return _nodes.size(); }
    int nodesOnlineSince(unsigned long age) const;
    const DiscoveredNode* findNodeByHex(const std::string& hex) const;
    std::string lookupName(const std::string& hex) const;
    bool saveNode(const std::string& hex);
    bool unsaveNode(const std::string& hex);
    bool deleteContactByHex(const std::string& hex);
    bool setContactName(const std::string& hex, const std::string& name);
    bool addManualContact(const std::string& hex, const std::string& name);
private:
    friend class ServiceClient;
    ServiceClient& _client;
    std::vector<DiscoveredNode> _nodes;
    std::map<std::string, std::string> _cachedNames;
};

class MessageViewModel {
public:
    explicit MessageViewModel(ServiceClient& client) : _client(client) {}
    const std::vector<std::string>& conversations() const { return _conversations; }
    const ConversationSummary* getConversationSummary(const std::string& peer) const;
    std::vector<LXMFMessage> getRecentMessages(const std::string& peer, size_t max);
    int unreadCount(const std::string& peer = "") const;
    int queuedCount() const;
    uint32_t storeRevision() const { return _revision; }
    uint32_t historyRevision() const { return _historyRevision; }
    bool markRead(const std::string& peer);
    bool deleteConversation(const std::string& peer);
private:
    friend class ServiceClient;
    ServiceClient& _client;
    std::vector<std::string> _conversations;
    std::map<std::string, ConversationSummary> _summaries;
    uint32_t _revision = 0;
    uint32_t _historyRevision = 0;
};

// Read-only UI view of the protocol. There is deliberately no loop, storage,
// callback-registration, or synchronous send method on this interface.
class ProtocolView {
public:
    explicit ProtocolView(const Status& status) : _status(status) {}
    bool protocolReady() const { return _status.ready; }
    bool isTransportActive() const { return _status.transport; }
    size_t pathCount() const { return _status.paths; }
    size_t linkCount() const { return _status.links; }
    unsigned long lastAnnounceTime() const { return _status.lastAnnounce; }
    int lxmfQueuedCount() const { return _status.queued; }
    String identityHash() const { return String(_status.identity); }
    String identityHashHex() const { return String(_status.identityHex); }
    String destinationHashHex() const { return String(_status.destination); }
    String destinationHashStr() const { return String(_status.destination); }
    String publicKeyHex() const { return String(_status.publicKey); }
private:
    const Status& _status;
};

class ServiceClient {
    Status _status;
public:
    using Completion = std::function<void(const Result&)>;
    explicit ServiceClient(ServiceMailbox& mailbox)
        : nodes(*this), messages(*this), protocol(_status), _mailbox(mailbox) {}
    void initialize(const UserConfig& source) { config = source; _committed = source; _mailbox.readStatus(_status); }
    void poll();
    const Status& status() const { return _status; }
    uint32_t action(Operation op, const std::string& peer = "", const std::string& body = "",
                    uint32_t argument = 0, Completion completion = {});
    bool applySettings(Completion completion = {}, bool applyRadio = true);
    bool available() const { return _mailbox.accepting(); }
    bool settingsPending() const { return _settingsPending; }
    void watchHistory(const std::string& peer);
    void closeHistory();
    bool historyLoading() const { return _historyPending || _historyLoading; }
    HistoryState historyState() const { return _historyState; }
    const std::vector<LXMFMessage>& history() const { return _history; }
    const std::string& historyPeer() const { return _historyPeer; }
    const std::vector<IdentitySlot>& identities() const { return _identities; }
    int activeIdentity() const;
    uint32_t identityRevision() const { return _identityRevision; }
    bool scan(Completion completion = {});
    const String& scanJson() const { return _scanJson; }
    uint32_t lifecycle(Operation operation, const std::string& identity = "");
    Operation completedLifecycle = Operation::Nodes;
    bool lifecycleComplete = false;
    bool lifecycleFailed = false;
    uint32_t lifecycleStarted = 0;
    std::function<void(const char*)> onNotice;
    std::function<void()> onConfigApplied;
    NodeView nodes;
    MessageViewModel messages;
    ProtocolView protocol;
    UserConfig config;

private:
    using ValueCompletion = std::function<void(const Result&, const char*)>;
    uint32_t submit(Request request, const std::string& body, size_t capacity, ValueCompletion callback);
    void requestNodes();
    void requestConversations();
    void requestHistory();
    void requestIdentities();
    void requestSettings();
    void tell(const char* text) { if (onNotice && text && *text) onNotice(text); }
    ServiceMailbox& _mailbox;
    struct Callback { uint32_t id = 0; ValueCompletion callback; };
    std::array<Callback, ServiceMailbox::SlotCount> _callbacks;
    char _scratch[ServiceMailbox::MaxPayload + 1] = {};
    UserConfig _committed;
    bool _settingsPending = false, _settingsQuery = false;
    uint32_t _configRevision = 1;
    uint32_t _noticeRevision = 0, _incomingRevision = 0;
    bool _unhealthy = false;
    uint32_t _nodeRevision = 0, _nodeCursorRevision = 0, _nodeOffset = 0;
    bool _nodesPending = false;
    std::vector<DiscoveredNode> _nodeStaging;
    uint32_t _convOffset = 0, _convCursorRevision = 0;
    bool _conversationsPending = false, _conversationsLoaded = false;
    std::vector<std::string> _convStaging;
    std::map<std::string, ConversationSummary> _summaryStaging;
    uint32_t _identityRevision = 0;
    bool _identitiesPending = false;
    std::vector<IdentitySlot> _identities;
    std::string _historyPeer;
    std::vector<LXMFMessage> _history, _historyStaging;
    uint32_t _historyRetryAt = 0;
    uint32_t _historyQuery = 1, _historyIndex = 0, _bodyOffset = 0, _historyStoreRevision = 0;
    bool _historyPending = false, _historyLoading = false;
    HistoryState _historyState = HistoryState::Closed;
    LXMFMessage _currentMessage;
    String _scanJson;
};

} // namespace handheld
#endif
