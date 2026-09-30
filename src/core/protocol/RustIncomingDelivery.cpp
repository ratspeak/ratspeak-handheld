#include "RustIncomingDelivery.h"
#include "RustClock.h"
#include "RustInterfacePump.h"
#include "RustLinkManager.h"
#include "runtime/TaskOwner.h"
#include <algorithm>
#include <cmath>
#include <cstring>

using namespace handheld::storage;

RustIncomingDelivery::IncomingRow* RustIncomingDelivery::row(IncomingRef ref) {
    if (!ref.valid()) return nullptr;
    auto& value = _rows[ref.slot];
    return value.phase != Phase::Free && value.generation == ref.generation ? &value : nullptr;
}
RustIncomingDelivery::ReceiptContext* RustIncomingDelivery::receipt(handheld::TxReceipt ref) {
    return const_cast<ReceiptContext*>(static_cast<const RustIncomingDelivery*>(this)->receipt(ref));
}
const RustIncomingDelivery::ReceiptContext* RustIncomingDelivery::receipt(handheld::TxReceipt ref) const {
    if (!ref.valid() || ref.slot >= ReceiptCount) return nullptr;
    const auto& value = _receipts[ref.slot];
    return value.state() != State::Free && value.generation == ref.generation ? &value : nullptr;
}

bool RustIncomingDelivery::begin(const Deps& deps, bool bindReceiptHook) {
    handheld::assertDeviceOwner();
    if (!drained() || _identityGeneration == UINT32_MAX || !deps.ctx || !deps.clock ||
        !deps.store || !deps.pump || !deps.ourDestHash) return false;
    _d = deps; _bindReceiptHook = bindReceiptHook; ++_identityGeneration; _accepting = true;
    if (_bindReceiptHook) _d.pump->setReceiptHook(receiptHook, this);
    return true;
}

bool RustIncomingDelivery::captureSeed(ReceiptSeed& seed, Kind kind, uint8_t iface,
                                      const uint8_t* raw, size_t length, const uint8_t* linkId) {
    if (!_accepting || !_d.pump || !_d.clock || length > 128 || (length && !raw)) return false;
    seed = {}; seed.kind = kind; seed.raw = raw; seed.length = length;
    seed.interfaceId = iface; seed.interfaceGeneration = _d.pump->interfaceGeneration(iface);
    seed.bornMs = _d.clock->nowMs();
    if (!seed.interfaceGeneration) return false;
    if (linkId && (!_d.links || !_d.links->receiptBinding(iface, linkId, seed.linkSlot, seed.linkGeneration))) return false;
    return true;
}

RustIncomingDelivery::IncomingRef RustIncomingDelivery::allocateRow(const MessageView& message) {
    for (uint8_t index = 0; index < RowCount; ++index) {
        auto& value = _rows[index];
        if (value.phase != Phase::Free || value.generation == UINT32_MAX) continue;
        const uint32_t generation = value.generation + 1;
        value = {}; value.generation = generation; value.phase = Phase::Reserved;
        value.identityGeneration = _identityGeneration; value.visibilityGeneration = _visibilityGeneration;
        memcpy(value.messageId, message.messageId, 32); memcpy(value.peer, message.source, 16);
        return {generation, index};
    }
    return {};
}

handheld::TxReceipt RustIncomingDelivery::allocateReceipt(const ReceiptSeed& seed, IncomingRef incoming) {
    auto* owner = row(incoming);
    if (owner && __builtin_popcount(owner->proofMask & ProofBits) >= ReceiptsPerRow) return {};
    if (seed.length > 128 || (seed.length && !seed.raw) || !seed.interfaceGeneration ||
        !seed.maxWaitMs || (seed.maxWaitMs > MaxWaitMs && seed.kind != Kind::ReservedResource)) return {};
    if (seed.kind == Kind::Control) {
        size_t controls = 0;
        for (const auto& context : _receipts)
            if (context.state() != State::Free && context.kind() == Kind::Control) ++controls;
        if (controls >= 4 || seed.linkSlot == NoLink) return {};
    }
    for (uint8_t index = 0; index < ReceiptCount; ++index) {
        auto& value = _receipts[index];
        if (value.state() != State::Free || value.generation == UINT32_MAX) continue;
        const uint32_t generation = value.generation + 1;
        value = {}; value.generation = generation;
        if (seed.length) memcpy(value.raw, seed.raw, seed.length);
        value.rawLength = uint8_t(seed.length); value.bornMs = seed.bornMs; value.maxWaitMs = seed.maxWaitMs;
        value.interfaceId = seed.interfaceId; value.interfaceGeneration = seed.interfaceGeneration;
        value.linkGeneration = seed.linkGeneration; value.rowIndex = owner ? incoming.slot : NoRow;
        value.rowGeneration = owner ? incoming.generation : _identityGeneration;
        value.set(seed.kind, State::Reserved, seed.linkSlot);
        if (owner) owner->proofMask |= uint16_t(1u << index);
        return {generation, index};
    }
    return {};
}

