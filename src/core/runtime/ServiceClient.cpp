#include "ServiceClient.h"
#if !defined(RSCARDPUTER)
#include <cstring>

namespace handheld {

int NodeView::nodesOnlineSince(unsigned long age) const {
    int count = 0; for (const auto& node : _nodes) if (node.lastSeen && millis() >= node.lastSeen && millis() - node.lastSeen <= age) ++count;
    return count;
}
const DiscoveredNode* NodeView::findNodeByHex(const std::string& hex) const {
    for (const auto& node : _nodes) if (!hex.empty() && node.hash.toHex().compare(0, hex.size(), hex) == 0) return &node;
    return nullptr;
}
std::string NodeView::lookupName(const std::string& hex) const {
    const auto* node = findNodeByHex(hex);
    if (node) return node->name;
    const auto it = _cachedNames.find(hex);
    return it == _cachedNames.end() ? std::string() : it->second;
}
bool NodeView::saveNode(const std::string& hex) { return _client.action(Operation::SaveContact, hex) != 0; }
bool NodeView::unsaveNode(const std::string& hex) { return deleteContactByHex(hex); }
bool NodeView::deleteContactByHex(const std::string& hex) { return _client.action(Operation::DeleteContact, hex) != 0; }
bool NodeView::setContactName(const std::string& hex, const std::string& name) { return _client.action(Operation::RenameContact, hex, name) != 0; }
bool NodeView::addManualContact(const std::string& hex, const std::string& name) { return _client.action(Operation::SaveContact, hex, name) != 0; }

const ConversationSummary* MessageViewModel::getConversationSummary(const std::string& peer) const {
    const auto it = _summaries.find(peer); return it == _summaries.end() ? nullptr : &it->second;
}
std::vector<LXMFMessage> MessageViewModel::getRecentMessages(const std::string& peer, size_t max) {
    _client.watchHistory(peer);
    const auto& history = _client.history();
    if (history.size() <= max) return history;
    return {history.end() - max, history.end()};
}
int MessageViewModel::unreadCount(const std::string& peer) const {
    if (peer.empty()) return _client.status().unread;
    const auto* summary = getConversationSummary(peer); return summary ? summary->unreadCount : 0;
}
int MessageViewModel::queuedCount() const { return _client.status().queued; }
bool MessageViewModel::markRead(const std::string& peer) { return _client.action(Operation::MarkRead, peer) != 0; }
bool MessageViewModel::deleteConversation(const std::string& peer) { return _client.action(Operation::DeleteConversation, peer) != 0; }

uint32_t ServiceClient::submit(Request request, const std::string& body, size_t capacity, ValueCompletion callback) {
    request.generation = _status.generation;
    Callback* pending = nullptr;
    for (auto& item : _callbacks) if (!item.id) { pending = &item; break; }
    if (!pending) { tell("Device busy; try again"); return 0; }
    uint32_t id = 0;
    const auto admitted = _mailbox.submit(request, body.data(), body.size(), capacity, id);
    if (admitted != Admission::Admitted) {
        if (!queryOperation(request.operation)) tell(admitted == Admission::NotReady ? "Device not ready" : "Device busy; try again");
        return 0;
    }
    pending->id = id; pending->callback = std::move(callback);
    return id;
}

uint32_t ServiceClient::action(Operation op, const std::string& peer, const std::string& body,
                              uint32_t argument, Completion completion) {
    if (peer.size() > 32) { tell("Invalid destination"); return 0; }
    Request request; request.operation = op; request.argument = argument;
    strlcpy(request.peer, peer.c_str(), sizeof(request.peer));
    return submit(request, body, 0, [this, completion](const Result& result, const char*) {
        if (result.outcome != Outcome::Ok) tell(result.detail[0] ? result.detail : "Operation failed");
        else if (result.detail[0]) tell(result.detail);
        if (completion) completion(result);
    });
}

bool ServiceClient::applySettings(Completion completion, bool applyRadio) {
    if (_settingsPending || _settingsQuery) { config = _committed; tell("Settings save in progress"); return false; }
    Request request; request.operation = Operation::ApplySettings; request.revision = _configRevision;
    request.argument = applyRadio;
    const String encoded = config.encode();
    const auto id = submit(request, encoded.c_str(), 4096,
        [this, completion](const Result& result, const char* json) {
            _settingsPending = false;
            if (result.length && config.decode(json)) {
                _committed = config; _configRevision = result.revision;
                if (onConfigApplied) onConfigApplied();
                if (result.outcome != Outcome::Ok) tell(result.detail);
            } else {
                config = _committed;
                tell(result.detail[0] ? result.detail : "Save failed");
                if (result.outcome == Outcome::Stale) _configRevision = 0;
            }
            if (completion) completion(result);
        });
    _settingsPending = id != 0;
    if (!id) config = _committed;
    return id != 0;
}

void ServiceClient::poll() {
    _mailbox.readStatus(_status);
    const bool unhealthy = _status.state == ServiceState::Running &&
        millis() - _status.heartbeat > 5000;
    if (unhealthy && !_unhealthy) tell("Device service stalled; wait or reset device");
    _unhealthy = unhealthy;
    for (unsigned count = 0; count < 4; ++count) {
        const auto slot = _mailbox.nextResult();
        if (slot == ServiceMailbox::NoSlot) break;
        const auto request = _mailbox.request(slot);
        auto result = _mailbox.result(slot);
        _mailbox.read(slot, _scratch, result.length); _scratch[result.length] = 0;
        ValueCompletion callback;
        for (auto& item : _callbacks) if (item.id == request.id) {
            callback = std::move(item.callback); item.id = 0; break;
        }
        _mailbox.release(slot);
        if (request.generation != _status.generation) result.outcome = Outcome::Stale;
        if (callback) callback(result, _scratch);
    }
    if (_noticeRevision != _status.noticeRevision) { _noticeRevision = _status.noticeRevision; tell(_status.notice); }
    if (_incomingRevision != _status.incomingRevision) {
        _incomingRevision = _status.incomingRevision; tell("New message");
    }
    if (!_mailbox.accepting() || _unhealthy) return;
    if (!_settingsPending && !_settingsQuery && static_cast<int32_t>(_status.configRevision - _configRevision) > 0) requestSettings();
    if (!_nodesPending && (_nodeOffset || _nodeRevision != _status.nodeRevision)) requestNodes();
    // Revision zero is valid after loading persisted messages at boot. Until
    // the first complete snapshot arrives, an empty UI cache is not up to date.
    if (!_conversationsPending && (!_conversationsLoaded || _convOffset ||
        messages._revision != _status.storeRevision)) requestConversations();
    if (!_identitiesPending && _identityRevision != _status.identityRevision) requestIdentities();
    if (!_historyPeer.empty() && !_historyPending &&
        static_cast<int32_t>(millis() - _historyRetryAt) >= 0 &&
        (_historyLoading || _historyState != HistoryState::Ready ||
         messages._historyRevision != _status.storeRevision)) requestHistory();
}

void ServiceClient::requestNodes() {
    Request request; request.operation = Operation::Nodes; request.offset = _nodeOffset; request.revision = _nodeCursorRevision;
    if (!_nodeOffset) _nodeStaging.clear();
    _nodesPending = submit(request, "", 4096, [this](const Result& result, const char* data) {
        _nodesPending = false;
        JsonDocument doc;
        if (result.outcome != Outcome::Ok || deserializeJson(doc, data)) { _nodeOffset = 0; return; }
        for (JsonObject row : doc.as<JsonArray>()) {
            DiscoveredNode node; node.hash.assignHex(row["hash"] | ""); node.name = row["name"] | "";
            node.identityHex = row["identity"] | ""; node.rssi = row["rssi"]; node.snr = row["snr"];
            node.hops = row["hops"]; node.lastSeen = row["seen"]; node.saved = row["saved"];
            _nodeStaging.push_back(std::move(node));
        }
        _nodeCursorRevision = result.revision;
        if (result.more) _nodeOffset = result.next;
        else { nodes._nodes.swap(_nodeStaging); _nodeStaging.clear(); _nodeOffset = 0; _nodeRevision = result.revision; }
    }) != 0;
}

void ServiceClient::requestConversations() {
    Request request; request.operation = Operation::Conversations; request.offset = _convOffset; request.revision = _convCursorRevision;
    if (!_convOffset) { _convStaging.clear(); _summaryStaging.clear(); nodes._cachedNames.clear(); }
    _conversationsPending = submit(request, "", 4096, [this](const Result& result, const char* data) {
        _conversationsPending = false;
        JsonDocument doc;
        if (result.outcome != Outcome::Ok || deserializeJson(doc, data) || !doc.is<JsonArray>()) {
            _convOffset = 0; return;
        }
        for (JsonObject row : doc.as<JsonArray>()) {
            const std::string peer = row["peer"] | ""; _convStaging.push_back(peer);
            nodes._cachedNames[peer] = row["name"] | "";
            auto& s = _summaryStaging[peer]; s.lastTimestamp = row["time"]; s.lastPreview = row["preview"] | "";
            s.lastIncoming = row["incoming"]; s.unreadCount = row["unread"]; s.totalCount = row["total"];
            s.hasOutgoing = row["outgoing"]; s.hasPending = row["pending"]; s.hasFailed = row["failed"];
            s.lastOutgoingStatus = LXMFStatus(row["status"].as<uint8_t>()); s.lastOutgoingCounter = row["counter"];
            s.pendingCount = row["pendingCount"]; s.failedCount = row["failedCount"];
        }
        _convCursorRevision = result.revision;
        if (result.more) _convOffset = result.next;
        else {
            messages._conversations.swap(_convStaging); messages._summaries.swap(_summaryStaging);
            _convStaging.clear(); _summaryStaging.clear(); _convOffset = 0; messages._revision = result.revision;
            _conversationsLoaded = true;
        }
    }) != 0;
}

void ServiceClient::watchHistory(const std::string& peer) {
    if (_historyPeer == peer) return;
    _historyPeer = peer; _history.clear(); _historyStaging.clear(); _currentMessage = {};
    _historyIndex = _bodyOffset = 0; ++_historyQuery; _historyPending = false;
    _historyLoading = true; _historyRetryAt = millis(); messages._historyRevision = 0;
    _historyState = HistoryState::Loading;
}
void ServiceClient::closeHistory() {
    if (_mailbox.accepting()) action(Operation::CloseHistory);
    _historyPeer.clear(); _history.clear(); _historyStaging.clear(); _currentMessage = {};
    ++_historyQuery; _historyPending = _historyLoading = false;
    _historyState = HistoryState::Closed;
}

void ServiceClient::requestHistory() {
    if (!_historyLoading) {
        _historyLoading = true; _historyIndex = _bodyOffset = 0; _historyStaging.clear(); ++_historyQuery;
    }
    Request request; request.operation = !_historyIndex && !_bodyOffset ? Operation::History : Operation::MessageBody;
    request.offset = _historyIndex; request.argument = _bodyOffset; request.query = _historyQuery;
    request.revision = _historyStoreRevision; strlcpy(request.peer, _historyPeer.c_str(), sizeof(request.peer));
    const auto query = _historyQuery;
    _historyPending = submit(request, "", 4096, [this, query](const Result& result, const char* data) {
        if (query != _historyQuery) return;
        _historyPending = false;
        JsonDocument doc;
        if (result.outcome != Outcome::Ok || deserializeJson(doc, data)) {
            _historyLoading = false; _historyIndex = _bodyOffset = 0; _historyStaging.clear();
            _historyRetryAt = millis() + 1000;
            if (result.outcome != Outcome::Stale) {
                // Initial failures are shown inline. Keep an existing snapshot
                // visible on refresh failure, with at most one notice per retry episode.
                if (_historyState != HistoryState::Retrying && !_history.empty())
                    tell("Couldn't refresh messages; retrying");
                _historyState = HistoryState::Retrying;
            }
            return;
        }
        _historyStoreRevision = result.revision;
        if (result.total) {
            if (!_bodyOffset) {
                _currentMessage = {};
                _currentMessage.sourceHash.assignHex(doc["source"] | ""); _currentMessage.destHash.assignHex(doc["dest"] | "");
                _currentMessage.timestamp = doc["time"]; _currentMessage.incoming = doc["incoming"];
                _currentMessage.status = LXMFStatus(doc["status"].as<uint8_t>()); _currentMessage.read = doc["read"];
                _currentMessage.messageId.assignHex(doc["msgid"] | "");
                _currentMessage.savedCounter = doc["counter"]; _currentMessage.receiveCounter = doc["receive"];
            }
            rs::Bytes body; body.assignHex(doc["body"] | "");
            _currentMessage.content.append(reinterpret_cast<const char*>(body.data()), body.size());
            if (doc["bodyEnd"].as<bool>()) {
                _historyStaging.push_back(std::move(_currentMessage)); ++_historyIndex; _bodyOffset = 0;
            } else _bodyOffset = result.next;
        }
        if (!result.more) {
            _history.swap(_historyStaging); _historyStaging.clear(); _historyLoading = false;
            _historyIndex = _bodyOffset = 0; messages._historyRevision = result.revision;
            _historyState = HistoryState::Ready;
        }
    }) != 0;
}

void ServiceClient::requestSettings() {
    Request request; request.operation = Operation::Settings;
    _settingsQuery = submit(request, "", 4096, [this](const Result& result, const char* data) {
        _settingsQuery = false;
        if (result.outcome == Outcome::Ok && config.decode(data)) {
            _committed = config; _configRevision = result.revision;
            if (onConfigApplied) onConfigApplied();
        }
    }) != 0;
}
void ServiceClient::requestIdentities() {
    Request request; request.operation = Operation::Identities;
    _identitiesPending = submit(request, "", 4096, [this](const Result& result, const char* data) {
        _identitiesPending = false;
        JsonDocument doc;
        if (result.outcome != Outcome::Ok || deserializeJson(doc, data)) return;
        _identities.clear();
        for (JsonObject row : doc.as<JsonArray>()) {
            IdentitySlot identity; identity.hash = row["hash"] | ""; identity.displayName = row["name"] | "";
            identity.active = row["active"]; _identities.push_back(std::move(identity));
        }
        _identityRevision = result.revision;
    }) != 0;
}
int ServiceClient::activeIdentity() const {
    for (size_t i = 0; i < _identities.size(); ++i) if (_identities[i].active) return i;
    return -1;
}
bool ServiceClient::scan(Completion completion) {
    Request request; request.operation = Operation::Scan;
    return submit(request, "", 4096, [this, completion](const Result& result, const char* data) {
        _scanJson = result.outcome == Outcome::Ok ? data : "[]";
        if (completion) completion(result);
    }) != 0;
}
uint32_t ServiceClient::lifecycle(Operation operation, const std::string& identity) {
    const auto id = action(operation, identity, "", 0, [this, operation](const Result& result) {
        completedLifecycle = operation; lifecycleComplete = result.outcome == Outcome::Ok;
        lifecycleFailed = !lifecycleComplete;
    });
    if (id) { lifecycleStarted = millis(); lifecycleComplete = lifecycleFailed = false; tell("Finishing device operations..."); }
    return id;
}

} // namespace handheld
#endif
