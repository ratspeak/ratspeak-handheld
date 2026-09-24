#include "DeviceService.h"
#include "MaintenanceOperation.h"
#include "config/SettingsTransaction.h"
#include "util/AnnounceData.h"
#include "history/HistoryWindow.h"
#include "history/ConversationWindow.h"
#include "TaskOwner.h"
#if !defined(RSCARDPUTER)
#include "util/Bytes.h"
#include "config/Timezones.h"
#include "storage/MessageRecord.h"
#include "storage/StorageLease.h"
#include <esp_heap_caps.h>
#include <cassert>
#include <cstring>

namespace handheld {

namespace {
bool summaryQuery(Operation operation) {
    return operation == Operation::ConversationPage || operation == Operation::ConversationDetail;
}
size_t queryCapacity(Operation operation) {
    switch (operation) {
        case Operation::ConversationPage: return history::ConversationList::PageSize * sizeof(storage::ConversationSelector);
        case Operation::ConversationDetail: return sizeof(storage::ConversationView);
        case Operation::HistoryPage: return 48 * sizeof(storage::HistoryEntry);
        case Operation::ReadRecord: return 512;
        case Operation::HistoryStatus: return sizeof(history::HistoryWindow::StatusProjection);
        default: return 0;
    }
}
bool readConversationQuery(const ServiceMailbox& mailbox, uint8_t slot, ConversationQuery& query) {
    if (mailbox.length(slot) != sizeof(query) || !mailbox.read(slot, &query, sizeof(query)) ||
        query.hasCursor > 1 || !storage::validConversationOrder(query.order) ||
        !storage::validConversationDirection(query.direction) || !std::isfinite(query.selector.cursor.timestamp) ||
        query.selector.reserved || query.selector.error != storage::Error::None) return false;
    for (auto byte : query.reserved) if (byte) return false;
    if (mailbox.request(slot).operation == Operation::ConversationDetail)
        return storage::validConversationSelector(query.selector);
    return query.selector.counter == 0 && query.selector.incoming == 0;
}
} // namespace

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
    _backend.setMessageCallback([this](const LXMFManager::CommittedMessage&) { ++_status.incomingRevision; });
    // Durable store and transient outgoing status have separate revisions.
    _backend.setStatusCallback(nullptr);
    refreshStatus();
    return true;
}