bool RustIncomingDelivery::bindingLive(const ReceiptContext& value) const {
    if (!_accepting || !_d.clock || !_d.pump || !value.maxWaitMs) return false;
    const uint64_t now = _d.clock->nowMs();
    if (now < value.bornMs || now - value.bornMs >= value.maxWaitMs ||
        _d.pump->interfaceGeneration(value.interfaceId) != value.interfaceGeneration) return false;
    return value.linkSlot() == NoLink || (_d.links &&
        _d.links->receiptLive(value.linkSlot(), value.linkGeneration, value.interfaceId));
}

bool RustIncomingDelivery::receiptLive(handheld::TxReceipt ref) const {
    const auto* value = receipt(ref);
    if (!value || (value->state() != State::Eligible && value->state() != State::Queued) || !bindingLive(*value)) return false;
    if (value->rowIndex == NoRow)
        return value->kind() == Kind::Control && value->rowGeneration == _identityGeneration;
    if (value->rowIndex >= RowCount) return false;
    const auto& owner = _rows[value->rowIndex];
    return owner.generation == value->rowGeneration && owner.identityGeneration == _identityGeneration &&
        (owner.phase == Phase::Committed || owner.phase == Phase::Reaction);
}

void RustIncomingDelivery::releaseRow(IncomingRef ref) {
    auto* value = row(ref);
    if (!value || value->ticketSequence || value->proofMask) return;
    const uint32_t generation = value->generation;
    *value = {}; value->generation = generation;
}
void RustIncomingDelivery::retireReceipt(handheld::TxReceipt ref) {
    auto* value = receipt(ref); if (!value) return;
    const IncomingRef owner{value->rowGeneration, value->rowIndex};
    if (auto* incoming = row(owner)) incoming->proofMask &= uint16_t(~(1u << ref.slot));
    const uint32_t generation = value->generation;
    *value = {}; value->generation = generation;
    releaseRow(owner);
}

