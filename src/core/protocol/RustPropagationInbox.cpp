#include "RustPropagationInbox.h"
#include "RustLxmfEngine.h"
#include "RustClock.h"
#include "RustEntropy.h"
#include "RustInterfacePump.h"
#include "RustResourceEngine.h"
#include "storage/MessageStore.h"
#include <algorithm>

using namespace handheld::storage;
using handheld::propagation::Nodes;
using handheld::propagation::Selection;

void RustPropagationInbox::begin(RustLxmfEngine& engine) {
    configASSERT(drained());
    *this = RustPropagationInbox{}; _engine = &engine;
}

handheld::propagation::SyncView RustPropagationInbox::view() const {
    handheld::propagation::SyncView view;
    memcpy(view.node, _node, 16); view.status = _status; view.received = _received;
    view.busy = _stage != Stage::Idle || _requested;
    return view;
}

bool RustPropagationInbox::requestSync() {
    if (!_engine || !_engine->_accepting || !_engine->_d.propagation ||
        !_engine->_d.propagation->settings().enabled || _stage != Stage::Idle || _requested) return false;
    const uint64_t now = _engine->_d.clock->nowMs();
    if (_hasStarted && now - _started < 30000) return false;
    _requested = true; _next = now; _status = Status::Waiting; return true;
}

bool RustPropagationInbox::allowed() const {
    if (!_engine || !_engine->_accepting || _stopping || !_engine->_d.propagation) return false;
    const auto& nodes = *_engine->_d.propagation;
    const auto& settings = nodes.settings();
    return settings.enabled && nodes.owns(_owner) &&
        (_stage == Stage::Select || settings.selection != Selection::Manual ||
         (settings.hasManual && !memcmp(settings.manual, _node, 16)));
}

void RustPropagationInbox::finish(Status status, bool networkFailure) {
    if (!_engine) return;
    auto& d = _engine->_d;
    if (d.links) d.links->cancelRequest(*this);
    if (_incoming.valid()) _engine->_incoming.releaseHeld(_incoming);
    _incoming = {};
    if (d.propagation) {
        if (_owner && networkFailure) d.propagation->outcome(_node, false, d.clock->nowMs());
        d.propagation->release(_owner);
    }
    _owner = 0; _status = status; _requested = false;
    // A failed/cancelled accepted store operation still owns its terminal credit.
    if (_storage.valid()) { d.store->cancel(_storage); _stage = Stage::Drain; }
    else _stage = Stage::Idle;
    const uint64_t cadence = _interface == 0 ? 1800000 : 300000;
    uint16_t jitter = 0; RustEntropy::fill(reinterpret_cast<uint8_t*>(&jitter), sizeof jitter);
    _next = d.clock->nowMs() + cadence + jitter % 30000;
}

void RustPropagationInbox::stop() { _stopping = true; finish(Status::Off); }

void RustPropagationInbox::dropPeer(const uint8_t peer[16]) {
    if (_stage != Stage::Idle && peer && !memcmp(_journal.bytes + 48, peer, 16)) finish(Status::StorageError);
}

void RustPropagationInbox::discard(Status after) {
    _afterClear = after; _stage = Stage::Clear; _status = Status::Saving;
}

void RustPropagationInbox::nextItem() {
    if (_received >= 4 || _attempts >= 8) { finish(_skipped); return; }
    _journal = {}; _sourceSince = 0; _linkSince = 0; _requestAt = 0;
    _stepSince = _engine->_d.clock->nowMs(); _stage = Stage::List;
}

void RustPropagationInbox::skip(Status why) {
    _skipped = why;
    if (_index == UINT8_MAX) { finish(why); return; }
    ++_index; nextItem();
}

