#include "ServiceClient.h"
#include "storage/MessageRecord.h"
#include <freertos/FreeRTOS.h>
#if !defined(RSCARDPUTER)
#include <cstring>

namespace handheld {

bool ServiceClient::initialize(const UserConfig& source) {
    UserConfig editable, committed;
    if (!editable.tryAssign(source) || !committed.tryAssign(source)) return false;
    config.swap(editable); _committed.swap(committed);
    _configReady = true;
    _mailbox.readStatus(_status);
    return true;
}

bool ServiceClient::publishSettings(const char* bytes, size_t length) {
    UserConfig editable, committed;
    if (!length || !editable.decode(bytes, length) || !committed.tryAssign(editable)) return false;
    config.swap(editable); _committed.swap(committed);
    _configReady = true; _settingsRefresh = false;
    return true;
}

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
    return node ? node->name : std::string();
}
bool NodeView::saveNode(const std::string& hex) { return _client.action(Operation::SaveContact, hex) != 0; }
bool NodeView::unsaveNode(const std::string& hex) { return deleteContactByHex(hex); }
bool NodeView::deleteContactByHex(const std::string& hex) { return _client.action(Operation::DeleteContact, hex) != 0; }
bool NodeView::setContactName(const std::string& hex, const std::string& name) { return _client.action(Operation::RenameContact, hex, name) != 0; }
bool NodeView::addManualContact(const std::string& hex, const std::string& name) { return _client.action(Operation::SaveContact, hex, name) != 0; }

uint32_t ServiceClient::submit(Request request, const std::string& body, size_t capacity, ValueCompletion callback) {
    return submit(request, body.data(), body.size(), capacity, std::move(callback));
}