RustIncomingDelivery::ReceiveResult RustIncomingDelivery::accept(const MessageView& message,
                                                                const ReceiptSeed& seed) {
    handheld::assertDeviceOwner();
    if (!_accepting) return {ReceiveCode::Rejected, {}, ReceiveError::Stopped};
    const bool held = seed.kind == Kind::Propagation;
    if (!message.messageId || !message.source || !std::isfinite(message.timestamp) ||
        !Budget::validBody(message.titleLength, message.contentLength) ||
        (message.titleLength && !message.title) || (message.contentLength && !message.content) ||
        (seed.kind != Kind::NoProof && !held && (!seed.raw || !seed.length || seed.length > 128)) ||
        (held && message.reaction))
        return {ReceiveCode::Rejected, {}, ReceiveError::Invalid};
    IncomingRef ref;
    for (uint8_t index = 0; index < RowCount; ++index) {
        const auto& value = _rows[index];
        if (value.phase != Phase::Free && value.phase != Phase::Invisible &&
            value.identityGeneration == _identityGeneration &&
            !memcmp(value.peer, message.source, 16) && !memcmp(value.messageId, message.messageId, 32)) {
            ref = {value.generation, index}; break;
        }
    }
    const bool existing = ref.valid();
    if (!existing) ref = allocateRow(message);
    auto* incoming = row(ref);
    if (!incoming) return {ReceiveCode::Backpressured, {}, ReceiveError::Capacity};
    handheld::TxReceipt proof;
    if (seed.kind != Kind::NoProof && !held) {
        // Identical live receipts coalesce without a fourth context or a new clock.
        for (uint8_t index = 0; index < ReceiptCount; ++index) {
            const auto& value = _receipts[index];
            if (!(incoming->proofMask & (1u << index)) || value.kind() != seed.kind ||
                value.interfaceId != seed.interfaceId || value.interfaceGeneration != seed.interfaceGeneration ||
                value.linkGeneration != seed.linkGeneration || value.linkSlot() != seed.linkSlot ||
                value.rawLength != seed.length || memcmp(value.raw, seed.raw, seed.length)) continue;
            proof = {value.generation, index}; break;
        }
        if (!proof.valid() && seed.reserved.valid()) {
            auto* reserved = receipt(seed.reserved);
            if (reserved && reserved->kind() == Kind::PreparedResource && bindingLive(*reserved) &&
                __builtin_popcount(incoming->proofMask & ProofBits) < ReceiptsPerRow && seed.kind == Kind::Resource &&
                seed.length >= RS_HANDHELD_RESOURCE_PROOF_LEN && seed.length <= sizeof(reserved->raw) &&
                seed.interfaceId == reserved->interfaceId && seed.interfaceGeneration == reserved->interfaceGeneration &&
                seed.linkSlot == reserved->linkSlot() && seed.linkGeneration == reserved->linkGeneration &&
                !memcmp(reserved->raw + 16, seed.raw + seed.length - RS_HANDHELD_RESOURCE_PROOF_LEN, 32)) {
                memcpy(reserved->raw, seed.raw, seed.length); reserved->rawLength = uint8_t(seed.length);
                reserved->rowIndex = ref.slot; reserved->rowGeneration = ref.generation;
                reserved->set(Kind::Resource, State::Reserved, seed.linkSlot);
                incoming->proofMask |= uint16_t(1u << seed.reserved.slot); proof = seed.reserved;
            }
        }
        if (!proof.valid() && !seed.reserved.valid()) proof = allocateReceipt(seed, ref);
        if (proof.valid() && seed.reserved.valid() &&
            (proof.slot != seed.reserved.slot || proof.generation != seed.reserved.generation)) releaseReservation(seed.reserved);
        if (!proof.valid()) {
            if (!existing) releaseRow(ref);
            return {ReceiveCode::Backpressured, {}, ReceiveError::Capacity};
        }
        auto* context = receipt(proof);
        if (!context || !bindingLive(*context)) {
            retireReceipt(proof); if (!existing) releaseRow(ref);
            return {ReceiveCode::Rejected, {}, ReceiveError::Invalid};
        }
    }
    if (existing) {
        if (held) incoming->proofMask |= HeldCommit;
        if (auto* context = receipt(proof))
            if (context->state() == State::Reserved &&
                (incoming->phase == Phase::Committed || incoming->phase == Phase::Reaction)) context->state(State::Eligible);
        return {message.reaction ? ReceiveCode::HandledNonpersistent : ReceiveCode::Pending, message.reaction ? IncomingRef{} : ref, ReceiveError::None};
    }
    if (message.reaction) {
        incoming->phase = Phase::Reaction;
        if (auto* context = receipt(proof)) context->state(State::Eligible);
        releaseRow(ref);
        return {ReceiveCode::HandledNonpersistent, {}, ReceiveError::None};
    }
    Request request; request.operation = Operation::CreateIncoming; request.key.incoming = true;
    memcpy(request.key.peer, message.source, 16); memcpy(request.source, message.source, 16);
    memcpy(request.destination, _d.ourDestHash, 16); memcpy(request.messageId, message.messageId, 32);
    request.hasMessageId = true; request.timestamp = message.timestamp; request.status = uint8_t(LXMFStatus::DELIVERED);
    request.titleLength = uint16_t(message.titleLength); request.contentLength = uint16_t(message.contentLength);
    request.identityGeneration = incoming->identityGeneration; request.peerGeneration = incoming->visibilityGeneration;
    const auto admitted = _d.store->requestSave(request, message.title, message.content);
    if (!admitted.accepted()) {
        retireReceipt(proof); releaseRow(ref);
        const bool retry = admitted.rejection == Rejection::Busy || admitted.rejection == Rejection::NoMemory;
        return {retry ? ReceiveCode::Backpressured : ReceiveCode::Rejected, {}, ReceiveError::Storage};
    }
    incoming = row(ref);
    incoming->ticketSequence = admitted.ticket.sequence; incoming->ticketSlot = admitted.ticket.slot;
    if (held) incoming->proofMask |= HeldCommit;
    incoming->phase = Phase::AwaitStore;
    return {ReceiveCode::Pending, ref, ReceiveError::None};
}

bool RustIncomingDelivery::receiptHook(void* owner, handheld::TxReceipt ref, handheld::TxReceiptEvent event) {
    auto& incoming = *static_cast<RustIncomingDelivery*>(owner);
    if (event == handheld::TxReceiptEvent::Validate) return incoming.receiptLive(ref);
    incoming.retireReceipt(ref); return true;
}

