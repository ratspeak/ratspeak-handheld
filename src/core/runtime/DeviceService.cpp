#include "DeviceService.h"
#include "TaskOwner.h"
#if !defined(RSCARDPUTER)
#include "util/Bytes.h"
#include "config/Timezones.h"
#include <esp_heap_caps.h>
#include <nvs_flash.h>
#include <cassert>
#include <cstring>

namespace handheld {

DeviceService::DeviceService(ProtocolBackend& backend, MessageStore& messages,
        UserConfig& config, IdentityManager& identities, FlashStore& flash, SDStore& sd)
    : _backend(backend), _messages(messages), _config(config), _identities(identities),
      _flash(flash), _sd(sd), _pump(_mailbox, *this) {}

DeviceService::~DeviceService() { heap_caps_free(_arena); }

bool DeviceService::begin(AnnounceManager* nodes) {
    _nodes = nodes;
    _arena = static_cast<uint8_t*>(heap_caps_calloc(1, ServiceMailbox::ArenaSize,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!_arena) _arena = static_cast<uint8_t*>(calloc(1, ServiceMailbox::ArenaSize));
    if (!_arena) return false;
    Serial.printf("[SERVICE] bridge_control=%u payload=%u scratch=%u runner_stack=%u\n",
        unsigned(sizeof(ServiceMailbox)), unsigned(ServiceMailbox::ArenaSize),
        unsigned(sizeof(_scratch)), 24u * 1024u);
    _mailbox.begin(_arena, ServiceMailbox::ArenaSize);
    _mailbox.setAccepting(false);
    _status.state = ServiceState::Starting;
    _backend.setMessageCallback([this](const LXMFMessage&) { ++_status.incomingRevision; });
    // Storage revision is authoritative, including coalesced delivery updates.
    _backend.setStatusCallback(nullptr);
    refreshStatus();
    return true;
}

void DeviceService::bindOwner() {
    _owner = xTaskGetCurrentTaskHandle();
    bindDeviceOwner();
    _status.state = ServiceState::Running;
    _mailbox.setAccepting(true);
}

void DeviceService::tick() {
    configASSERT(_owner == xTaskGetCurrentTaskHandle());
    const auto start = millis();
    _pump.tick(_status.generation);
    const auto duration = millis() - start;
    _status.serviceMaxMs = std::max(_status.serviceMaxMs, static_cast<uint32_t>(duration));
    if (millis() - _lastStatus >= 250) refreshStatus();
}

void DeviceService::notice(const char* text) {
    strlcpy(_status.notice, text ? text : "", sizeof(_status.notice));
    ++_status.noticeRevision;
}

void DeviceService::poll() {
    if (_status.state == ServiceState::Running && pollNetwork) pollNetwork();
    if (_status.state == ServiceState::Running && millis() - _lastIdentityRetry >= 30000) {
        _lastIdentityRetry = millis();
        if (!_identities.flushPending()) notice("Identity metadata save failed; retry pending");
        if (!_config.flushPending(_sd, _flash)) notice("Settings saved on device; SD backup retry pending");
    }
    if (_historySlot != ServiceMailbox::NoSlot) {
        const auto step = _messages.stepHistory(_history);
        if (step != MessageStore::HistoryStep::Pending) {
            const auto slot = _historySlot; _historySlot = ServiceMailbox::NoSlot;
            if (step == MessageStore::HistoryStep::Stale) complete(slot, Outcome::Stale);
            else historyChunk(slot);
        }
    }
    if (_historyQuery && millis() - _lastHistoryAccess > 5000) closeHistory();
    if (_scanSlot != ServiceMailbox::NoSlot && finishScan) {
        String json;
        if (finishScan(json)) {
            Result result;
            result.outcome = json.isEmpty() ? Outcome::Failed : Outcome::Ok;
            if (json.length() <= _mailbox.capacity(_scanSlot) &&
                _mailbox.write(_scanSlot, json.c_str(), json.length())) result.length = json.length();
            else result.outcome = Outcome::Failed;
            _mailbox.complete(_scanSlot, result);
            _scanSlot = ServiceMailbox::NoSlot;
            _status.scanRunning = false;
        }
    }
    if (_lifecycleSlot != ServiceMailbox::NoSlot) {
        if (!_quiesceStarted) {
            _quiesceStarted = true;
            if (beginQuiesce) beginQuiesce();
        }
        if (!quiescent || quiescent()) {
            const auto slot = _lifecycleSlot;
            _lifecycleSlot = ServiceMailbox::NoSlot;
            runLifecycle(slot);
        }
    }
}

void DeviceService::refreshStatus() {
    _lastStatus = millis();
    _status.heartbeat = _lastStatus;
    _status.ready = _backend.protocolReady();
    _status.transport = _backend.isTransportActive();
    _status.paths = _backend.pathCount();
    _status.links = _backend.linkCount();
    _status.queued = _backend.lxmfQueuedCount();
    _status.resources = _backend.activeResourceTransfers();
    _status.lastAnnounce = _backend.lastAnnounceTime();
    _status.unread = _messages.totalUnreadCount();
    _status.storeRevision = _messages.revision();
    _status.flash = _flash.isReady();
    _status.sd = _sd.isReady();
    if (!_lastStorageStatus || _lastStatus - _lastStorageStatus >= 5000) {
        _lastStorageStatus = _lastStatus;
        _status.flashUsed = _flash.usedBytes();
        _status.flashTotal = _flash.totalBytes();
    }
    if (_owner) _status.stackFree = uxTaskGetStackHighWaterMark(_owner);
    strlcpy(_status.identity, _backend.identityHash().c_str(), sizeof(_status.identity));
    strlcpy(_status.identityHex, _backend.identityHashHex().c_str(), sizeof(_status.identityHex));
    strlcpy(_status.destination, _backend.destinationHashHex().c_str(), sizeof(_status.destination));
    strlcpy(_status.publicKey, _backend.publicKeyHex().c_str(), sizeof(_status.publicKey));
    uint32_t fingerprint = 2166136261u;
    if (_nodes) for (const auto& node : _nodes->nodes()) {
        for (size_t i = 0; i < node.hash.size(); ++i) fingerprint = (fingerprint ^ node.hash.data()[i]) * 16777619u;
        for (auto ch : node.name) fingerprint = (fingerprint ^ uint8_t(ch)) * 16777619u;
        fingerprint = (fingerprint ^ node.lastSeen ^ uint32_t(node.rssi) ^ node.saved) * 16777619u;
    }
    if (fingerprint != _nodeFingerprint) { _nodeFingerprint = fingerprint; ++_status.nodeRevision; }
    if (networkStatus) networkStatus(_status);
    _mailbox.publishStatus(_status);
}

void DeviceService::complete(uint8_t slot, Outcome outcome, const char* detail) {
    Result result; result.outcome = outcome;
    if (detail) strlcpy(result.detail, detail, sizeof(result.detail));
    _mailbox.complete(slot, result);
}

void DeviceService::jsonResult(uint8_t slot, JsonDocument& doc, Result result) {
    const auto length = measureJson(doc);
    if (doc.overflowed() || length > _mailbox.capacity(slot) || length > ServiceMailbox::MaxPayload) {
        complete(slot, Outcome::Failed, "Response exceeds transfer budget"); return;
    }
    result.length = serializeJson(doc, _scratch, sizeof(_scratch));
    result.outcome = Outcome::Ok;
    _mailbox.write(slot, _scratch, result.length);
    _mailbox.complete(slot, result);
}

void DeviceService::execute(uint8_t slot) {
    configASSERT(_owner == xTaskGetCurrentTaskHandle());
    const auto request = _mailbox.request(slot);
    if (lifecycleOperation(request.operation)) {
        if (_status.state == ServiceState::Failed && request.operation == Operation::Restart) {
            // Explicit second restart after the visible failure warning. Never
            // restart automatically on a failed flush, or resume a second owner.
            complete(slot, Outcome::Ok, "Forced restart after storage failure");
            return;
        }
        _status.state = ServiceState::Quiescing;
        _lifecycleSlot = slot;
        _quiesceStarted = false;
        return;
    }
    const auto length = _mailbox.length(slot);
    if (!_mailbox.read(slot, _scratch, length)) { complete(slot, Outcome::Invalid); return; }
    _scratch[length] = 0;
    switch (request.operation) {
    case Operation::Send: {
        rs::Bytes destination; destination.assignHex(request.peer);
        if (destination.size() != 16 || !length || std::strlen(_scratch) != length) {
            complete(slot, Outcome::Invalid, "Invalid message"); break;
        }
        const bool accepted = _backend.lxmfSendMessage(destination.data(), _scratch, "", request.argument != 0);
        complete(slot, accepted ? Outcome::Ok : Outcome::Failed,
                 accepted ? "Message queued" : "Message not saved");
        break;
    }
    case Operation::MarkRead: {
        const bool saved = _messages.markConversationRead(request.peer);
        complete(slot, saved ? Outcome::Ok : Outcome::Failed,
                 saved ? "" : "Couldn't mark messages as read");
        break;
    }
    case Operation::DeleteConversation:
        _backend.lxmfDropPeer(request.peer);
        complete(slot, _messages.deleteConversation(request.peer) ? Outcome::Ok : Outcome::Failed,
                 ""); break;
    case Operation::SaveContact: {
        if (!_nodes) { complete(slot, Outcome::NotReady); break; }
        const bool saved = _nodes->findNodeByHex(request.peer)
            ? _nodes->saveNode(request.peer) : _nodes->addManualContact(request.peer, _scratch);
        complete(slot, saved ? Outcome::Ok : Outcome::Failed,
                 saved ? "Contact saved" : "Contact persistence incomplete; retry pending"); break;
    }
    case Operation::DeleteContact:
        complete(slot, _nodes && _nodes->deleteContactByHex(request.peer) ? Outcome::Ok : Outcome::Failed,
                 ""); break;
    case Operation::RenameContact:
        complete(slot, _nodes && _nodes->setContactName(request.peer, _scratch) ? Outcome::Ok : Outcome::Failed,
                 ""); break;
    case Operation::Announce: {
        const auto& name = _config.settings().displayName;
        const size_t size = std::min(size_t(name.length()), size_t(31));
        uint8_t bytes[36] = {0x93, 0xc4, uint8_t(size)};
        std::memcpy(bytes + 3, name.c_str(), size); bytes[size + 3] = 0xc0; bytes[size + 4] = 0x90;
        const auto sent = _backend.announce(bytes, size + 5);
        complete(slot, sent == ProtocolBackend::AnnounceResult::Failed ? Outcome::Failed : Outcome::Ok,
                 sent == ProtocolBackend::AnnounceResult::Sent ? "Announce sent" :
                 sent == ProtocolBackend::AnnounceResult::Deferred ? "Announce queued" : "Announce not sent");
        break;
    }
    case Operation::ApplySettings: {
        if (request.revision != _status.configRevision) { complete(slot, Outcome::Stale, "Settings changed; try again"); break; }
        UserConfig candidate = _config;
        if (!candidate.decode(String(_scratch))) { complete(slot, Outcome::Invalid, "Invalid settings"); break; }
        const String encoded = candidate.encode();
        if (encoded.isEmpty() || encoded.length() > _mailbox.capacity(slot)) {
            complete(slot, Outcome::Invalid, "Settings exceed transfer budget"); break;
        }
        if (!candidate.save(_sd, _flash)) { complete(slot, Outcome::Failed, "Save failed"); break; }
        _config = candidate;
        ++_status.configRevision;
        bool identitySaved = true;
        if (_identities.activeIndex() >= 0 &&
            _identities.getDisplayName(_identities.activeIndex()) != _config.settings().displayName) {
            identitySaved = _identities.setDisplayName(_identities.activeIndex(), _config.settings().displayName);
            ++_status.identityRevision;
        }
        if (_config.settings().timezoneIdx < TIMEZONE_COUNT) {
            setenv("TZ", TIMEZONE_TABLE[_config.settings().timezoneIdx].posixTZ, 1);
            tzset();
        }
        identitySaved &= _identities.flushPending();
        if (request.argument && applyRadio) applyRadio(_config.settings());
        if (applyPeripherals) applyPeripherals(_config.settings());
        Result result; result.outcome = Outcome::Ok; result.revision = _status.configRevision;
        if (encoded.length() <= _mailbox.capacity(slot)) {
            _mailbox.write(slot, encoded.c_str(), encoded.length()); result.length = encoded.length();
        }
        if (!identitySaved) result.outcome = Outcome::Failed;
        strlcpy(result.detail, !identitySaved ? "Settings saved; identity name retry pending" :
            _config.mirrorPending() ? "Settings saved on device; SD backup pending" : "Settings saved", sizeof(result.detail));
        _mailbox.complete(slot, result);
        break;
    }
    case Operation::Settings: {
        const String json = _config.encode();
        if (json.length() > _mailbox.capacity(slot)) { complete(slot, Outcome::Failed); break; }
        _mailbox.write(slot, json.c_str(), json.length());
        Result result; result.outcome = Outcome::Ok; result.length = json.length(); result.revision = _status.configRevision;
        _mailbox.complete(slot, result); break;
    }
    case Operation::Nodes: {
        refreshStatus();
        if (request.offset && request.revision != _status.nodeRevision) { complete(slot, Outcome::Stale); break; }
        JsonDocument doc; auto rows = doc.to<JsonArray>();
        const size_t count = _nodes ? _nodes->nodes().size() : 0;
        const size_t end = std::min(count, size_t(request.offset) + 8);
        for (size_t i = request.offset; i < end; ++i) {
            const auto& node = _nodes->nodes()[i]; auto row = rows.add<JsonObject>();
            row["hash"] = node.hash.toHex(); row["name"] = node.name; row["identity"] = node.identityHex;
            row["rssi"] = node.rssi; row["snr"] = node.snr; row["hops"] = node.hops;
            row["seen"] = node.lastSeen; row["saved"] = node.saved;
        }
        Result result; result.revision = _status.nodeRevision; result.next = end; result.total = count; result.more = end < count;
        jsonResult(slot, doc, result); break;
    }
    case Operation::Conversations: {
        if (request.offset && request.revision != _messages.revision()) { complete(slot, Outcome::Stale); break; }
        const auto& conversations = _messages.conversations();
        const auto end = std::min(conversations.size(), size_t(request.offset) + 8);
        JsonDocument doc; auto rows = doc.to<JsonArray>();
        for (size_t i = request.offset; i < end; ++i) {
            const auto* s = _messages.getSummary(conversations[i]);
            if (!s) continue;
            auto row = rows.add<JsonObject>(); row["peer"] = conversations[i];
            row["name"] = _nodes ? _nodes->lookupName(conversations[i]) : std::string();
            row["time"] = s->lastTimestamp;
            row["preview"] = s->lastPreview; row["incoming"] = s->lastIncoming; row["unread"] = s->unreadCount;
            row["total"] = s->totalCount; row["outgoing"] = s->hasOutgoing; row["pending"] = s->hasPending;
            row["failed"] = s->hasFailed; row["status"] = uint8_t(s->lastOutgoingStatus);
            row["counter"] = s->lastOutgoingCounter; row["pendingCount"] = s->pendingCount; row["failedCount"] = s->failedCount;
        }
        Result result; result.revision = _messages.revision(); result.next = end; result.total = conversations.size(); result.more = end < conversations.size();
        jsonResult(slot, doc, result); break;
    }
    case Operation::History: case Operation::MessageBody: history(slot); break;
    case Operation::CloseHistory:
        closeHistory(); complete(slot, Outcome::Ok); break;
    case Operation::Identities: {
        JsonDocument doc; auto rows = doc.to<JsonArray>();
        for (const auto& identity : _identities.identities()) {
            auto row = rows.add<JsonObject>(); row["hash"] = identity.hash;
            row["name"] = identity.displayName; row["active"] = identity.active;
        }
        Result result; result.revision = _status.identityRevision;
        jsonResult(slot, doc, result); break;
    }
    case Operation::CreateIdentity: case Operation::ImportIdentity: {
        int index = request.operation == Operation::CreateIdentity
            ? _identities.createIdentity(_config.settings().displayName)
            : _identities.importIdentity(_config.settings().displayName);
        ++_status.identityRevision;
        complete(slot, index >= 0 ? Outcome::Ok : Outcome::Failed,
                 index >= 0 ? "Identity added" : "Identity operation failed"); break;
    }
    case Operation::Scan:
        if (_scanSlot != ServiceMailbox::NoSlot || !startScan || !finishScan) { complete(slot, Outcome::Failed, "Scan unavailable"); break; }
        _scanSlot = slot; _status.scanRunning = true; startScan(); break;
    case Operation::HomeReady:
        if (homeReady) homeReady(); complete(slot, Outcome::Ok); break;
    case Operation::Diagnostics:
        if (diagnostics) diagnostics(request.argument); complete(slot, Outcome::Ok); break;
    default: complete(slot, Outcome::Invalid); break;
    }
}

void DeviceService::closeHistory() {
    if (_historySlot != ServiceMailbox::NoSlot) complete(_historySlot, Outcome::Stale);
    _historySlot = ServiceMailbox::NoSlot;
    _history = {}; _historyMessage = {}; _historyQuery = 0; _historyMessageIndex = UINT32_MAX;
}

void DeviceService::history(uint8_t slot) {
    const auto request = _mailbox.request(slot);
    if (request.operation == Operation::History) {
        closeHistory();
        _messages.beginHistory(_history, request.peer);
        _historyQuery = request.query; _historyPeer = request.peer; _historyRevision = _messages.revision();
        _lastHistoryAccess = millis(); _historySlot = slot;
        return;
    }
    if (_historyQuery != request.query || _historyPeer != request.peer ||
        request.revision != _historyRevision || _historyRevision != _messages.revision()) {
        complete(slot, Outcome::Stale); return;
    }
    historyChunk(slot);
}

void DeviceService::historyChunk(uint8_t slot) {
    const auto request = _mailbox.request(slot);
    _lastHistoryAccess = millis();
    JsonDocument doc;
    Result result; result.total = _history.count; result.revision = _historyRevision;
    if (request.offset < _history.count) {
        if (_historyMessageIndex != request.offset) {
            if (!_messages.readHistory(_history, request.offset, _historyMessage)) {
                complete(slot, Outcome::Failed, "History read failed"); closeHistory(); return;
            }
            _historyMessageIndex = request.offset;
        }
        const auto& msg = _historyMessage;
        doc["source"] = msg.sourceHash.toHex(); doc["dest"] = msg.destHash.toHex();
        doc["time"] = msg.timestamp; doc["status"] = uint8_t(msg.status); doc["incoming"] = msg.incoming;
        doc["read"] = msg.read; doc["counter"] = msg.savedCounter; doc["receive"] = msg.receiveCounter;
        doc["msgid"] = msg.messageId.toHex();
        // Hex preserves bytes across UTF-8 chunk boundaries.
        const size_t offset = std::min(size_t(request.argument), msg.content.size());
        const size_t count = std::min(size_t(1024), msg.content.size() - offset);
        rs::Bytes bytes(reinterpret_cast<const uint8_t*>(msg.content.data() + offset), count);
        doc["body"] = bytes.toHex(); doc["bodyEnd"] = offset + count == msg.content.size();
        result.next = offset + count;
        result.more = request.offset + 1 < _history.count || !doc["bodyEnd"].as<bool>();
    }
    jsonResult(slot, doc, result);
    if (!result.more) closeHistory();
}

void DeviceService::runLifecycle(uint8_t slot) {
    const auto request = _mailbox.request(slot);
    const bool protocolFlushed = _backend.persistData();
    const bool contactsFlushed = !_nodes || _nodes->flushPending();
    const bool identityFlushed = _identities.flushPending();
    // Canonical settings are already durable; a missing/failed optional SD
    // backup must not make restart lose or roll back the committed settings.
    _config.flushPending(_sd, _flash);
    if (!protocolFlushed || !contactsFlushed || !identityFlushed) {
        _status.state = ServiceState::Failed;
        _mailbox.allowRecovery();
        complete(slot, Outcome::Failed, "Flush failed. Restart again to force; pending data may be lost.");
        refreshStatus();
        return;
    }
    bool ok = true;
    switch (request.operation) {
    case Operation::FormatSD: ok = _sd.isReady() && _sd.formatForRsDeck(); break;
    case Operation::WipeSD: case Operation::ClearOldDataAndRestart:
        ok = _sd.isReady() && _sd.wipeRsDeck();
        if (ok && _nodes && request.operation == Operation::ClearOldDataAndRestart) _nodes->clearAll();
        break;
    case Operation::FactoryReset:
        ok = (!_sd.isReady() || _sd.wipeRsDeck()) && _flash.format();
        if (ok) ok = nvs_flash_erase() == ESP_OK;
        break;
    case Operation::EnableSDAndRestart: {
        UserConfig candidate = _config; candidate.settings().sdStorageEnabled = true;
        ok = candidate.save(_sd, _flash); if (ok) _config = candidate; break;
    }
    case Operation::SwitchIdentity: {
        int index = -1;
        const auto& identities = _identities.identities();
        for (size_t i = 0; i < identities.size(); ++i) if (identities[i].hash == request.peer) index = i;
        ok = index >= 0 && _identities.switchTo(index);
        if (ok) {
            _config.settings().displayName = _identities.getDisplayName(index);
            ok = _config.save(_sd, _flash);
        }
        break;
    }
    default: break;
    }
    // Once quiesced, even an operation failure requires explicit reboot before
    // resuming networking; never reuse partially formatted/switched state.
    _status.state = ok ? ServiceState::Stopped : ServiceState::Failed;
    if (!ok) _mailbox.allowRecovery();
    complete(slot, ok ? Outcome::Ok : Outcome::Failed,
             ok ? "Ready to restart" : "Operation failed; restart required");
    refreshStatus();
}

} // namespace handheld
#endif