bool RustPropagationInbox::ensureLink() {
    auto& d = _engine->_d;
    const uint64_t now = d.clock->nowMs();
    const auto* node = d.propagation->find(_node);
    rs_handheld_route_t route{};
    if (!node || !Nodes::usable(*node, now) ||
        rs_handheld_rns_route(d.ctx, _node, now, &route) != RS_HANDHELD_OK ||
        route.kind != RS_HANDHELD_ROUTE_DIRECT || !d.pump->interfaceGeneration(route.interface_id)) {
        if (node && node->cost > Nodes::MaxStampCost) { finish(Status::StampCostHigh); return false; }
        if (now - _stepSince >= 60000) { finish(Status::Unavailable, true); return false; }
        if (now >= _requestAt) {
            uint8_t tag[16]; RustEntropy::fill(tag, sizeof tag);
            rs_handheld_rns_request_path(d.ctx, _node, tag, 0, now); _requestAt = now + 10000;
        }
        _status = Status::Connecting; return false;
    }
    _interface = route.interface_id;
    // A full local pool does not consume a peer handshake timeout.
    _stepSince = now;
    const bool active = d.links->ensureLink(_node, node->publicKey, route);
    if (!_owner) return false; // An admitted failure may notify synchronously.
    if (!active && d.links->linkEstablishing(_node)) {
        if (!_linkSince) _linkSince = now + 1;
        else if (now >= _linkSince && now - _linkSince > 60000 + d.pump->interfaceTxWaitMs(route.interface_id, 4)) {
            finish(Status::Unavailable, true); return false;
        }
    }
    if (!active || d.resources->activeTransfers()) { _status = Status::Connecting; return false; }
    _linkSince = 0;
    return d.links->linkOnRoute(_node, route);
}

void RustPropagationInbox::settleStorage() {
    if (!_storage.valid()) return;
    auto& store = *_engine->_d.store;
    Result result;
    if (!store.peekResult(_storage, result)) return;
    const Stage stage = _stage;
    bool copied = false;
    if ((stage == Stage::Load || stage == Stage::Write) && result.length == purge::Size)
        copied = store.readPayload(_storage, _journal.bytes, purge::Size) && _journal.valid() &&
                 !memcmp(_journal.local(), _engine->_d.ourDestHash, 16);
    store.releaseResult(_storage); _storage = {};
    if (stage == Stage::Drain) { _stage = Stage::Idle; return; }
    if (!allowed()) { finish(Status::Off); return; }
    if (result.outcome != Outcome::Committed || result.error != Error::None) {
        if (stage == Stage::Load && copied && (result.error == Error::Stale || result.error == Error::InvalidRecord))
            discard(Status::StorageError);
        else finish(Status::StorageError);
        return;
    }
    if (stage == Stage::Load) {
        if (!result.length) { nextItem(); return; }
        if (!copied) { finish(Status::StorageError); return; }
        if (memcmp(_journal.node(), _node, 16)) { discard(Status::Idle); return; }
        _stage = Stage::Purge;
    } else if (stage == Stage::Write) {
        if (!copied || memcmp(_journal.node(), _node, 16)) { finish(Status::StorageError); return; }
        if (_incoming.valid()) _engine->_incoming.releaseHeld(_incoming);
        _incoming = {}; _stage = Stage::Purge;
    } else if (stage == Stage::Clear) {
        if (_afterClear == Status::StorageError) { finish(_afterClear); return; }
        if (_afterClear == Status::Complete) ++_received;
        nextItem();
    }
}