void RustIncomingDelivery::drainReceipt(handheld::TxReceipt ref) {
    auto* value = receipt(ref); if (!value || value->state() != State::Eligible) return;
    if (!receiptLive(ref)) { retireReceipt(ref); return; }
    handheld::TxLease lease;
    if (!_d.pump->captureLeaseAt(value->interfaceId, value->raw, value->rawLength,
                                value->bornMs, value->maxWaitMs, lease)) { retireReceipt(ref); return; }
    lease.setReceipt(ref);
    const auto offered = _d.pump->offerReceipt(value->raw, value->rawLength, lease);
    value = receipt(ref); // the disposition hook can run inline and retire this generation
    if (!value) return;
    if (offered == handheld::TxOffer::Queued) value->state(State::Queued);
    else if (offered == handheld::TxOffer::Started || offered == handheld::TxOffer::Rejected) retireReceipt(ref);
}

void RustIncomingDelivery::poll() {
    handheld::assertDeviceOwner();
    if (_polling || !_d.store) return;
    _polling = true; _d.store->poll();
    for (uint8_t index = 0; index < RowCount; ++index) {
        auto& value = _rows[index]; if (!value.ticketSequence) continue;
        const IncomingRef ref{value.generation, index};
        const Ticket ticket{value.ticketSequence, value.ticketSlot};
        Result result;
        if (!_d.store->peekResult(ticket, result)) continue;
        const bool visible = _accepting && value.phase != Phase::Invisible && value.identityGeneration == _identityGeneration;
        if (result.outcome == Outcome::Committed && visible) {
            value.committedCounter = result.key.counter; value.commitRevision = result.revision; value.phase = Phase::Committed;
            // Cache insertion is only a performance hint; a later cache hit still
            // requires this row or an ordered retained-history transaction.
            rs_handheld_rns_seed_seen_message(_d.ctx, value.messageId);
            if (!result.duplicate && _d.onMessage && *_d.onMessage) {
                const LXMFManager::CommittedMessage notice{result.key, result.revision};
                try { (*_d.onMessage)(notice); }
                catch (...) { Serial.println("[RUST-LXMF] committed-message notification failed; record retained"); }
            }
        } else {
            value.phase = Phase::Invisible;
            // Failed rows have no commit revision. Reuse that scalar for the
            // retained failure, keeping the established 80-byte row budget.
            value.commitRevision = uint32_t(result.error == Error::None ? Error::Cancelled : result.error);
        }
        auto* current = row(ref);
        if (current && current->phase == Phase::Committed) {
            for (uint8_t proof = 0; proof < ReceiptCount; ++proof)
                if (current->proofMask & (1u << proof)) _receipts[proof].state(State::Eligible);
        } else if (current) {
            for (uint8_t proof = 0; proof < ReceiptCount; ++proof)
                if (current->proofMask & (1u << proof)) retireReceipt({_receipts[proof].generation, proof});
        }
        _d.store->releaseResult(ticket);
        if ((current = row(ref))) { current->ticketSequence = 0; current->ticketSlot = UINT8_MAX; }
        releaseRow(ref);
    }
    for (uint8_t index = 0; index < ReceiptCount; ++index) {
        const handheld::TxReceipt ref{_receipts[index].generation, index};
        const auto* value = receipt(ref); if (!value) continue;
        if (!bindingLive(*value)) { retireReceipt(ref); continue; }
        drainReceipt(ref);
    }
    _polling = false;
}