uint32_t ServiceClient::submit(Request request, const void* body, size_t length, size_t capacity, ValueCompletion callback) {
    request.generation = _status.generation;
    Callback* pending = nullptr;
    for (auto& item : _callbacks) if (!item.id) { pending = &item; break; }
    if (!pending) { tell("Device busy; try again"); return 0; }
    uint32_t id = 0;
    const auto admitted = _mailbox.submit(request, body, length, capacity, id, millis());
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

uint32_t ServiceClient::requestPeerName(const std::string& peer, TextCompletion completion) {
    uint8_t decoded[16];
    if (!storage::decodeHex(peer.data(), peer.size(), decoded, sizeof(decoded))) return 0;
    Request request; request.operation = Operation::PeerName;
    strlcpy(request.peer, peer.c_str(), sizeof(request.peer));
    return submit(request, "", ServiceMailbox::MaxPayload, std::move(completion));
}

bool ServiceClient::applySettings(Completion completion, bool applyRadio) {
    if (!_configReady) { tell("Settings unavailable; retry"); return false; }
    if (settingsPending()) { tell("Settings save in progress"); return false; }
    Request request; request.operation = Operation::ApplySettings; request.revision = _configRevision;
    request.argument = applyRadio;
    const String encoded = config.encode();
    UserConfig rollback;
    if (encoded.isEmpty() || !rollback.tryAssign(_committed)) {
        tell("Settings memory unavailable; retry"); return false;
    }
    ValueCompletion callback;
    try {
        callback = [this, completion = std::move(completion)](const Result& result, const char* json) {
            _settingsPending = false;
            Result shown = result;
            if ((result.outcome == Outcome::Ok || result.outcome == Outcome::Cancelled) &&
                publishSettings(json, result.length)) {
                _configRevision = result.revision;
                if (result.outcome == Outcome::Ok && onConfigApplied) onConfigApplied();
                if (result.outcome != Outcome::Ok) tell(result.detail);
            } else {
                if (result.settingsCommitted && result.outcome != Outcome::Stale) {
                    shown.outcome = Outcome::NotReady;
                    strlcpy(shown.detail, "Settings saved; refresh pending", sizeof shown.detail);
                    _settingsRefresh = true;
                } else if (result.outcome == Outcome::Stale) {
                    _settingsRefresh = true;
                }
                tell(shown.detail[0] ? shown.detail : "Save failed");
            }
            if (completion) completion(shown);
        };
    } catch (const std::bad_alloc&) {
        tell("Settings memory unavailable; retry"); return false;
    }
    // Prepare the callback and restoration before admission. Neither a local
    // refusal nor later cancellation needs an allocating settings copy.
    config.swap(rollback);
    const auto id = submit(request, encoded.c_str(), encoded.length(), UserConfig::SnapshotLimit,
        std::move(callback));
    _settingsPending = id != 0;
    return id != 0;
}

void ServiceClient::poll() {
    const auto previousGeneration = _status.generation;
    _mailbox.readStatus(_status);
    if (previousGeneration != _status.generation) {
        // Names and saved aliases belong to an identity. Drop the previous
        // publication immediately, but retain any accepted request until its
        // stale callback retires below.
        nodes._nodes.clear(); nodes._revision = 0;
        _nodeStaging.clear(); _nodeOffset = 0; _nodeCursorRevision = 0;
    }
    if (_history.mode() != history::HistoryWindow::Mode::Closed &&
        _history.identityGeneration() != _status.generation) _history.close();
    _history.observeStatusRevision(_status.statusRevision);
    _history.observeHistoryRevision(_status.historyRevision);
    if (_conversationWindow.identityGeneration() != _status.generation &&
        _conversationWindow.state() != history::ConversationList::State::Closed) _conversationWindow.close();
    _conversationWindow.observeRevision(_status.storeRevision);
    _conversationWindow.observeStatusRevision(_status.statusRevision);
    const bool unhealthy = _status.state == ServiceState::Running &&
        millis() - _status.heartbeat > 5000;
    if (unhealthy && !_unhealthy) tell("Device service stalled; wait or reset device");
    _unhealthy = unhealthy;
    for (unsigned count = 0; count < 4; ++count) {
        const auto slot = _mailbox.nextResult();
        if (slot == ServiceMailbox::NoSlot) break;
        const auto request = _mailbox.request(slot);
        auto result = _mailbox.result(slot);
        if (result.length >= sizeof(_scratch) || !_mailbox.read(slot, _scratch, result.length)) {
            result.outcome = Outcome::Failed; result.storageError = storage::Error::Internal;
            result.length = 0;
        }
        _scratch[result.length] = 0;
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
    if (!_settingsPending && !_settingsQuery &&
        (_settingsRefresh || static_cast<int32_t>(_status.configRevision - _configRevision) > 0) &&
        uint32_t(millis() - _lastSettingsQuery) >= 1000) requestSettings();
    if (!_nodesPending && (_nodeOffset || nodes._revision != _status.nodeRevision)) requestNodes();
    if (!_identitiesPending && _identityRevision != _status.identityRevision) requestIdentities();
    requestHistory();
    requestConversationWindow();
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
        else { nodes._nodes.swap(_nodeStaging); _nodeStaging.clear(); _nodeOffset = 0; nodes._revision = result.revision; }
    }) != 0;
}

void ServiceClient::watchHistory(const std::string& peer) {
    uint8_t decoded[16];
    if (!storage::decodeHex(peer.data(), peer.size(), decoded, 16)) { closeHistory(); return; }
    if (_history.mode() != history::HistoryWindow::Mode::Closed &&
        _history.identityGeneration() == _status.generation && !memcmp(_history.peer(), decoded, 16)) return;
    _history.open(decoded, _status.generation);
}
void ServiceClient::closeHistory() {
    // A mode/peer change invalidates presentation, never the callback that owns
    // an accepted mailbox result. Hidden views still copy/release that result.
    _history.close();
}