void RustPropagationInbox::poll() {
    if (!_engine) return;
    auto& d = _engine->_d;
    settleStorage();
    if (_stage == Stage::Drain) return;
    if (!d.propagation || !d.links || !d.resources) return;
    const uint64_t now = d.clock->nowMs();
    const bool enabled = !_stopping && _engine->_accepting && d.propagation->settings().enabled;
    if (!enabled) {
        if (_stage != Stage::Idle || _requested) finish(Status::Off);
        _status = Status::Off; _enabled = false; return;
    }
    if (!_enabled) {
        _enabled = true;
        if (!_requested) { _next = now + 60000; _status = Status::Idle; }
    }
    if (_stage == Stage::Idle) {
        if (now < _next || _engine->_deleting || d.resources->activeTransfers() || d.links->requestPending()) return;
        _owner = d.propagation->claim();
        if (!_owner) { _status = Status::Waiting; return; }
        _journal = {};
        _started = _stepSince = now; _hasStarted = true; _requested = false;
        _requestAt = _sourceSince = _linkSince = 0;
        _received = _attempts = 0; _skipped = Status::Complete;
        _stage = Stage::Select;
    }
    if (!allowed()) { finish(Status::Off); return; }
    if (_storage.valid()) return;
    // Whole-cycle work is finite too. Local contention ends this cycle without
    // penalizing the node; ordinary cadence/manual sync can start a later one.
    if (now - _started >= 600000 + uint64_t(d.pump->interfaceTxWaitMs(_interface, 32))) {
        finish(Status::Waiting); return;
    }
    if (_stage == Stage::Select) {
        const auto* node = d.propagation->select(now, true, _owner);
        if (!node) {
            _status = Status::Connecting;
            const auto& settings = d.propagation->settings();
            if (settings.selection == Selection::Manual && settings.hasManual) {
                if (memcmp(_node, settings.manual, 16)) _index = 0;
                memcpy(_node, settings.manual, 16);
                const auto* manual = d.propagation->find(_node);
                if (manual && manual->cost > Nodes::MaxStampCost) { finish(Status::StampCostHigh); return; }
                if (now >= _requestAt) {
                    uint8_t tag[16]; RustEntropy::fill(tag, sizeof tag);
                    rs_handheld_rns_request_path(d.ctx, _node, tag, 0, now); _requestAt = now + 10000;
                }
            }
            if (now - _started >= 60000) finish(Status::Unavailable);
            return;
        }
        if (memcmp(_node, node->address, 16)) _index = 0;
        memcpy(_node, node->address, 16); _interface = node->interface; _stage = Stage::Load;
    }
    // The immediate executor must obey the same physical-radio I/O guard as
    // other message admissions; completed results above never require it.
    if (!d.store->deferredIO() && !d.pump->pollRadioBeforeBlockingWork()) return;
    Submission submitted;
    if (_stage == Stage::Load) submitted = d.store->requestPurgeJournal(d.ourDestHash);
    else if (_stage == Stage::Write) submitted = d.store->requestWritePurge(_journal);
    else if (_stage == Stage::Clear) submitted = d.store->requestClearPurge(_journal);
    else if (_stage == Stage::Commit) {
        RustIncomingDelivery::CommitResult committed;
        const auto state = _engine->_incoming.heldCommit(_incoming, committed);
        if (state == RustIncomingDelivery::CommitState::Pending) return;
        if (state != RustIncomingDelivery::CommitState::Committed) { finish(Status::StorageError); return; }
        uint8_t transient[32]; memcpy(transient, _journal.transientId(), 32);
        if (!purge::Journal::make(_journal, committed.key, committed.revision, d.ourDestHash, _node,
                                   committed.messageId, transient)) { finish(Status::StorageError); return; }
        _stage = Stage::Write; return;
    } else if (_stage == Stage::List || _stage == Stage::Fetch || _stage == Stage::Purge) {
        if (now < _requestAt || !ensureLink()) return;
        const Stage sending = _stage;
        const uint8_t operation = sending == Stage::List ? 0 : sending == Stage::Fetch ? 1 : 2;
        _stage = sending == Stage::List ? Stage::ListWait : sending == Stage::Fetch ? Stage::FetchWait : Stage::PurgeWait;
        _status = sending == Stage::List ? Status::Listing : sending == Stage::Fetch ? Status::Receiving : Status::Purging;
        const uint32_t timeout = 60000 + d.pump->interfaceTxWaitMs(_interface, 16);
        if (!d.links->startGet(_node, operation, operation ? _journal.transientId() : nullptr,
                              RS_HANDHELD_RESOURCE_DATA_MAX - 24, *this, timeout)) {
            _stage = sending; _requestAt = now + 250;
        }
        return;
    } else return;
    if (submitted.accepted()) _storage = submitted.ticket;
    else if (submitted.rejection != Rejection::Busy) finish(Status::StorageError);
}