handheld::TxReceipt RustIncomingDelivery::reserveResource(uint8_t iface, const uint8_t linkId[16], const uint8_t hash[32]) {
    uint8_t binding[48]; memcpy(binding, linkId, 16); memcpy(binding + 16, hash, 32);
    ReceiptSeed seed;
    if (!captureSeed(seed, Kind::ReservedResource, iface, binding, sizeof(binding), linkId)) return {};
    seed.maxWaitMs = UINT32_MAX; // refined to the advertised operation deadline before parts are accepted
    return allocateReceipt(seed, {});
}
bool RustIncomingDelivery::sameResource(const ReceiptContext& value, uint8_t iface,
                                       const uint8_t linkId[16], const uint8_t hash[32]) const {
    if ((value.kind() != Kind::Resource && value.kind() != Kind::ReservedResource && value.kind() != Kind::PreparedResource) ||
        value.state() == State::Free || value.interfaceId != iface || !bindingLive(value)) return false;
    uint8_t slot; uint32_t generation;
    if (!_d.links || !_d.links->receiptBinding(iface, linkId, slot, generation) ||
        value.linkSlot() != slot || value.linkGeneration != generation) return false;
    if (value.kind() != Kind::Resource) return !memcmp(value.raw, linkId, 16) && !memcmp(value.raw + 16, hash, 32);
    return value.rawLength >= RS_HANDHELD_RESOURCE_PROOF_LEN &&
        !memcmp(value.raw + value.rawLength - RS_HANDHELD_RESOURCE_PROOF_LEN, hash, 32);
}
bool RustIncomingDelivery::resourcePending(uint8_t iface, const uint8_t linkId[16], const uint8_t hash[32]) const {
    for (const auto& value : _receipts) if (sameResource(value, iface, linkId, hash)) return true;
    return false;
}
bool RustIncomingDelivery::resourceSeed(handheld::TxReceipt ref, ReceiptSeed& seed, const uint8_t* raw, size_t length) {
    auto* value = receipt(ref);
    if (!value || (value->kind() != Kind::ReservedResource && value->kind() != Kind::PreparedResource) ||
        !bindingLive(*value) || !raw || length > 128) return false;
    if (value->kind() == Kind::ReservedResource) {
        const uint64_t now = _d.clock->nowMs();
        const uint32_t remaining = value->maxWaitMs - uint32_t(now - value->bornMs);
        value->bornMs = now; value->maxWaitMs = std::min(MaxWaitMs, remaining);
        value->set(Kind::PreparedResource, State::Reserved, value->linkSlot());
    }
    seed = {}; seed.kind = Kind::Resource; seed.reserved = ref; seed.raw = raw; seed.length = length;
    seed.bornMs = value->bornMs; seed.maxWaitMs = value->maxWaitMs;
    seed.interfaceId = value->interfaceId; seed.interfaceGeneration = value->interfaceGeneration;
    seed.linkSlot = value->linkSlot(); seed.linkGeneration = value->linkGeneration;
    return true;
}
bool RustIncomingDelivery::resourceLive(handheld::TxReceipt ref) const {
    const auto* value = receipt(ref);
    return value && (value->kind() == Kind::ReservedResource || value->kind() == Kind::PreparedResource ||
        value->kind() == Kind::Resource) && bindingLive(*value);
}
void RustIncomingDelivery::setResourceDeadline(handheld::TxReceipt ref, uint32_t waitMs) {
    auto* value = receipt(ref);
    if (value && value->kind() == Kind::ReservedResource) value->maxWaitMs = std::min(value->maxWaitMs, waitMs);
}
void RustIncomingDelivery::cancelResource(uint8_t iface, const uint8_t linkId[16], const uint8_t hash[32]) {
    for (uint8_t index = 0; index < ReceiptCount; ++index)
        if (sameResource(_receipts[index], iface, linkId, hash)) retireReceipt({_receipts[index].generation, index});
    // An admitted valid message may still commit/notify. This revokes only the
    // exact transfer's proof permission and never erases committed history.
}
void RustIncomingDelivery::releaseReservation(handheld::TxReceipt ref) { retireReceipt(ref); }
bool RustIncomingDelivery::retainControl(uint8_t iface, const uint8_t linkId[16], const uint8_t* raw,
                                         size_t length, handheld::TxReceipt reservation) {
    ReceiptSeed seed;
    if (!captureSeed(seed, Kind::Control, iface, raw, length, linkId)) return false;
    // A live assembly retains its reservation until terminal control admission
    // succeeds. Do not lose that credit while the unrelated control quota is full.
    if (auto* reserved = receipt(reservation)) {
        if (reserved->kind() != Kind::ReservedResource && reserved->kind() != Kind::PreparedResource) return false;
        size_t controls = 0;
        for (const auto& context : _receipts)
            if (context.state() != State::Free && context.kind() == Kind::Control) ++controls;
        if (controls >= 4) return false;
        memcpy(reserved->raw, raw, length); reserved->rawLength = uint8_t(length);
        reserved->bornMs = seed.bornMs; reserved->maxWaitMs = seed.maxWaitMs;
        reserved->rowGeneration = _identityGeneration;
        reserved->set(Kind::Control, State::Eligible, seed.linkSlot);
        return true;
    }
    const auto ref = allocateReceipt(seed, {}); auto* value = receipt(ref);
    if (!value) return false;
    value->state(State::Eligible); return true;
}