void ServiceClient::requestHistory() {
    if (_conversationWindow.state() == history::ConversationList::State::Closed &&
        _conversationWindow.ownerTicket().valid()) return;
    const auto query = _history.next(millis());
    using Window = history::HistoryWindow;
    if (query.kind == Window::Kind::None) return;
    Request request;
    request.operation = query.kind == Window::Kind::Page ? Operation::HistoryPage :
                        query.kind == Window::Kind::Status ? Operation::HistoryStatus : Operation::ReadRecord;
    request.query = query.nonce; request.argument = query.cursor.counter; request.incoming = query.cursor.incoming;
    request.offset = query.offset; request.historyDirection = query.direction;
    request.statusRevision = query.statusRevision;
    storage::encodeHex(query.peer, 16, request.peer);
    submitReadQuery(_history, query.nonce, query.kind == Window::Kind::Status, request, nullptr, 0, query.capacity);
}

void ServiceClient::watchConversations() { _conversationWindow.resume(_status.generation); }
void ServiceClient::closeConversations() { _conversationWindow.close(); }

void ServiceClient::requestConversationWindow() {
    // A hidden chat still owns its accepted read until the mailbox releases it.
    // Let that credit retire before asking the shared reader for the list.
    if (_history.state() == history::HistoryWindow::State::Closed && _history.ownerTicket().valid()) return;
    using Window = history::ConversationList;
    const auto query = _conversationWindow.next(millis());
    if (query.kind == Window::Kind::None) return;
    Request request; request.query = query.nonce;
    request.operation = query.kind == Window::Kind::Page ? Operation::ConversationPage :
        query.kind == Window::Kind::Detail ? Operation::ConversationDetail : Operation::HistoryStatus;
    request.statusRevision = query.statusRevision; request.argument = query.selector.counter;
    storage::encodeHex(query.selector.cursor.peer, 16, request.peer);
    ConversationQuery envelope;
    envelope.selector = query.selector; envelope.order = query.order; envelope.direction = query.direction;
    envelope.hasCursor = query.hasCursor;
    const bool status = query.kind == Window::Kind::Status;
    submitReadQuery(_conversationWindow, query.nonce, status, request, status ? nullptr : &envelope,
                    status ? 0 : sizeof(envelope), query.capacity);
}

template<class Window>
void ServiceClient::submitReadQuery(Window& window, uint32_t nonce, bool status, Request request,
                                     const void* body, size_t length, size_t capacity) {
    const auto id = submit(request, body, length, capacity,
        [this, &window, nonce, status](const Result& result, const char* data) {
            storage::Result stored;
            stored.key = result.key; stored.length = result.length; stored.total = result.total;
            stored.nextOffset = result.next; stored.more = result.more;
            stored.revision = status ? result.statusRevision : result.revision;
            stored.outcome = result.outcome == Outcome::Ok ? storage::Outcome::Committed :
                             result.outcome == Outcome::Cancelled ? storage::Outcome::Cancelled : storage::Outcome::Failed;
            stored.error = result.storageError;
            if (result.outcome == Outcome::Stale) stored.error = storage::Error::Stale;
            else if (result.outcome != Outcome::Ok && stored.error == storage::Error::None)
                stored.error = storage::Error::Unavailable;
            // The poll loop copied the payload and retired its mailbox credit.
            // Both windows consume that same stable copy with their own lineage.
            const bool copied = window.result(nonce, stored, data, result.length, millis());
            configASSERT(copied);
            if (!copied) { tell("Message query ownership failed"); return; }
            const bool released = window.released(nonce);
            configASSERT(released);
            if (!released) tell("Message query release failed");
        });
    if (id) {
        const bool admitted = window.admitted(nonce, {id, 0});
        configASSERT(admitted);
        if (!admitted) tell("Message query ownership failed");
    } else window.rejected(nonce, _mailbox.accepting()
        ? storage::Rejection::Busy : storage::Rejection::Unavailable, millis());
}

void ServiceClient::requestSettings() {
    Request request; request.operation = Operation::Settings;
    _lastSettingsQuery = millis();
    _settingsQuery = submit(request, "", UserConfig::SnapshotLimit, [this](const Result& result, const char* data) {
        _settingsQuery = false;
        if (result.outcome == Outcome::Ok && publishSettings(data, result.length)) {
            _configRevision = result.revision;
            if (onConfigApplied) onConfigApplied();
        } else {
            _settingsRefresh = true;
            tell("Settings refresh pending");
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