void DeviceService::bindOwner() {
    // A refused transfer is terminal for this startup attempt. Mailbox recovery
    // must not silently adopt storage later when the previous owner progresses.
    if (_owner) {
        configASSERT(_owner == xTaskGetCurrentTaskHandle());
        return;
    }
    _owner = xTaskGetCurrentTaskHandle();
    _storageOwnerBound = _messages.adoptOwner();
    if (!_storageOwnerBound) {
        _status.state = ServiceState::Failed;
        _status.ready = false;
        notice("Storage owner transfer failed. Restart to force; pending data may be lost.");
        _mailbox.setAccepting(false);
        _mailbox.allowRecovery();
        refreshStatus();
        return;
    }
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

bool DeviceService::readyForCommand() {
    if (!_storageOwnerBound || _backend.pollRadioBeforeBlockingWork()) return true;
    const auto* request = _mailbox.nextRequest();
    if (!request || !_messages.deferredIO()) return false;
    switch (request->operation) {
        case Operation::Send: case Operation::MarkRead: case Operation::DeleteConversation:
        case Operation::ConversationPage: case Operation::ConversationDetail:
        case Operation::HistoryPage: case Operation::ReadRecord: case Operation::HistoryStatus:
            return true; // Typed worker submissions and result copies only.
        default: return false; // Settings, contacts and callbacks may write inline.
    }
}

void DeviceService::poll() {
    // Start the reserved request's admission clock even while an older normal
    // command is retained. Claiming control does not grant teardown permission.
    const auto control = _mailbox.takeLifecycle();
    if (control != ServiceMailbox::NoSlot) {
        if (_mailbox.request(control).generation != _status.generation) {
            complete(control, Outcome::Stale);
            if (_maintenance.accepting() && _status.state == ServiceState::Running)
                _mailbox.setAccepting(true);
        } else execute(control);
    }
    // Only the reserved explicit recovery result is ours after refusal. The
    // setup owner retains every storage/engine/driver and outstanding credit.
    if (!_storageOwnerBound) return;
    _backend.pollRadioBeforeBlockingWork();
    // Settlement only publishes completed worker results; it never performs I/O.
    // Keep it and network polling live while an independent radio is busy.
    _messages.poll();
    // Observe UI cancellation before the pump can start another queued frame.
    pollSends();
    if (_status.state == ServiceState::Running && pollNetwork) pollNetwork();
    else if (!_maintenance.accepting() && pollSettlements) pollSettlements();
    pollSends();
    pollStorageWrites();
    if (_backend.pollRadioBeforeBlockingWork()) pollSettings();
    if (_status.state == ServiceState::Running && millis() - _lastIdentityRetry >= 30000 &&
        _backend.pollRadioBeforeBlockingWork()) {
        _lastIdentityRetry = millis();
        if (!_identities.flushPending()) notice("Identity metadata save failed; retry pending");
        if (!_config.flushPending(_sd, _flash)) notice("Settings saved on device; backup retry pending");
    }
    if (_backend.pollRadioBeforeBlockingWork() || _messages.deferredIO()) pollHistory();
    if (_scanSlot != ServiceMailbox::NoSlot && finishScan) {
        String json;
        const auto scan = finishScan(json);
        if (scan != ScanResult::Pending) {
            Result result;
            result.outcome = scan == ScanResult::Cancelled ? Outcome::Cancelled : Outcome::Failed;
            if (scan == ScanResult::Ready && !json.isEmpty() &&
                json.length() <= _mailbox.capacity(_scanSlot) &&
                _mailbox.write(_scanSlot, json.c_str(), json.length())) {
                result.length = json.length(); result.outcome = Outcome::Ok;
            } else strlcpy(result.detail, scan == ScanResult::Cancelled ? "Scan cancelled" : "Scan failed",
                           sizeof(result.detail));
            _mailbox.complete(_scanSlot, result);
            _scanSlot = ServiceMailbox::NoSlot;
            _status.scanRunning = false;
        }
    }
    pollMaintenance();
}

void DeviceService::finishSettings(uint8_t slot) {
    const auto request = _mailbox.request(slot);
    const auto radio = request.argument && applyRadio
        ? applyRadio(_config.settings(), _maintenance.accepting()) : RadioApply::Applied;
    if (_maintenance.accepting() && radio == RadioApply::Pending) {
        _settingsSlot = slot;
        if (strcmp(_status.notice, "Settings saved; waiting for radio") != 0)
            notice("Settings saved; waiting for radio");
        return;
    }
    ++_status.configRevision;
    ++_status.identityRevision;
    if (_maintenance.accepting()) {
        if (_config.settings().timezoneIdx < TIMEZONE_COUNT) {
            setenv("TZ", TIMEZONE_TABLE[_config.settings().timezoneIdx].posixTZ, 1);
            tzset();
        }
        if (applyPeripherals) applyPeripherals(_config.settings());
    }
    // During maintenance the records may settle, but UI callbacks must not
    // reopen onboarding or perform hardware actions after shutdown admission.
    Result result;
    result.settingsCommitted = true;
    result.outcome = _maintenance.accepting() ? Outcome::Ok : Outcome::Cancelled;
    result.revision = _status.configRevision;
    const String encoded = _config.encode();
    if (!encoded.isEmpty() && encoded.length() <= _mailbox.capacity(slot) &&
        _mailbox.write(slot, encoded.c_str(), encoded.length())) result.length = encoded.length();
    else result.outcome = Outcome::NotReady;
    strlcpy(result.detail, !_maintenance.accepting() ? "Settings saved; shutdown in progress" :
        radio == RadioApply::RebootRequired ? "Settings saved; radio enable needs reboot" :
        radio == RadioApply::Unavailable ? "Settings saved; radio unavailable" :
        _config.mirrorPending() ? "Settings saved on device; backup pending" : "Settings saved", sizeof result.detail);
    _settingsSlot = ServiceMailbox::NoSlot;
    _mailbox.complete(slot, result);
}

void DeviceService::pollSettings() {
    if (_config.settingsPending()) {
        if (millis() - _lastIdentityRetry < 1000) return;
        _lastIdentityRetry = millis();
        const auto saved = SettingsTransaction::recover(_config, _identities, _sd, _flash);
        if (!saved.complete()) { notice(saved.detail); return; }
    }
    if (_settingsSlot != ServiceMailbox::NoSlot) {
        finishSettings(_settingsSlot);
    }
}

void DeviceService::pollMaintenance() {
    if (_maintenance.accepting()) return;
    MaintenanceBarrier::Snapshot snapshot;
    snapshot.normalPending = _mailbox.normalWorkPending() || _config.settingsPending();
    snapshot.applicationPending = !_backend.lxmfDrained();
    snapshot.helpersPending = !_maintenance.helpersStarted() || (quiescent && !quiescent());
    snapshot.storageStopped = _maintenance.storageStopStarted() && _messages.finishStop();
    snapshot.error = _backend.lxmfDrainError();
    snapshot.unrecoverable = settlementFailed && settlementFailed();
    if (snapshot.error != storage::Error::None && snapshot.error != _status.maintenanceError &&
        _maintenance.phase() != MaintenanceBarrier::Phase::Failed)
        notice("Saving message status; storage retry pending");
    const auto actions = _maintenance.step(millis(), snapshot);
    _status.maintenancePending = _maintenance.pending();
    _status.maintenanceError = _maintenance.lastError();
    if (actions & MaintenanceBarrier::ReportFailure) {
        _status.state = ServiceState::Failed;
        _mailbox.allowRecovery();
        Result result;
        result.outcome = Outcome::Failed;
        result.storageError = _maintenance.lastError();
        result.retainedOwners = _maintenance.failedPending();
        _status.maintenanceFailedPending = result.retainedOwners;
        strlcpy(result.detail, snapshot.unrecoverable
            ? "Radio failed. Restart again to force; pending data may be lost."
            : snapshot.normalPending ? "Command still busy. Restart again to force; data may be lost."
            : snapshot.applicationPending ? "Message work pending. Restart again to force; data may be lost."
            : snapshot.helpersPending ? "Connection still busy. Restart again to force; data may be lost."
            : "Storage still busy. Restart again to force; data may be lost.", sizeof(result.detail));
        const auto slot = _lifecycleSlot;
        _lifecycleSlot = ServiceMailbox::NoSlot;
        if (slot != ServiceMailbox::NoSlot) _mailbox.complete(slot, result);
        notice(result.detail);
        refreshStatus();
    }
    if (actions & MaintenanceBarrier::BeginHelpers) {
        if (beginQuiesce) beginQuiesce();
    }
    if (actions & MaintenanceBarrier::StopStorage) _messages.requestStop();
    if (actions & MaintenanceBarrier::Perform) {
        const auto slot = _lifecycleSlot;
        _lifecycleSlot = ServiceMailbox::NoSlot;
        runLifecycle(slot);
    }
}

void DeviceService::refreshStatus() {
    _lastStatus = millis();
    _status.heartbeat = _lastStatus;
    // begin() publishes Starting before any binding attempt, when setup still
    // owns these objects. A refused attempt may publish only that cached view.
    if (_owner && !_storageOwnerBound) {
        _mailbox.publishStatus(_status);
        return;
    }
    _status.ready = _backend.protocolReady();
    _status.transport = _backend.isTransportActive();
    _status.paths = _backend.pathCount();
    _status.links = _backend.linkCount();
    _status.queued = _backend.lxmfQueuedCount();
    _status.resources = _backend.activeResourceTransfers();
    _status.lastAnnounce = _backend.lastAnnounceTime();
    _status.unread = _messages.totalUnreadCount();
    _status.storeRevision = _messages.revision();
    _status.historyRevision = _messages.historyRevision();
    _status.statusRevision = _backend.lxmfStatusRevision();
    _status.flash = _flash.isReady();
    _status.sd = _sd.isReady();
    if ((!_lastStorageStatus || _lastStatus - _lastStorageStatus >= 5000) &&
        _backend.pollRadioBeforeBlockingWork()) {
        storage::StorageLease lease;
        // The storage worker may own a long scan. Preserve the last sample
        // until it finishes; lease contention is not an empty filesystem.
        if (lease.held()) {
            _lastStorageStatus = _lastStatus;
            _status.flashUsed = _flash.usedBytes();
            _status.flashTotal = _flash.totalBytes();
        }
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

void DeviceService::pollSends() {
    for (uint8_t slot = 0; slot < ServiceMailbox::NormalSlots; ++slot) {
        auto& pending = _sends[slot];
        if (!pending.ticket.valid()) continue;
        const auto ticket = pending.ticket;
        const auto generation = _mailbox.request(slot).generation;
        if (!pending.cancelled && (_mailbox.cancellationRequested(slot) || generation != _status.generation)) {
            _backend.lxmfCancel(ticket);
            pending.cancelled = true;
        }
        outgoing::InitialResult initial;
        const auto state = _backend.lxmfPoll(ticket, initial);
        if (state == outgoing::Poll::Pending) continue;
        bool cancelled;
        if (!_mailbox.claimCompletion(slot, cancelled)) continue;
        if (cancelled && !pending.cancelled) {
            _backend.lxmfCancel(ticket);
            pending.cancelled = true;
        }
        Result result;
        result.key = initial.key;
        result.revision = initial.revision;
        result.storageError = initial.error;
        result.txSuppressed = initial.txSuppressed || pending.cancelled;
        if (state == outgoing::Poll::Invalid) {
            result.storageError = storage::Error::Internal;
            strlcpy(result.detail, "Message result unavailable", sizeof(result.detail));
        } else if (initial.outcome == storage::Outcome::Committed) {
            result.outcome = Outcome::Ok;
            strlcpy(result.detail, result.txSuppressed ? "Message saved; sending cancelled" : "Message saved",
                    sizeof(result.detail));
        } else if (initial.outcome == storage::Outcome::Cancelled) {
            result.outcome = Outcome::Cancelled;
            result.txSuppressed = true;
            strlcpy(result.detail, "Sending cancelled", sizeof(result.detail));
        } else strlcpy(result.detail, "Message not saved", sizeof(result.detail));
        if (generation != _status.generation) result.outcome = Outcome::Stale;
        // No callback/body survives acknowledgement, which may release the
        // descriptor for another producer. The mailbox still owns this result.
        pending = {};
        if (state == outgoing::Poll::Ready) _backend.lxmfAcknowledge(ticket);
        _mailbox.publishCompletion(slot, result);
    }
}

void DeviceService::pollStorageWrites() {
    for (uint8_t slot = 0; slot < ServiceMailbox::NormalSlots; ++slot) {
        const auto ticket = _storageWrites[slot];
        if (!ticket.valid()) continue;
        storage::Result stored;
        if (!_messages.peekResult(ticket, stored)) continue;
        uint8_t peer[16];
        const auto& request = _mailbox.request(slot);
        const bool deleting = request.operation == Operation::DeleteConversation;
        const bool validPeer = deleting && storage::decodeHex(request.peer,
            strnlen(request.peer, sizeof(request.peer)), peer, 16);
        _storageWrites[slot] = {};
        _messages.releaseResult(ticket);
        if (validPeer) _backend.lxmfFinishPeerDelete(peer, stored);
        Result result;
        result.key = stored.key; result.revision = stored.revision; result.storageError = stored.error;
        result.outcome = stored.outcome == storage::Outcome::Committed ? Outcome::Ok : Outcome::Failed;
        if (result.outcome != Outcome::Ok) strlcpy(result.detail,
            deleting ? "Conversation not deleted" : "Couldn't mark messages as read", sizeof(result.detail));
        _mailbox.complete(slot, result);
    }
}

void DeviceService::complete(uint8_t slot, Outcome outcome, const char* detail) {
    Result result; result.outcome = outcome;
    result.txSuppressed = outcome == Outcome::Cancelled;
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
    if (!_storageOwnerBound &&
        !(_status.state == ServiceState::Failed && request.operation == Operation::Restart)) {
        complete(slot, Outcome::NotReady, "Storage owner unavailable");
        return;
    }
    if (lifecycleOperation(request.operation)) {
        if (_status.state == ServiceState::Failed && request.operation == Operation::Restart) {
            // Explicit second restart after the visible failure warning. Never
            // restart automatically on a failed flush, or resume a second owner.
            complete(slot, Outcome::Ok, "Forced restart after storage failure");
            return;
        }
        if (_status.state != ServiceState::Running ||
            !_maintenance.begin({request.id, request.generation}, request.admittedAt)) {
            complete(slot, Outcome::NotReady); return;
        }
        _status.state = ServiceState::Quiescing;
        _lifecycleSlot = slot;
        _outgoingPaused = true;
        _backend.lxmfStopAdmissions();
        if (closeAdmissions) closeAdmissions();
        notice("Finishing pending work");
        refreshStatus();
        return;
    }
    if (!_maintenance.accepting() && (request.operation == Operation::Announce ||
        request.operation == Operation::Diagnostics || request.operation == Operation::HomeReady ||
        request.operation == Operation::Scan)) {
        complete(slot, Outcome::Cancelled, "Cancelled for maintenance"); return;
    }
    const auto length = _mailbox.length(slot);
    if (!_mailbox.read(slot, _scratch, length)) { complete(slot, Outcome::Invalid); return; }
    _scratch[length] = 0;
    switch (request.operation) {
    case Operation::Send: {
        if (_outgoingPaused || _mailbox.cancellationRequested(slot)) {
            complete(slot, Outcome::Cancelled, "Sending cancelled"); break;
        }
        rs::Bytes destination; destination.assignHex(request.peer);
        if (destination.size() != 16 || !length || std::strlen(_scratch) != length) {
            complete(slot, Outcome::Invalid, "Invalid message"); break;
        }
        const auto submitted = _backend.lxmfSubmit(destination.data(), nullptr, 0,
            reinterpret_cast<const uint8_t*>(_scratch), length, request.argument != 0);
        if (submitted.accepted()) _sends[slot] = {submitted.ticket, false};
        else complete(slot, Outcome::Failed, "Message not saved; try again");
        break;
    }
    case Operation::MarkRead: {
        const auto submitted = _messages.requestMarkRead(request.peer, request.generation);
        if (submitted.accepted()) _storageWrites[slot] = submitted.ticket;
        else complete(slot, Outcome::Failed, "Couldn't mark messages as read");
        break;
    }
    case Operation::DeleteConversation: {
        uint8_t peer[16];
        if (!storage::decodeHex(request.peer, strnlen(request.peer, sizeof(request.peer)), peer, 16)) {
            complete(slot, Outcome::Invalid); break;
        }
        if (!_backend.lxmfBeginPeerDelete(peer)) {
            complete(slot, Outcome::Failed, "Deletion busy; try again"); break;
        }
        const auto submitted = _messages.requestDelete(request.peer, request.generation);
        if (submitted.accepted()) _storageWrites[slot] = submitted.ticket;
        else {
            storage::Result failure; failure.error = storage::Error::Unavailable;
            _backend.lxmfFinishPeerDelete(peer, failure);
            complete(slot, Outcome::Failed, "Conversation not deleted");
        }
        break;
    }
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
        if (settingsApplyPending()) { complete(slot, Outcome::NotReady, "Settings apply pending"); break; }
        const auto bytes = encodeAnnounceName(_config.settings().displayName);
        const auto sent = _backend.announce(bytes.data(), bytes.size());
        complete(slot, sent == ProtocolBackend::AnnounceResult::Failed ? Outcome::Failed : Outcome::Ok,
                 sent == ProtocolBackend::AnnounceResult::Sent ? "Announce sent" :
                 sent == ProtocolBackend::AnnounceResult::Deferred ? "Announce queued" : "Announce not sent");
        break;
    }
    case Operation::ApplySettings: {
        if (_settingsSlot != ServiceMailbox::NoSlot || _config.settingsPending()) { complete(slot, Outcome::NotReady, "Settings recovery pending"); break; }
        if (request.revision != _status.configRevision) { complete(slot, Outcome::Stale, "Settings changed; try again"); break; }
        UserConfig candidate;
        if (!candidate.tryAssign(_config)) { complete(slot, Outcome::NotReady, "Settings memory unavailable; retry"); break; }
        if (!candidate.decode(_scratch, strlen(_scratch))) { complete(slot, Outcome::Invalid, "Invalid settings"); break; }
        {
            const String encoded = candidate.encode();
            if (encoded.isEmpty() || encoded.length() > _mailbox.capacity(slot)) {
                complete(slot, Outcome::Invalid, "Settings exceed transfer budget"); break;
            }
        }
        const auto saved = SettingsTransaction::apply(_config, candidate, _identities, _sd, _flash);
        if (saved.state == SettingsTransaction::State::Pending) {
            _settingsSlot = slot;
            _lastIdentityRetry = millis();
            notice(saved.detail);
        } else if (saved.complete()) { _settingsSlot = slot; finishSettings(slot); }
        else complete(slot, Outcome::Failed, saved.detail);
        break;
    }
    case Operation::Settings: {
        const String json = _config.encode();
        if (json.isEmpty() || json.length() > _mailbox.capacity(slot) ||
            !_mailbox.write(slot, json.c_str(), json.length())) {
            complete(slot, Outcome::NotReady, "Settings snapshot unavailable; retry"); break;
        }
        Result result; result.outcome = Outcome::Ok; result.length = json.length(); result.revision = _status.configRevision;
        _mailbox.complete(slot, result); break;
    }
    case Operation::PeerName: {
        uint8_t peer[16];
        if (!storage::decodeHex(request.peer, strnlen(request.peer, sizeof(request.peer)), peer, sizeof(peer))) {
            complete(slot, Outcome::Invalid); break;
        }
        // The announce owner retains names for peers evicted from the live
        // node list. Query it without rebuilding another UI summary/name cache.
        char canonical[33]; storage::encodeHex(peer, sizeof(peer), canonical);
        const auto name = _nodes ? _nodes->lookupName(canonical) : std::string();
        if (name.size() > _mailbox.capacity(slot) || !_mailbox.write(slot, name.data(), name.size())) {
            complete(slot, Outcome::Failed); break;
        }
        Result result; result.outcome = Outcome::Ok; result.length = name.size();
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
    case Operation::ConversationPage: case Operation::ConversationDetail:
    case Operation::HistoryPage: case Operation::ReadRecord: case Operation::HistoryStatus:
        history(slot); break;
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

void DeviceService::history(uint8_t slot) {
    const auto& request = _mailbox.request(slot);
    uint8_t peer[16];
    if (_querySlot != ServiceMailbox::NoSlot) {
        complete(slot, Outcome::NotReady, "History reader busy"); return;
    }
    if (summaryQuery(request.operation)) {
        ConversationQuery query;
        if (!readConversationQuery(_mailbox, slot, query)) { complete(slot, Outcome::Invalid); return; }
    } else if (!storage::decodeHex(request.peer, strnlen(request.peer, sizeof(request.peer)), peer, 16) ||
        (request.operation != Operation::HistoryPage && !request.argument) ||
        (request.operation == Operation::HistoryStatus && request.incoming) ||
        (request.operation == Operation::HistoryPage &&
         request.historyDirection != storage::HistoryDirection::Before &&
         request.historyDirection != storage::HistoryDirection::After)) {
        complete(slot, Outcome::Invalid); return;
    }
    const size_t required = queryCapacity(request.operation);
    if (_mailbox.capacity(slot) < required) { complete(slot, Outcome::Invalid); return; }
    _querySlot = slot; _queryTicket = {}; _queryRetryAt = millis();
    pollHistory();
}

void DeviceService::finishHistory(const storage::Result& stored) {
    const auto slot = _querySlot;
    const auto& request = _mailbox.request(slot);
    Result result;
    result.key = stored.key; result.revision = stored.revision;
    result.next = stored.nextOffset; result.total = stored.total; result.more = stored.more;
    result.storageError = stored.error;
    result.outcome = stored.outcome == storage::Outcome::Committed ? Outcome::Ok : Outcome::Failed;
    if (request.operation == Operation::HistoryStatus) {
        handheld::history::HistoryWindow::StatusProjection row;
        row.counter = request.argument; row.error = stored.error;
        storage::StoredRecordHeader header;
        if (stored.outcome == storage::Outcome::Committed && stored.error == storage::Error::None &&
            stored.length == sizeof(header) && _messages.readPayload(_queryTicket, &header, sizeof(header)) &&
            header.counter == request.argument && !header.incoming &&
            !memcmp(header.destination, stored.key.peer, 16) && header.status <= uint8_t(LXMFStatus::UNCONFIRMED)) {
            row.desired = row.durable = header.status;
            row.flags = handheld::history::HistoryWindow::StatusProjection::Available;
        } else if (row.error == storage::Error::None) row.error = storage::Error::InvalidRecord;
        result.statusRevision = request.statusRevision;
        // Retirement/transition during the read invalidates its sampled overlay.
        if (request.statusRevision != _backend.lxmfStatusRevision()) {
            result.outcome = Outcome::Stale; result.storageError = storage::Error::Stale;
        }
        result.length = sizeof(row); result.more = false; result.next = result.total = 0;
        if (!_mailbox.write(slot, &row, sizeof(row))) {
            result.outcome = Outcome::Failed; result.storageError = storage::Error::Internal;
            result.length = 0;
        }
    } else if (stored.length > queryCapacity(request.operation) ||
               !_messages.readPayload(_queryTicket, _scratch, stored.length) ||
               !_mailbox.write(slot, _scratch, stored.length)) {
        result.outcome = Outcome::Failed; result.storageError = storage::Error::Internal;
    } else result.length = stored.length;
    // Copy while the storage credit is held, then release before mailbox
    // publication. The UI owns only the independent bounded mailbox copy.
    const bool released = _messages.releaseResult(_queryTicket);
    configASSERT(released);
    if (!released) return; // Keep the exact storage and mailbox owners on invariant failure.
    _queryTicket = {}; _querySlot = ServiceMailbox::NoSlot;
    _mailbox.complete(slot, result);
}

void DeviceService::pollHistory() {
    if (_querySlot == ServiceMailbox::NoSlot) return;
    const auto& request = _mailbox.request(_querySlot);
    if (_queryTicket.valid()) {
        storage::Result stored;
        if (_messages.peekResult(_queryTicket, stored)) finishHistory(stored);
        return;
    }
    if (int32_t(millis() - _queryRetryAt) < 0) return;
    storage::RecordKey key;
    storage::decodeHex(request.peer, strnlen(request.peer, sizeof(request.peer)), key.peer, 16);
    key.counter = request.argument; key.incoming = request.incoming;
    if (request.operation == Operation::HistoryStatus) {
        Result result; result.key = key; result.statusRevision = request.statusRevision;
        if (request.statusRevision != _backend.lxmfStatusRevision()) {
            const auto slot = _querySlot; _querySlot = ServiceMailbox::NoSlot;
            result.outcome = Outcome::Stale; result.storageError = storage::Error::Stale;
            _mailbox.complete(slot, result); return;
        }
        outgoing::StatusView overlay;
        if (_backend.lxmfStatus(key, overlay)) {
            handheld::history::HistoryWindow::StatusProjection row;
            row.counter = key.counter; row.desired = uint8_t(overlay.desired); row.durable = uint8_t(overlay.durable);
            row.error = overlay.error;
            row.flags = handheld::history::HistoryWindow::StatusProjection::Available |
                (overlay.pending ? handheld::history::HistoryWindow::StatusProjection::Pending : 0) |
                (overlay.txSuppressed ? handheld::history::HistoryWindow::StatusProjection::TxSuppressed : 0);
            result.outcome = Outcome::Ok; result.length = sizeof(row);
            if (!_mailbox.write(_querySlot, &row, sizeof(row))) {
                result.outcome = Outcome::Failed; result.storageError = storage::Error::Internal;
                result.length = 0;
            }
            const auto slot = _querySlot; _querySlot = ServiceMailbox::NoSlot;
            _mailbox.complete(slot, result); return;
        }
    }
    storage::Submission submitted;
    if (summaryQuery(request.operation)) {
        ConversationQuery query;
        if (!readConversationQuery(_mailbox, _querySlot, query)) {
            const auto slot = _querySlot; _querySlot = ServiceMailbox::NoSlot;
            complete(slot, Outcome::Invalid); return;
        }
        memcpy(key.peer, query.selector.cursor.peer, 16); key.counter = query.selector.counter;
        key.incoming = query.selector.incoming;
        submitted = request.operation == Operation::ConversationPage ?
            _messages.requestConversationPage(query.selector.cursor, query.hasCursor, query.order, query.direction, history::ConversationList::PageSize) :
            _messages.requestConversation(query.selector);
    } else submitted = request.operation == Operation::HistoryPage ?
        _messages.requestHistoryPage(request.peer, {request.argument, request.incoming}, 48, request.historyDirection) :
        _messages.requestRecord(key, request.operation == Operation::HistoryStatus ? 0 : request.offset,
            request.operation == Operation::HistoryStatus ? sizeof(storage::StoredRecordHeader) : 512);
    if (submitted.accepted()) { _queryTicket = submitted.ticket; return; }
    // Busy means another bounded ticket owns the slot. Poll admission again
    // after owner settlement; a one-second timer here delays ordinary reads
    // even when that slot is released on the very next service tick.
    if (submitted.rejection == storage::Rejection::Busy) return;
    if (submitted.rejection == storage::Rejection::NoMemory) {
        _queryRetryAt = millis() + 1000; return;
    }
    const auto slot = _querySlot; _querySlot = ServiceMailbox::NoSlot;
    Result result; result.outcome = Outcome::Failed; result.storageError = storage::Error::Unavailable;
    result.key = key; _mailbox.complete(slot, result);
}

void DeviceService::runLifecycle(uint8_t slot) {
    const auto request = _mailbox.request(slot);
    const auto result = performMaintenance(_maintenance, request, _backend, _config,
        _identities, _flash, _sd, _nodes);
    _status.state = result.ok ? ServiceState::Stopped : ServiceState::Failed;
    if (!result.ok) _mailbox.allowRecovery();
    complete(slot, result.ok ? Outcome::Ok : Outcome::Failed, result.detail);
    refreshStatus();
}

} // namespace handheld
#endif