void RustIncomingDelivery::dropPeer(const uint8_t peer[16]) {
    handheld::assertDeviceOwner();
    if (_visibilityGeneration == UINT32_MAX) { stopAdmissions(); return; }
    ++_visibilityGeneration;
    for (uint8_t index = 0; index < RowCount; ++index) {
        auto& value = _rows[index];
        if (value.phase == Phase::Free || memcmp(value.peer, peer, 16)) continue;
        value.phase = Phase::Invisible;
        value.proofMask &= ~HeldCommit;
        if (value.ticketSequence) _d.store->cancel({value.ticketSequence, value.ticketSlot});
        for (uint8_t proof = 0; proof < ReceiptCount; ++proof)
            if (value.proofMask & (1u << proof)) retireReceipt({_receipts[proof].generation, proof});
        releaseRow({value.generation, index});
    }
    for (uint8_t index = 0; index < ReceiptCount; ++index) {
        const auto& value = _receipts[index];
        if (value.state() == State::Free || value.rowIndex != NoRow || value.linkSlot() == NoLink) continue;
        // An assembling Resource has no validated LXMF source yet, even when
        // our initiator Link authenticates its remote delivery destination.
        const bool sourceUnknown = value.kind() == Kind::ReservedResource || value.kind() == Kind::PreparedResource;
        const auto* boundPeer = _d.links ? _d.links->receiptPeer(value.linkSlot(), value.linkGeneration) : nullptr;
        if (sourceUnknown || !boundPeer || !memcmp(boundPeer, peer, 16)) retireReceipt({value.generation, index});
    }
}
void RustIncomingDelivery::dropLink(uint8_t slot, uint32_t generation) {
    for (uint8_t index = 0; index < ReceiptCount; ++index) {
        const auto& value = _receipts[index];
        if (value.state() != State::Free && value.linkSlot() == slot && value.linkGeneration == generation)
            retireReceipt({value.generation, index});
    }
}
void RustIncomingDelivery::stopAdmissions() {
    handheld::assertDeviceOwner(); _accepting = false;
    for (auto& value : _rows) {
        if (value.phase == Phase::Free) continue;
        value.phase = Phase::Invisible;
        value.proofMask &= ~HeldCommit;
        if (value.ticketSequence && _d.store) _d.store->cancel({value.ticketSequence, value.ticketSlot});
    }
    for (uint8_t index = 0; index < ReceiptCount; ++index) retireReceipt({_receipts[index].generation, index});
    for (uint8_t index = 0; index < RowCount; ++index) releaseRow({_rows[index].generation, index});
}
size_t RustIncomingDelivery::pendingRows() const {
    size_t count = 0; for (const auto& value : _rows) if (value.phase != Phase::Free) ++count; return count;
}
size_t RustIncomingDelivery::retainedReceipts() const {
    size_t count = 0; for (const auto& value : _receipts) if (value.state() != State::Free) ++count; return count;
}
bool RustIncomingDelivery::drained() const { return !pendingRows() && !retainedReceipts(); }
void RustIncomingDelivery::detach() {
    handheld::assertDeviceOwner(); configASSERT(!_accepting && drained());
    if (_d.pump && _bindReceiptHook) _d.pump->setReceiptHook(nullptr, nullptr);
    _d = {};
}

RustIncomingDelivery::CommitState RustIncomingDelivery::heldCommit(IncomingRef ref, CommitResult& out) const {
    handheld::assertDeviceOwner();
    if (!_accepting || !ref.valid()) return CommitState::Invalid;
    const auto& value = _rows[ref.slot];
    if (value.generation != ref.generation || value.phase == Phase::Free ||
        value.identityGeneration != _identityGeneration || !(value.proofMask & HeldCommit)) return CommitState::Invalid;
    if (value.ticketSequence || value.phase == Phase::Reserved || value.phase == Phase::AwaitStore) return CommitState::Pending;
    out = {};
    if (value.phase != Phase::Committed || !value.committedCounter || !value.commitRevision) {
        out.error = Error(value.commitRevision); return CommitState::Failed;
    }
    memcpy(out.key.peer, value.peer, 16); out.key.counter = value.committedCounter; out.key.incoming = true;
    memcpy(out.messageId, value.messageId, 32); out.revision = value.commitRevision;
    return CommitState::Committed;
}

bool RustIncomingDelivery::releaseHeld(IncomingRef ref) {
    handheld::assertDeviceOwner();
    auto* value = row(ref);
    if (!value || !(value->proofMask & HeldCommit)) return false;
    value->proofMask &= ~HeldCommit;
    releaseRow(ref);
    return true;
}
