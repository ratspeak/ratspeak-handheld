#pragma once

#if !defined(RSCARDPUTER)
#include "ServiceMailbox.h"
#include "history/HistoryWindow.h"
#include "history/ConversationWindow.h"
#include "config/UserConfig.h"
#include "reticulum/AnnounceManager.h"
#include "reticulum/IdentityManager.h"
#include "reticulum/LXMFMessage.h"
#include <array>
#include <functional>

namespace handheld {
class ServiceClient;

class NodeView {
public:
    explicit NodeView(ServiceClient& client) : _client(client) {}
    const std::vector<DiscoveredNode>& nodes() const { return _nodes; }
    // Revision of the fully copied list, which may lag the backend Status.
    uint32_t revision() const { return _revision; }
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
    uint32_t _revision = 0;
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
    // Text is a copied mailbox value valid only during the callback. Renderers
    // copy names into their widget owner; no conversation-name map is retained.
    using TextCompletion = std::function<void(const Result&, const char*)>;
    explicit ServiceClient(ServiceMailbox& mailbox)
        : nodes(*this), protocol(_status), _mailbox(mailbox) {}
    bool initialize(const UserConfig& source);
    void poll();
    const Status& status() const { return _status; }
    uint32_t action(Operation op, const std::string& peer = "", const std::string& body = "",
                    uint32_t argument = 0, Completion completion = {});
    uint32_t requestPeerName(const std::string& peer, TextCompletion completion);
    // Pending Send only. Completion is still consumed; navigation does not cancel.
    bool cancelSend(uint32_t requestId) { return _mailbox.cancelSend(requestId); }
    bool applySettings(Completion completion = {}, bool applyRadio = true);
    bool available() const { return _mailbox.accepting(); }
    bool settingsPending() const { return _settingsPending || _settingsQuery || _settingsRefresh; }
    void watchHistory(const std::string& peer);
    void closeHistory();
    void watchConversations();
    void closeConversations();
    history::ConversationList& conversationWindow() { return _conversationWindow; }
    const history::ConversationList& conversationWindow() const { return _conversationWindow; }
    history::HistoryWindow& historyWindow() { return _history; }
    const history::HistoryWindow& historyWindow() const { return _history; }
    bool historyStatusReady() const { return _history.statusReady(); }
    uint32_t historyStatusRevision() const { return _history.statusRevision(); }
    bool historyFreshnessAvailable() const { return _history.freshnessAvailable(); }
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
    ProtocolView protocol;
    UserConfig config;

private:
    using ValueCompletion = std::function<void(const Result&, const char*)>;
    uint32_t submit(Request request, const std::string& body, size_t capacity, ValueCompletion callback);
    uint32_t submit(Request request, const void* body, size_t length, size_t capacity, ValueCompletion callback);
    template<class Window> void submitReadQuery(Window&, uint32_t nonce, bool status,
        Request, const void* body, size_t length, size_t capacity);
    void requestNodes();
    void requestHistory();
    void requestConversationWindow();
    void requestIdentities();
    void requestSettings();
    bool publishSettings(const char* bytes, size_t length);
    void tell(const char* text) { if (onNotice && text && *text) onNotice(text); }
    ServiceMailbox& _mailbox;
    struct Callback { uint32_t id = 0; ValueCompletion callback; };
    std::array<Callback, ServiceMailbox::SlotCount> _callbacks;
    char _scratch[ServiceMailbox::MaxPayload + 1] = {};
    UserConfig _committed;
    bool _settingsPending = false, _settingsQuery = false;
    bool _configReady = false, _settingsRefresh = false;
    uint32_t _lastSettingsQuery = 0;
    uint32_t _configRevision = 1;
    uint32_t _noticeRevision = 0, _incomingRevision = 0;
    bool _unhealthy = false;
    uint32_t _nodeCursorRevision = 0, _nodeOffset = 0;
    bool _nodesPending = false;
    std::vector<DiscoveredNode> _nodeStaging;
    uint32_t _identityRevision = 0;
    bool _identitiesPending = false;
    std::vector<IdentitySlot> _identities;
    history::HistoryWindow _history;
    history::ConversationList _conversationWindow;
    String _scanJson;
};

} // namespace handheld
#endif