void RustPropagationInbox::onLinkResponse(const uint8_t* value, size_t length) {
    if (!allowed()) { finish(Status::Off); return; }
    auto& d = _engine->_d;
    const uint64_t now = d.clock->nowMs();
    if (_stage == Stage::ListWait) {
        uint32_t count = 0; uint8_t id[32];
        if (rs_handheld_lxmf_available_at(value, length, _index, id, &count) != RS_HANDHELD_OK) {
            finish(Status::Invalid); return;
        }
        if (_index >= count) {
            _index = 0; d.propagation->outcome(_node, true, now); finish(_skipped); return;
        }
        memcpy(_journal.bytes + 96, id, 32); ++_attempts;
        _stage = Stage::Fetch; _requestAt = 0; return;
    }
    if (_stage == Stage::PurgeWait) {
        uint8_t unused[32]; uint32_t count = 0;
        if (rs_handheld_lxmf_available_first(value, length, unused, &count) != RS_HANDHELD_OK || count) {
            finish(Status::Invalid); return;
        }
        d.propagation->outcome(_node, true, now); discard(Status::Complete); return;
    }
    if (_stage != Stage::FetchWait) return;
    size_t offset = 0, bytes = 0;
    if (rs_handheld_lxmf_fetched_view(value, length, &offset, &bytes) != RS_HANDHELD_OK ||
        offset > length || bytes > length - offset || bytes > RS_HANDHELD_RESOURCE_DATA_MAX) {
        finish(Status::Invalid); return;
    }
    if (!bytes) { skip(Status::Unsupported); return; }
    auto& codec = d.resources->_codec;
    memmove(codec, value + offset, bytes); // Also safe when the response occupies this scratch.
    uint8_t transient[32]; size_t plain = 0;
    if (rs_handheld_lxmf_relay_decrypt(d.ctx, codec, bytes, &plain, transient) != RS_HANDHELD_OK ||
        memcmp(transient, _journal.transientId(), 32) || plain < 32 || memcmp(codec, d.ourDestHash, 16)) {
        skip(Status::Invalid); return;
    }
    memcpy(_journal.bytes + 48, codec + 16, 16); // Cancellation binding before the held commit exists.
    RustIncomingDelivery::ReceiptSeed seed; seed.kind = RustIncomingDelivery::Kind::Propagation;
    const auto received = _engine->onDirectPayload(codec, plain, seed);
    if (received.code == RustIncomingDelivery::ReceiveCode::Pending && received.incoming.valid()) {
        _incoming = received.incoming; _stage = Stage::Commit; _status = Status::Saving;
    } else if (received.error == RustIncomingDelivery::ReceiveError::SourceUnknown ||
               received.code == RustIncomingDelivery::ReceiveCode::Backpressured) {
        if (!_sourceSince) _sourceSince = now + 1;
        if (_attempts >= 8 || (now >= _sourceSince && now - _sourceSince >= 60000)) {
            skip(received.error == RustIncomingDelivery::ReceiveError::SourceUnknown ? Status::SourceUnknown : Status::StorageError);
        } else {
            ++_attempts; _stage = Stage::Fetch; _requestAt = now + 10000;
        }
    } else skip(Status::Invalid);
}

void RustPropagationInbox::onLinkRequestFailed(RustLinkManager::RequestError error) {
    finish(error == RustLinkManager::RequestError::Unsupported ? Status::Unsupported :
           error == RustLinkManager::RequestError::Invalid ? Status::Invalid : Status::Unavailable,
           error == RustLinkManager::RequestError::Timeout || error == RustLinkManager::RequestError::LinkClosed);
}

void RustPropagationInbox::onLinkSetupFailure(const uint8_t peer[16]) {
    if (_owner && !memcmp(peer, _node, 16)) finish(Status::Unavailable, true);
}
