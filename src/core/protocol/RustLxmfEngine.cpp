
#include "protocol/RustLxmfEngine.h"
#include "protocol/RustClock.h"
#include "protocol/RustEntropy.h"
#include "protocol/RustKeyMap.h"
#include "protocol/RustInterfacePump.h"
#include "protocol/RustWire.h"
#include "protocol/RustLinkManager.h"
#include "protocol/RustResourceEngine.h"
#include "storage/MessageStore.h"
#include "storage/PreparedEnvelope.h"
#include <Arduino.h>
#include <algorithm>

namespace {
constexpr unsigned long DISCOVERY_RETRY_MS = 10000;
constexpr unsigned long TX_RETRY_MS = 1000; // Backpressure must not spin ECIES at the 100 Hz owner cadence.
constexpr int DISCOVERY_MAX_ATTEMPTS = 7;   // immediate + six 10s retries ~= 60s
// Proof-wait before requeue. Shorter than the donor's 60s (closer to upstream LXMF's 10s
// DELIVERY_RETRY_WAIT), sized for the worst-case LoRa link RTT (~5-8s) with margin. Plus a
// per-pending random jitter so messages lost together (a burst) don't retry in lockstep and
// re-collide on the half-duplex link. Both are local timing (wire-neutral).
constexpr unsigned long PROOF_TIMEOUT_MS = 20000;
constexpr unsigned long PROOF_JITTER_MAX_MS = 8000;
constexpr unsigned long LATE_PROOF_GRACE_MS = 120000;
constexpr unsigned long LINK_WAIT_TIMEOUT_MS = 60000;  // six discovery retry intervals

unsigned long proofJitterMs() {
    uint32_t r = 0;
    RustEntropy::fill((uint8_t*)&r, sizeof(r));
    return r % PROOF_JITTER_MAX_MS;
}

std::string hex(const uint8_t* d, size_t n) {
    static const char* H = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) {
        s.push_back(H[d[i] >> 4]);
        s.push_back(H[d[i] & 0xF]);
    }
    return s;
}

void secureZero(uint8_t* data, size_t len) {
    volatile uint8_t* p = data;
    while (len--) *p++ = 0;
}
bool sendableBody(size_t titleLength, size_t contentLength) {
    size_t packed = 0;
    return handheld::storage::Budget::validBody(titleLength, contentLength) &&
        rs_handheld_lxmf_packed_size(titleLength, contentLength, &packed) == RS_HANDHELD_OK &&
        packed <= RS_HANDHELD_RESOURCE_DATA_MAX;
}
}  // namespace

using handheld::outgoing::Poll;
using handheld::outgoing::Rejection;
using handheld::storage::Error;
using handheld::storage::Operation;
using handheld::storage::Outcome;

bool RustLxmfEngine::begin(const Deps& deps) {
    if (!drained() || _identityGeneration == UINT32_MAX || !deps.ctx || !deps.clock ||
        !deps.store || !deps.pump || !deps.ourDestHash) return false;
    _d = deps;
    ++_identityGeneration;
    _accepting = true; _recovering = true; _recoveryCursor = {};
    RustIncomingDelivery::Deps incoming;
    incoming.ctx = deps.ctx; incoming.clock = deps.clock; incoming.store = deps.store;
    incoming.pump = deps.pump; incoming.links = deps.links; incoming.onMessage = deps.onMessage;
    incoming.ourDestHash = deps.ourDestHash;
    if (!_incoming.begin(incoming, false)) { _accepting = false; return false; }
    _inbox.begin(*this);
    _d.pump->setReceiptHook(receiptHook, this);
    return true;
}

RustLxmfEngine::OutgoingRow* RustLxmfEngine::row(Ticket ticket) {
    return ticket.valid() && _rows[ticket.slot].generation == ticket.generation &&
        _rows[ticket.slot].phase != Phase::Free ? &_rows[ticket.slot] : nullptr;
}
const RustLxmfEngine::OutgoingRow* RustLxmfEngine::row(Ticket ticket) const {
    return ticket.valid() && _rows[ticket.slot].generation == ticket.generation &&
        _rows[ticket.slot].phase != Phase::Free ? &_rows[ticket.slot] : nullptr;
}
handheld::storage::RecordKey RustLxmfEngine::key(const OutgoingRow& value) const {
    handheld::storage::RecordKey result;
    memcpy(result.peer, value.peer, 16); result.counter = value.counter;
    return result;
}
handheld::storage::Ticket RustLxmfEngine::storageTicket(const OutgoingRow& value) const {
    return {value.storageSequence, value.storageSlot};
}
void RustLxmfEngine::hold(OutgoingRow& value, handheld::storage::Submission admission, Operation operation) {
    value.storageSequence = admission.ticket.sequence; value.storageSlot = admission.ticket.slot;
    value.storageOperation = operation;
}
RustLxmfEngine::Ticket RustLxmfEngine::allocate() {
    for (uint8_t i = 0; i < RowCount; ++i) {
        auto& value = _rows[i];
        if (value.phase != Phase::Free || value.generation == UINT32_MAX) continue;
        const uint32_t generation = value.generation + 1;
        value = {}; value.generation = generation; value.identityGeneration = _identityGeneration;
        value.phase = Phase::Saving;
        return {generation, i};
    }
    Ticket oldest;
    for (uint8_t i = 0; i < RowCount; ++i) {
        const auto& value = _rows[i];
        if (value.phase == Phase::Grace && value.generation < UINT32_MAX && (value.flags & Acknowledged) &&
            !value.storageSequence && !value.queuedReceipts && value.desired == value.durable &&
            (!oldest.valid() || value.receiptSince < _rows[oldest.slot].receiptSince)) oldest = {value.generation, i};
    }
    if (oldest.valid()) { retire(oldest); return allocate(); }
    return {};
}
void RustLxmfEngine::releaseBody(Ticket ticket) {
    if (_body.slot == ticket.slot && _body.generation == ticket.generation) {
        _body.slot = UINT8_MAX; _body.generation = 0; _body.length = 0; _body.targets = 0;
    }
}
void RustLxmfEngine::retire(Ticket ticket) {
    auto* value = row(ticket);
    if (!value || value->storageSequence || value->queuedReceipts) return;
    releaseBody(ticket);
    if (_relay.ticket == ticket) resetRelay();
    const uint32_t generation = value->generation;
    *value = {}; value->generation = generation;
    ++_statusRevision;
}

RustLxmfEngine::Submission RustLxmfEngine::submit(const uint8_t dest[16], const uint8_t* title,
    size_t titleLength, const uint8_t* content, size_t contentLength, bool preferLink) {
    if (!_accepting) return {{}, Rejection::Stopped};
    if (_recovering) return {{}, Rejection::Recovering};
    if (!dest || (titleLength && !title) || (contentLength && !content)) return {{}, Rejection::Invalid};
    if (_deleting && !memcmp(_deletingPeer, dest, 16)) return {{}, Rejection::Fenced};
    if (!sendableBody(titleLength, contentLength)) return {{}, Rejection::TooLarge};
    const Ticket ticket = allocate();
    auto* value = row(ticket);
    if (!value) {
        bool exhausted = true;
        for (const auto& candidate : _rows) exhausted &= candidate.generation == UINT32_MAX;
        return {{}, exhausted ? Rejection::Exhausted : Rejection::Busy};
    }
    memcpy(value->peer, dest, 16);
    const uint64_t epoch = RustClock::epochSecs();
    value->timestamp = epoch ? double(epoch) : double(_d.clock->nowMs()) / 1000.0;
    value->flags = preferLink ? PreferLink : 0;
    handheld::storage::Request request;
    request.operation = Operation::CreateOutgoing;
    memcpy(request.key.peer, dest, 16); memcpy(request.source, _d.ourDestHash, 16);
    memcpy(request.destination, dest, 16); request.timestamp = value->timestamp;
    request.identityGeneration = _identityGeneration;
    request.titleLength = titleLength; request.contentLength = contentLength;
    if (_d.propagation && _d.propagation->settings().enabled) {
        const bool always = _d.propagation->settings().delivery == handheld::propagation::Delivery::Always;
        request.deliveryPolicy = always ? handheld::messaging::DeliveryPolicy::Always : handheld::messaging::DeliveryPolicy::Auto;
        value->flags |= always ? PolicyAlways | RelayRequired : PolicyAuto;
        if (always) value->desired = value->durable = LXMFStatus::PROP_QUEUED;
    }
    request.status = uint8_t(value->desired);
    const auto admission = _d.store->requestSave(request, title, content);
    if (!admission.accepted()) {
        retire(ticket);
        return {{}, static_cast<Rejection>(admission.rejection)};
    }
    hold(*value, admission, Operation::CreateOutgoing);
    value->initial = 1;
    ++_statusRevision;
    return {ticket, Rejection::None};
}
Poll RustLxmfEngine::poll(Ticket ticket, InitialResult& result) const {
    const auto* value = row(ticket);
    if (!value || (value->flags & Acknowledged) || !value->initial) return Poll::Invalid;
    if (value->initial == 1) return Poll::Pending;
    result.outcome = value->initial == 2 ? Outcome::Committed :
        value->initial == 4 ? Outcome::Cancelled : Outcome::Failed;
    result.error = value->initialError; result.key = key(*value);
    result.revision = value->initial == 2 ? 1 : 0;
    result.txSuppressed = value->flags & InitialSuppressed;
    return Poll::Ready;
}
bool RustLxmfEngine::acknowledge(Ticket ticket) {
    auto* value = row(ticket);
    if (!value || value->initial < 2 || (value->flags & Acknowledged)) return false;
    value->flags |= Acknowledged;
    return true;
}
bool RustLxmfEngine::status(const handheld::storage::RecordKey& record, StatusView& result) const {
    if (!record.counter || record.incoming) return false;
    for (const auto& value : _rows) {
        if (value.phase == Phase::Free || value.counter != record.counter || memcmp(value.peer, record.peer, 16)) continue;
        result.desired = value.desired; result.durable = value.durable; result.error = value.error;
        result.pending = value.desired != value.durable ||
            (value.storageSequence && value.storageOperation == Operation::UpdateStatus);
        result.txSuppressed = value.flags & Suppressed;
        return true;
    }
    return false;
}
int RustLxmfEngine::queuedCount() const {
    int count = 0;
    for (const auto& value : _rows)
        if (value.phase != Phase::Free && value.phase != Phase::Settled && value.phase != Phase::Grace) ++count;
    return count;
}
void RustLxmfEngine::setStatus(Ticket ticket, LXMFStatus status) {
    auto* value = row(ticket);
    if (!value || (value->flags & Deleted) || value->desired == LXMFStatus::DELIVERED || value->desired == status) return;
    value->desired = status; value->statusRetry = 0; value->flags |= Notify; ++_statusRevision;
    if (_relay.ticket == ticket && _relay.direct &&
        (handheld::messaging::failedStatus(uint8_t(status)) || status == LXMFStatus::UNCONFIRMED)) {
        if (value->proofCount && !(value->flags & ProofGrace)) {
            value->flags |= ProofGrace; value->receiptSince = _d.clock->nowMs();
            value->phase = Phase::Grace;
        }
        resetRelay();
    }
    if (status == LXMFStatus::DELIVERED) {
        value->receiptMask = 0; value->phase = Phase::Settled; releaseBody(ticket);
        if (_d.resources) _d.resources->cancelSend(ticket);
        if (_relay.ticket == ticket) resetRelay();
    }
}
bool RustLxmfEngine::cancel(Ticket ticket) {
    auto* value = row(ticket);
    if (!value) return false;
    value->flags |= Suppressed; value->receiptMask = 0;
    releaseBody(ticket);
    if (value->storageSequence && value->storageOperation != Operation::UpdateStatus)
        _d.store->cancel(storageTicket(*value));
    if (!value->storageSequence || (value->phase != Phase::Saving && value->phase != Phase::Query))
        value->phase = Phase::Settled;
    if (_d.resources) _d.resources->cancelSend(ticket);
    if (_relay.ticket == ticket) resetRelay();
    ++_statusRevision;
    return true;
}
bool RustLxmfEngine::beginPeerDelete(const uint8_t peer[16]) {
    if (!_accepting || _deleting || !peer) return false;
    _deleting = true; memcpy(_deletingPeer, peer, 16);
    _inbox.dropPeer(peer);
    _incoming.dropPeer(peer);
    for (uint8_t i = 0; i < RowCount; ++i) {
        auto& value = _rows[i];
        if (value.phase == Phase::Free || memcmp(value.peer, peer, 16)) continue;
        value.flags |= DeletePending;
        cancel({value.generation, i});
    }
    if (_d.resources) _d.resources->dropPeer(peer);
    return true;
}
void RustLxmfEngine::finishPeerDelete(const uint8_t peer[16], const handheld::storage::Result& result) {
    if (!_deleting || memcmp(_deletingPeer, peer, 16)) return;
    _deleting = false;
    for (uint8_t i = 0; i < RowCount; ++i) {
        auto& value = _rows[i];
        if (value.phase == Phase::Free || !(value.flags & DeletePending) || memcmp(value.peer, peer, 16)) continue;
        value.flags &= ~DeletePending;
        if (result.outcome == Outcome::Committed && !memcmp(result.key.peer, peer, 16) &&
            (!value.counter || value.counter <= result.key.counter)) {
            value.flags |= Deleted;
            value.error = Error::None;
        }
        ++_statusRevision;
    }
}
void RustLxmfEngine::stopAdmissions() {
    _accepting = false; _recovering = false;
    _inbox.stop();
    for (uint8_t i = 0; i < RowCount; ++i) {
        auto& value = _rows[i];
        if (value.phase == Phase::Free) continue;
        // A saved interrupted attempt remains restartable by the existing
        // QUEUED/SENDING recovery policy; a verified proof remains terminal.
        if (value.desired == LXMFStatus::SENT) setStatus({value.generation, i}, LXMFStatus::QUEUED);
        cancel({value.generation, i});
    }
}
bool RustLxmfEngine::drained() const {
    if (_deleting || !_inbox.drained()) return false;
    for (const auto& value : _rows) if (value.phase != Phase::Free) return false;
    return _body.slot == UINT8_MAX;
}
Error RustLxmfEngine::drainError() const {
    for (const auto& value : _rows)
        if (value.phase != Phase::Free && value.error != Error::None &&
            value.desired != value.durable && !(value.flags & Deleted)) return value.error;
    return Error::None;
}

void RustLxmfEngine::settleStorage(Ticket ticket) {
    auto* value = row(ticket);
    if (!value || !value->storageSequence) return;
    if (settleRelayStorage(ticket)) return;
    handheld::storage::Result result;
    const auto held = storageTicket(*value);
    if (!_d.store->peekResult(held, result)) return;
    const Operation operation = value->storageOperation;
    handheld::storage::StoredRecordHeader header;
    bool validHeader = false, foreignIdentity = false;
    if (result.outcome == Outcome::Committed &&
        (operation == Operation::ReadRecord || operation == Operation::ReadPending) && result.length) {
        validHeader = result.length >= sizeof(header) && _d.store->readPayload(held, &header, sizeof(header));
        foreignIdentity = validHeader && memcmp(header.source, _d.ourDestHash, 16);
        validHeader = validHeader && !foreignIdentity && !header.incoming && header.counter == result.key.counter &&
            header.revision == result.revision && !memcmp(header.destination, result.key.peer, 16);
        if (operation == Operation::ReadRecord) {
            validHeader = validHeader && value->counter == result.key.counter &&
                !memcmp(value->peer, result.key.peer, 16) &&
                sendableBody(header.titleLength, header.contentLength) &&
                result.length == sizeof(header) + header.titleLength + header.contentLength && !result.more &&
                result.nextOffset == header.titleLength + header.contentLength &&
                _body.slot == ticket.slot && _body.generation == ticket.generation;
            if (validHeader) {
                _body.header = header;
                validHeader = _d.store->readPayload(held, _body.bytes,
                    header.titleLength + header.contentLength, sizeof(header));
            }
        }
    }
    _d.store->releaseResult(held);
    value->storageSequence = 0; value->storageSlot = UINT8_MAX;
    if (operation == Operation::CreateOutgoing) {
        value->initial = result.outcome == Outcome::Committed ? 2 : result.outcome == Outcome::Cancelled ? 4 : 3;
        value->initialError = result.error;
        if (value->flags & Suppressed) value->flags |= InitialSuppressed;
        if (result.outcome == Outcome::Committed) {
            value->counter = result.key.counter; value->revision = result.revision;
            value->durable = LXMFStatus(result.newStatus); value->error = result.error;
            value->phase = value->flags & Suppressed ? Phase::Settled :
                value->flags & RelayRequired ? Phase::Relay : Phase::Ready;
        } else { value->error = result.error; value->phase = Phase::Settled; }
    } else if (operation == Operation::UpdateStatus) {
        if (result.outcome == Outcome::Committed) {
            value->revision = result.revision; value->durable = LXMFStatus(result.newStatus);
        }
        value->error = result.error == Error::None && (value->flags & BlockedRecord) ? Error::InvalidRecord : result.error;
        value->statusRetry = _d.clock->nowMs() + (result.outcome == Outcome::Committed ? 0 : TX_RETRY_MS);
    } else if (operation == Operation::ReadPending) {
        if (!_recovering || (value->flags & Suppressed)) { value->phase = Phase::Settled; }
        else if (result.outcome != Outcome::Committed) {
            value->error = result.error; value->nextAttempt = _d.clock->nowMs() + TX_RETRY_MS;
        } else if (!result.length) {
            _recovering = false; value->phase = Phase::Settled;
        } else {
            _recoveryCursor = result.key;
            if (!validHeader) { value->phase = Phase::Settled; }
            else {
                bool duplicate = false;
                for (uint8_t i = 0; i < RowCount; ++i) if (i != ticket.slot && _rows[i].phase != Phase::Free &&
                    _rows[i].counter == result.key.counter && !memcmp(_rows[i].peer, result.key.peer, 16)) duplicate = true;
                if (duplicate) value->phase = Phase::Settled;
                else {
                    memcpy(value->peer, result.key.peer, 16); memcpy(value->messageId, header.messageId, 32);
                    value->counter = result.key.counter; value->revision = result.revision;
                    value->timestamp = header.timestamp; value->durable = LXMFStatus(header.status);
                    value->desired = LXMFStatus::QUEUED; value->phase = Phase::Ready; value->error = Error::None;
                    if (header.deliveryPolicy == handheld::messaging::DeliveryPolicy::Auto) value->flags |= PolicyAuto;
                    if (header.deliveryPolicy == handheld::messaging::DeliveryPolicy::Always) value->flags |= PolicyAlways;
                    if ((value->flags & PolicyAlways) || handheld::messaging::relayStatus(header.status)) {
                        value->flags |= RelayRequired; value->phase = Phase::Relay;
                        value->desired = LXMFStatus::PROP_QUEUED;
                    }
                    if (!sendableBody(header.titleLength, header.contentLength)) {
                        value->error = Error::InvalidRecord; value->phase = Phase::Settled;
                        value->flags |= Suppressed | BlockedRecord;
                        setStatus(ticket, LXMFStatus::FAILED);
                    }
                }
            }
        }
    } else if (operation == Operation::ReadRecord) {
        if (value->flags & Suppressed || value->desired == LXMFStatus::DELIVERED) {
            releaseBody(ticket); value->phase = Phase::Settled;
        } else if (result.outcome != Outcome::Committed) {
            releaseBody(ticket); value->error = result.error;
            value->phase = value->flags & RelayRequired ? Phase::Relay : Phase::Ready;
            value->nextAttempt = _d.clock->nowMs() + TX_RETRY_MS;
        } else if (foreignIdentity) {
            // A record replaced/restored from another identity is preserved.
            // It cannot be transmitted or rewritten as this active author.
            releaseBody(ticket); value->error = Error::InvalidRecord;
            value->flags |= Suppressed | BlockedRecord; value->phase = Phase::Settled;
            value->receiptMask = 0; value->proofCount = 0; value->desired = value->durable;
            if (_relay.ticket == ticket) resetRelay();
        } else if (!validHeader) {
            if (_relay.ticket == ticket) resetRelay();
            releaseBody(ticket); value->error = Error::InvalidRecord; value->phase = Phase::Settled;
            value->flags |= Suppressed | BlockedRecord;
            setStatus(ticket, LXMFStatus::FAILED);
        } else {
            value->revision = result.revision; value->timestamp = header.timestamp;
            value->error = Error::None; value->phase = Phase::Reading;
            if (header.deliveryPolicy == handheld::messaging::DeliveryPolicy::Always || handheld::messaging::relayStatus(header.status)) {
                value->flags |= RelayRequired;
                if (_relay.ticket != ticket) { releaseBody(ticket); value->phase = Phase::Relay; }
            }
        }
    }
    ++_statusRevision;
}

bool RustLxmfEngine::receiptHook(void* context, handheld::TxReceipt receipt, handheld::TxReceiptEvent event) {
    auto& owner = *static_cast<RustLxmfEngine*>(context);
    static_assert(RustLinkManager::RequestReceiptSlot >= RustIncomingDelivery::ReceiptCount &&
                  RustLinkManager::RrcReceiptSlot < ReceiptBase, "Receipt namespaces must not overlap");
    if (receipt.slot == RustLinkManager::RrcReceiptSlot)
        return owner._d.links && owner._d.links->rrcReceipt(receipt, event);
    if (receipt.slot == RustLinkManager::RequestReceiptSlot || receipt.slot == RustLinkManager::IdentifyReceiptSlot)
        return owner._d.links && owner._d.links->requestReceipt(receipt, event);
    if (receipt.slot < RustIncomingDelivery::ReceiptCount)
        return RustIncomingDelivery::receiptHook(&owner._incoming, receipt, event);
    if (receipt.slot < ReceiptBase || receipt.slot >= ReceiptBase + RowCount * 4) return false;
    const Ticket ticket{receipt.generation, uint8_t((receipt.slot - ReceiptBase) / 4)};
    const uint8_t variant = (receipt.slot - ReceiptBase) % 4;
    auto* value = owner.row(ticket);
    if (!value) return false;
    if (event == handheld::TxReceiptEvent::Validate) return owner._accepting &&
        (!(value->flags & RelayRequired) || (variant == 3 && owner._relay.ticket == ticket && owner.relayAllowed())) &&
        value->identityGeneration == owner._identityGeneration && !(value->flags & (Suppressed | Deleted)) &&
        value->desired != LXMFStatus::DELIVERED && (value->receiptMask & (1u << variant));
    if (value->queuedReceipts) --value->queuedReceipts;
    if (event == handheld::TxReceiptEvent::Started) {
        if (value->flags & RelayRequired) {
            if (owner._relay.ticket == ticket) {
                owner._relay.emitted = true; owner._relay.sentAt = owner._d.clock->nowMs();
            }
            owner.setStatus(ticket, LXMFStatus::PROP_SENDING);
        } else owner.setStatus(ticket, LXMFStatus::SENT);
    }
    return true;
}

void RustLxmfEngine::preparePacket(Ticket ticket, const uint8_t* raw, size_t length,
    uint8_t interfaceId, bool broadcast) {
    auto* value = row(ticket);
    if (!value || value->proofCount >= ProofAttempts || !length || length > 500) return;
    memcpy(_body.bytes, raw, length); _body.length = length;
    _body.targets = 0;
    for (uint8_t i = 0; i <= RustInterfacePump::WIFI_AP_IFACE_ID; ++i) {
        _body.generations[i] = (broadcast || i == interfaceId) ? _d.pump->interfaceGeneration(i) : 0;
        if (_body.generations[i]) _body.targets |= 1u << i;
    }
    uint8_t first = 0;
    while (first < 7 && !(_body.targets & (1u << first))) ++first;
    const uint64_t now = _d.clock->nowMs();
    if (first == 7 || !_d.pump->captureLeaseAt(first, raw, length, now, 120000, _body.lease)) {
        value->nextAttempt = now + TX_RETRY_MS; value->phase = Phase::Ready; releaseBody(ticket); return;
    }
    const uint8_t attempt = value->proofCount;
    rs_handheld_rns_packet_hash(raw, length, raw[0] & 0x40 ? 1 : 0, value->hashes[attempt]);
    _body.lease.setReceipt({ticket.generation, uint8_t(ReceiptBase + ticket.slot * 4 + attempt)});
    value->receiptMask |= 1u << attempt;
    value->phase = Phase::Prepared;
    value->receiptSince = now;
    value->proofWait = PROOF_TIMEOUT_MS + proofJitterMs() + _d.pump->interfaceTxWaitMs(interfaceId, 2);
    value->nextAttempt = now;
    offerPrepared(ticket);
}

void RustLxmfEngine::offerPrepared(Ticket ticket) {
    auto* value = row(ticket);
    if (!value || _body.slot != ticket.slot || _body.generation != ticket.generation) return;
    bool accepted = false;
    const uint8_t attempt = value->proofCount;
    // The hash and count are installed before a synchronous interface may
    // deliver a proof or Started callback. Rejection rolls this provisional
    // attempt back; accepted radio queue ownership consumes it once.
    ++value->proofCount;
    value->phase = Phase::AwaitProof;
    for (uint8_t i = 0; i < 7; ++i) {
        if (!(_body.targets & (1u << i))) continue;
        _body.lease.interfaceId = i; _body.lease.generation = _body.generations[i];
        ++value->queuedReceipts;
        const auto offer = _d.pump->offerReceipt(_body.bytes, _body.length, _body.lease);
        value = row(ticket);
        if (!value) return;
        if (offer == handheld::TxOffer::Blocked) --value->queuedReceipts;
        else _body.targets &= ~(1u << i);
        accepted |= offer == handheld::TxOffer::Started || offer == handheld::TxOffer::Queued;
        if (_body.slot != ticket.slot || _body.generation != ticket.generation ||
            value->desired == LXMFStatus::DELIVERED || (value->flags & Suppressed)) return;
    }
    if (accepted) {
        releaseBody(ticket); value->nextAttempt = 0;
    } else {
        value->proofCount = attempt;
        value->receiptMask &= ~(1u << attempt);
        releaseBody(ticket); value->phase = Phase::Ready;
        value->nextAttempt = _d.clock->nowMs() + TX_RETRY_MS;
    }
}

handheld::TxOffer RustLxmfEngine::offerResource(Ticket ticket, uint8_t iface, const uint8_t* raw,
    size_t length, uint64_t bornMs) {
    auto* value = row(ticket);
    if (!value || value->phase != Phase::Resource || !_accepting || value->flags & Suppressed)
        return handheld::TxOffer::Rejected;
    handheld::TxLease lease;
    if (!_d.pump->captureLeaseAt(iface, raw, length, bornMs, 120000, lease)) return handheld::TxOffer::Rejected;
    lease.setReceipt({ticket.generation, uint8_t(ReceiptBase + ticket.slot * 4 + 3)});
    value->receiptMask |= 8;
    ++value->queuedReceipts;
    const auto result = _d.pump->offerReceipt(raw, length, lease);
    value = row(ticket);
    if (value && result == handheld::TxOffer::Blocked) --value->queuedReceipts;
    return result;
}

uint64_t RustLxmfEngine::resourceSendBinding(Ticket ticket, uint8_t iface, const uint8_t linkId[16]) const {
    const auto* value = row(ticket);
    if (!value || !_accepting || value->phase != Phase::Resource || (value->flags & Suppressed) || !_d.links) return 0;
    const auto* peer = (value->flags & RelayRequired) && _relay.ticket == ticket ? _relay.node : value->peer;
    const auto* active = _d.links->activeLinkId(peer);
    uint8_t slot = UINT8_MAX; uint32_t generation = 0;
    if (!active || _d.links->activeLinkIface(peer) != iface || memcmp(active, linkId, 16) ||
        !_d.links->receiptBinding(iface, linkId, slot, generation)) return 0;
    return (uint64_t(generation) << 8) | slot;
}

void RustLxmfEngine::finishRouteFailure(Ticket ticket) {
    auto* value = row(ticket);
    if (!value) return;
    if (beginRelay(ticket)) return;
    value->flags &= ~Rediscover;
    releaseBody(ticket);
    if (value->proofCount) {
        value->phase = Phase::Grace; value->flags |= ProofGrace;
        value->receiptSince = _d.clock->nowMs();
        setStatus(ticket, LXMFStatus::UNCONFIRMED);
    } else {
        value->phase = Phase::Settled;
        setStatus(ticket, LXMFStatus::FAILED);
    }
}

void RustLxmfEngine::onLinkSetupFailure(const uint8_t peer[16], const rs_handheld_route_t& failedRoute) {
    if (!_accepting) return;
    _inbox.onLinkSetupFailure(peer);
    if (_relay.ticket.valid() && !memcmp(_relay.node, peer, 16)) {
        finishRelay(_relay.ticket, LXMFStatus::PROP_UNAVAILABLE, true);
    }
    for (auto& value : _rows) {
        if ((value.phase != Phase::Ready && value.phase != Phase::Reading) ||
            !(value.flags & (PreferLink | ViaLink)) || value.flags & Suppressed ||
            value.desired == LXMFStatus::DELIVERED || memcmp(value.peer, peer, 16)) continue;
        // The Link manager owns the failed handshake. Its waiting messages
        // reuse the same route-recovery operation as a failed packet receipt.
        value.route = failedRoute; value.flags |= Rediscover;
        value.discoveryCount = 0; value.nextAttempt = 0;
    }
}

void RustLxmfEngine::attempt(Ticket ticket) {
    auto* value = row(ticket);
    if (!value || !_accepting || value->flags & Suppressed || _body.slot != ticket.slot ||
        _body.generation != ticket.generation) return;
    const uint64_t now = _d.clock->nowMs();
    rs_handheld_route_t route = {};
    if (rs_handheld_rns_route(_d.ctx, value->peer, now, &route) != RS_HANDHELD_OK) {
        value->nextAttempt = now + TX_RETRY_MS; releaseBody(ticket); value->phase = Phase::Ready; return;
    }
    if (value->linkSince && _d.links && !_d.links->linkActive(value->peer) &&
        now - value->linkSince > LINK_WAIT_TIMEOUT_MS + _d.pump->interfaceTxWaitMs(value->route.interface_id, 4)) {
        finishRouteFailure(ticket); return;
    }
    if ((value->flags & (PreferLink | ViaLink)) && route.kind == RS_HANDHELD_ROUTE_BROADCAST)
        value->flags |= Rediscover;
    if (value->flags & Rediscover) {
        const auto& old = value->route;
        const bool same = route.kind == old.kind && route.interface_id == old.interface_id &&
            route.header_type == old.header_type && route.hops == old.hops && !memcmp(route.next_hop, old.next_hop, 16);
        // A remembered public key does not make a failed radio route usable.
        // Keep this bounded discovery operation alive if its response is lost;
        // otherwise the two remaining packet attempts become unroutable
        // HEADER_1 broadcasts. Only the first request retires the failed path.
        // Any path learned after that retirement is new, even if its fields
        // happen to match the old route.
        if (route.kind == RS_HANDHELD_ROUTE_BROADCAST || (!value->discoveryCount && same)) {
            if (value->discoveryCount >= DISCOVERY_MAX_ATTEMPTS) {
                finishRouteFailure(ticket); return;
            }
            uint8_t tag[16]; RustEntropy::fill(tag, sizeof(tag));
            if (!value->discoveryCount) rs_handheld_rns_drop_path(_d.ctx, value->peer);
            rs_handheld_rns_request_path(_d.ctx, value->peer, tag, old.interface_id, now);
            ++value->discoveryCount; value->nextAttempt = now + DISCOVERY_RETRY_MS;
            releaseBody(ticket); value->phase = Phase::Ready; return;
        }
        value->flags &= ~Rediscover;
    }
    bool havePub = _d.keymap && _d.keymap->recall(value->peer, value->publicKey);
    int32_t hasPath = 0, hasNext = 0; uint8_t hops = 0, next[16], pathPub[64];
    if (rs_handheld_rns_path_info(_d.ctx, value->peer, now, &hasPath, &hops, next, &hasNext, pathPub) != RS_HANDHELD_OK) {
        value->nextAttempt = now + TX_RETRY_MS; releaseBody(ticket); value->phase = Phase::Ready; return;
    }
    if (!havePub && hasPath) {
        memcpy(value->publicKey, pathPub, 64); havePub = true;
        if (_d.keymap) _d.keymap->learn(value->peer, pathPub, now);
    }
    if (!havePub) {
        uint8_t tag[16]; RustEntropy::fill(tag, sizeof(tag));
        rs_handheld_rns_request_path(_d.ctx, value->peer, tag, 0, now);
        releaseBody(ticket); value->phase = Phase::Ready;
        value->nextAttempt = now + DISCOVERY_RETRY_MS;
        if (++value->discoveryCount >= DISCOVERY_MAX_ATTEMPTS) {
            if (!beginRelay(ticket)) { value->phase = Phase::Settled; setStatus(ticket, LXMFStatus::FAILED); }
        }
        return;
    }
    uint8_t recipientCost = 0;
    const bool knownCost = _d.keymap && _d.keymap->recallCost(value->peer, RustClock::synchronizedEpochSecs(), now, recipientCost);
    if (_relay.ticket == ticket && _relay.direct && !knownCost) {
        releaseBody(ticket); value->phase = Phase::Settled; setStatus(ticket, LXMFStatus::STAMP_UNKNOWN); return;
    }
    if (knownCost && recipientCost > RustStampWork::MaxCost) {
        releaseBody(ticket); value->phase = Phase::Settled; setStatus(ticket, LXMFStatus::STAMP_COST_HIGH); return;
    }
    if (knownCost && recipientCost) {
        value->flags |= PreferLink | ViaLink;
        if (route.kind != RS_HANDHELD_ROUTE_DIRECT) {
            value->route = route; value->flags |= Rediscover;
            releaseBody(ticket); value->phase = Phase::Ready; value->nextAttempt = now; return;
        }
    }
    const auto& header = _body.header;
    if (!(value->flags & (ViaLink | PreferLink))) {
        uint8_t ephemeral[32], iv[16], cipher[600], destination[16], messageId[32]; size_t cipherLength = 0;
        RustEntropy::fill(ephemeral, sizeof(ephemeral)); RustEntropy::fill(iv, sizeof(iv));
        const auto built = rs_handheld_rns_lxmf_build(_d.ctx, value->publicKey, value->timestamp,
            RustClock::synchronizedEpochSecs(), now, _body.bytes, header.titleLength, _body.bytes + header.titleLength,
            header.contentLength, ephemeral, iv, cipher, sizeof(cipher), &cipherLength, destination, messageId);
        secureZero(ephemeral, sizeof(ephemeral)); secureZero(iv, sizeof(iv));
        if (built == RS_HANDHELD_OK && !memcmp(destination, value->peer, 16)) {
            uint8_t raw[640]; size_t length = 0;
            rs_handheld_rns_packet_build(route.header_type, RustWire::PT_DATA, RustWire::DT_SINGLE, RustWire::CTX_NONE,
                route.header_type == 1 ? route.next_hop : nullptr, value->peer, cipher, cipherLength, raw, sizeof(raw), &length);
            const bool gated = route.kind == RS_HANDHELD_ROUTE_DIRECT && route.interface_id == RustInterfacePump::LORA_IFACE_ID &&
                length > RSDECK_RNODE_SINGLE_FRAME_RAW_MAX;
            if (length && !gated) {
                memcpy(value->messageId, messageId, 32); value->route = route;
                preparePacket(ticket, raw, length, route.interface_id, route.kind == RS_HANDHELD_ROUTE_BROADCAST); return;
            }
        }
        value->flags |= ViaLink;
    }
    if (!_d.links || !_d.resources || route.kind != RS_HANDHELD_ROUTE_DIRECT) {
        releaseBody(ticket); value->phase = Phase::Settled; setStatus(ticket, LXMFStatus::FAILED); return;
    }
    value->route = route;
    if (!_d.links->ensureLink(value->peer, value->publicKey, route)) {
        releaseBody(ticket); value->phase = Phase::Ready;
        if (!value->linkSince && _d.links->linkEstablishing(value->peer)) value->linkSince = now;
        value->nextAttempt = now + TX_RETRY_MS;
        return;
    }
    // Resource assembly and outgoing encoding share one synchronous workspace.
    // Keep the existing outgoing capacity even though assembly needs 16 extra
    // bytes. The FFI's temporary signing scratch is a separate heap allocation.
    auto& packed = _d.resources->_codec;
    uint8_t destination[16]; size_t length = 0;
    const auto built = rs_handheld_rns_lxmf_build_link(_d.ctx, value->publicKey, value->timestamp,
        _body.bytes, header.titleLength, _body.bytes + header.titleLength, header.contentLength,
        packed, RS_HANDHELD_RESOURCE_DATA_MAX, &length, destination, value->messageId);
    if (built != RS_HANDHELD_OK || memcmp(destination, value->peer, 16)) {
        releaseBody(ticket); value->phase = Phase::Settled; setStatus(ticket, LXMFStatus::FAILED); return;
    }
    if (recipientCost || (_relay.ticket == ticket && _relay.direct)) {
        if (!directStamp(ticket, recipientCost)) return;
        if (rs_handheld_lxmf_append_stamp(packed, length, RS_HANDHELD_RESOURCE_DATA_MAX,
                _relay.recipientStamp, &length) != RS_HANDHELD_OK) {
            releaseBody(ticket); value->phase = Phase::Settled; setStatus(ticket, LXMFStatus::STAMP_FAILED); return;
        }
    }
    if (length <= RS_HANDHELD_LINK_MDU) {
        uint8_t raw[500]; size_t rawLength = 0;
        if (_d.links->buildLinkDataPacket(value->peer, packed, length, raw, sizeof(raw), rawLength)) {
            value->flags |= PreferLink;
            preparePacket(ticket, raw, rawLength, _d.links->activeLinkIface(value->peer), false);
        } else { releaseBody(ticket); value->phase = Phase::Ready; value->nextAttempt = now + TX_RETRY_MS; }
        return;
    }
    if (_d.resources->sending()) {
        releaseBody(ticket); value->phase = Phase::Ready; value->linkSince = 0;
        value->nextAttempt = now + TX_RETRY_MS; return;
    }
    const uint8_t* link = _d.links->activeLinkId(value->peer);
    const uint8_t* session = _d.links->activeLinkKey(value->peer);
    const uint8_t iface = _d.links->activeLinkIface(value->peer);
    value->phase = Phase::Resource;
    releaseBody(ticket);
    const bool started = link && session && _d.resources->startSend(ticket, value->peer, link, session, iface, packed, length);
    value = row(ticket);
    if (value && !started && value->phase == Phase::Resource) {
        value->phase = Phase::Ready; value->nextAttempt = now + TX_RETRY_MS;
    }
}

void RustLxmfEngine::onResourceOutcome(Ticket ticket, handheld::outgoing::ResourceOutcome outcome) {
    auto* value = row(ticket);
    if (!value || value->phase != Phase::Resource) return;
    if ((value->flags & RelayRequired) && _relay.ticket == ticket) {
        finishRelay(ticket, outcome == handheld::outgoing::ResourceOutcome::Delivered ? LXMFStatus::PROPAGATED :
            outcome == handheld::outgoing::ResourceOutcome::Rejected ? LXMFStatus::PROP_REJECTED :
            outcome == handheld::outgoing::ResourceOutcome::NetworkFailure && _relay.emitted ?
                LXMFStatus::PROP_UNCONFIRMED : LXMFStatus::PROP_UNAVAILABLE,
            outcome == handheld::outgoing::ResourceOutcome::NetworkFailure);
        return;
    }
    if (outcome == handheld::outgoing::ResourceOutcome::NetworkFailure && beginRelay(ticket)) return;
    value->receiptMask &= ~8; value->phase = Phase::Settled;
    setStatus(ticket, outcome == handheld::outgoing::ResourceOutcome::Delivered ? LXMFStatus::DELIVERED : LXMFStatus::FAILED);
}

bool RustLxmfEngine::validatesReceipt(const OutgoingRow& value, const rs_handheld_local_frame_t& frame) const {
    for (uint8_t i = 0; i < value.proofCount; ++i) {
        int32_t valid = 0;
        if (rs_handheld_rns_proof_validate(value.publicKey, value.hashes[i], frame.payload,
            frame.payload_len, &valid) == RS_HANDHELD_OK && valid) return true;
    }
    return false;
}
void RustLxmfEngine::onProofFrame(const rs_handheld_local_frame_t& frame) {
    if (!_d.clock) return;
    const uint64_t now = _d.clock->nowMs();
    if (_relay.ticket.valid() && _relay.packet && _relay.stage == RelayWork::Stage::Await) {
        int32_t valid = 0;
        if (rs_handheld_rns_proof_validate(_relay.nodeKey, _relay.packetHash, frame.payload, frame.payload_len, &valid) == RS_HANDHELD_OK && valid) {
            finishRelay(_relay.ticket, LXMFStatus::PROPAGATED); return;
        }
    }
    for (uint8_t i = 0; i < RowCount; ++i) {
        const auto& value = _rows[i];
        if (value.phase == Phase::Free || value.flags & (Deleted | DeletePending) ||
            value.desired == LXMFStatus::DELIVERED ||
            // The original grace clock remains authoritative after polling or
            // cancellation changes the phase to Settled. A retained consumer
            // result must never reopen an expired network proof window.
            ((value.flags & ProofGrace) && now - value.receiptSince > LATE_PROOF_GRACE_MS)) continue;
        if (validatesReceipt(value, frame)) { setStatus({value.generation, i}, LXMFStatus::DELIVERED); return; }
    }
}

void RustLxmfEngine::advance(Ticket ticket) {
    auto* value = row(ticket);
    if (!value || value->storageSequence) return;
    const uint64_t now = _d.clock->nowMs();
    if (value->phase == Phase::Settled && (value->flags & Acknowledged) && !value->queuedReceipts &&
        (!(_accepting && (value->flags & BlockedRecord) && !(value->flags & Recovered))) &&
        !(value->flags & DeletePending) && (value->desired == value->durable || (value->flags & Deleted) || !value->counter)) {
        retire(ticket); return;
    }
    if (value->phase == Phase::AwaitProof && now - value->receiptSince > value->proofWait) {
        if (value->proofCount >= ProofAttempts) {
                if (!beginRelay(ticket)) {
                    value->phase = Phase::Grace; value->flags |= ProofGrace;
                    value->receiptSince = now; setStatus(ticket, LXMFStatus::UNCONFIRMED);
                }
        } else {
            value->phase = Phase::Ready; value->discoveryCount = 0;
            if (!(value->flags & PreferLink)) value->flags |= Rediscover;
            setStatus(ticket, LXMFStatus::QUEUED);
        }
    }
    if (value->phase == Phase::Grace && now - value->receiptSince > LATE_PROOF_GRACE_MS) value->phase = Phase::Settled;
    if (value->counter && value->desired != value->durable && now >= value->statusRetry &&
        !(value->flags & (Deleted | DeletePending))) {
        const auto admission = _d.store->requestStatus(key(*value), value->desired, _identityGeneration);
        if (admission.accepted()) { hold(*value, admission, Operation::UpdateStatus); return; }
        value->statusRetry = now + TX_RETRY_MS;
    }
    if (now < value->nextAttempt) return;
    if (value->flags & Suppressed) return;
    if (value->phase == Phase::Query) {
        const auto admission = _d.store->requestPending(_recoveryCursor);
        if (admission.accepted()) hold(*value, admission, Operation::ReadPending);
        return;
    }
    if (!_accepting) return;
    if (value->phase == Phase::Stamp) {
        if (_relay.ticket == ticket && !_relay.work.busy()) { value->phase = Phase::Ready; value->nextAttempt = 0; }
        return;
    }
    if (value->flags & RelayRequired) {
        if ((!_d.propagation || !_d.propagation->settings().enabled) && _relay.ticket != ticket &&
            (value->phase == Phase::Relay || value->phase == Phase::Reading)) {
            releaseBody(ticket); value->phase = Phase::Settled; value->flags |= PausedRelay;
        }
        if (value->desired == value->durable && (value->phase == Phase::Relay || value->phase == Phase::Reading)) advanceRelay(ticket);
        return;
    }
    if (value->phase == Phase::Prepared) { offerPrepared(ticket); return; }
    if (value->phase == Phase::Reading) { attempt(ticket); return; }
    if (value->phase != Phase::Ready || _body.slot != UINT8_MAX) return;
    const auto admission = _d.store->requestRecord(key(*value));
    if (admission.accepted()) {
        _body.slot = ticket.slot; _body.generation = ticket.generation;
        hold(*value, admission, Operation::ReadRecord); value->phase = Phase::Reading;
    }
}
void RustLxmfEngine::loop() {
    if (_polling) return;
    if (_d.pump) _d.pump->pollRadioBeforeBlockingWork();
    _polling = true;
    _incoming.poll();
    if (!_d.store || !_d.clock) { _polling = false; return; }
    _d.store->poll();
    _inbox.poll();
    const bool enabled = _d.propagation && _d.propagation->settings().enabled;
    if (enabled && !_propagationWasEnabled && _accepting) {
        _recovering = true; _recoveryCursor = {};
        for (auto& value : _rows) if (value.flags & PausedRelay) {
            value.flags &= ~PausedRelay;
            if (!(value.flags & Suppressed) && value.phase == Phase::Settled) value.phase = Phase::Relay;
        }
    }
    _propagationWasEnabled = enabled;
    if (_relay.ticket.valid()) {
        auto* active = row(_relay.ticket);
        if (!active) resetRelay();
        else if (!relayAllowed() && !active->storageSequence) {
            const auto ticket = _relay.ticket;
            if (_relay.emitted) finishRelay(ticket, LXMFStatus::PROP_UNCONFIRMED);
            else {
                active->phase = Phase::Relay; active->receiptMask &= ~8;
                if (_d.resources) _d.resources->cancelSend(ticket);
                releaseBody(ticket); resetRelay();
            }
        } else if (relayAllowed()) _relay.work.poll(_d.clock->nowMs());
    }
    for (uint8_t i = 0; i < RowCount; ++i) settleStorage({_rows[i].generation, i});
    // One bounded attempt/read/status admission per selected row. A rotating
    // cursor gives backpressured and failed work the same finite loop budget.
    for (uint8_t processed = 0; processed < 3; ++processed) {
        // Settlement above only consumes completed results. New requests may
        // run inline in the fallback executor; only those need an idle modem.
        // The normal worker keeps healthy interfaces moving during LoRa TX.
        if (_d.pump && !_d.pump->pollRadioBeforeBlockingWork() && !_d.store->deferredIO()) break;
        const uint8_t index = _cursor; _cursor = (_cursor + 1) % RowCount;
        advance({_rows[index].generation, index});
    }
    if (_recovering && _accepting) {
        bool query = false;
        for (const auto& value : _rows) query |= value.phase == Phase::Query;
        if (!query) {
            const auto ticket = allocate();
            if (auto* value = row(ticket)) { value->phase = Phase::Query; value->flags = Acknowledged | Recovered; }
        }
    }
    // Callbacks see durable owner state after all workspace use has ended.
    for (uint8_t i = 0; i < RowCount; ++i) {
        auto& value = _rows[i];
        if (!(value.flags & Notify) || value.phase == Phase::Free || _body.slot != UINT8_MAX) continue;
        value.flags &= ~Notify;
        if (_d.statusCb && *_d.statusCb) (*_d.statusCb)(hex(value.peer, 16), value.timestamp, value.counter, value.desired);
    }
    _polling = false;
}


bool RustLxmfEngine::buildPacketProof(const uint8_t packetHash[32], uint8_t raw[128], size_t& rawLen) {
    uint8_t proof[RS_HANDHELD_PROOF_MAX];
    size_t proofLen = 0;
    if (rs_handheld_rns_proof_build(_d.ctx, packetHash, 1, proof, sizeof(proof), &proofLen) !=
        RS_HANDHELD_OK) return false;
    // The trusted packet builder creates the SINGLE proof destination framing.
    return rs_handheld_rns_packet_build(0, RustWire::PT_PROOF, RustWire::DT_SINGLE, RustWire::CTX_NONE,
        nullptr, packetHash, proof, proofLen, raw, 128, &rawLen) == RS_HANDHELD_OK && rawLen;
}

void RustLxmfEngine::onDataFrame(const rs_handheld_local_frame_t& f, uint8_t ifaceId) {
    if (f.context != RustWire::CTX_NONE) return;  // link/resource contexts routed elsewhere
    uint8_t src[16];
    uint8_t keyHint = RS_HANDHELD_LXMF_BASE_KEY_HINT;
    if (rs_handheld_rns_lxmf_peek_source_hint(_d.ctx, f.payload, f.payload_len, src, &keyHint) !=
        RS_HANDHELD_OK) {
        return;
    }
    uint8_t pub[64];
    bool havePub = _d.keymap && _d.keymap->recall(src, pub);
    if (!havePub) {
        // Try the path table's announced key; else request a path and drop (sender retries).
        int32_t hp = 0, hn = 0;
        uint8_t hops = 0, nh[16] = {};
        if (rs_handheld_rns_path_info(_d.ctx, src, _d.clock->nowMs(), &hp, &hops, nh, &hn, pub) ==
                RS_HANDHELD_OK &&
            hp) {
            havePub = true;
        }
    }
    if (!havePub) {
        requestUnknownSource(src);
        Serial.println("[RUST-LXMF] inbound: source key unknown; dropping");
        return;
    }
    rs_handheld_lxmf_message_t msg;
    if (rs_handheld_rns_lxmf_parse_hint(_d.ctx, f.payload, f.payload_len, keyHint, pub, &msg) !=
        RS_HANDHELD_OK) {
        Serial.println("[RUST-LXMF] inbound parse/validate failed");
        return;
    }
    if (_d.keymap) _d.keymap->learn(src, pub, _d.clock->nowMs());
    uint8_t raw[128]; size_t rawLen = 0;
    RustIncomingDelivery::ReceiptSeed seed;
    if (!buildPacketProof(f.packet_hash, raw, rawLen) ||
        !_incoming.captureSeed(seed, RustIncomingDelivery::Kind::Packet, ifaceId, raw, rawLen)) return;
    const uint64_t epoch = RustClock::epochSecs();
    RustIncomingDelivery::MessageView view;
    view.messageId = msg.message_id; view.source = msg.source_hash;
    view.title = msg.title; view.titleLength = msg.title_len;
    view.content = msg.content; view.contentLength = msg.content_len;
    view.timestamp = epoch ? double(epoch) : msg.timestamp; view.reaction = msg.is_reaction != 0;
    _incoming.accept(view, seed);
}

RustIncomingDelivery::ReceiveResult RustLxmfEngine::onDirectPayload(
    const uint8_t* packed, size_t len, const RustIncomingDelivery::ReceiptSeed& seed) {
    using Code = RustIncomingDelivery::ReceiveCode;
    using Error = RustIncomingDelivery::ReceiveError;
    if (!packed || len < 32) return {Code::Rejected, {}, Error::Invalid};
    uint8_t src[16]; memcpy(src, packed + 16, 16);
    uint8_t pub[64];
    bool havePub = _d.keymap && _d.keymap->recall(src, pub);
    if (!havePub) {
        int32_t hp = 0, hn = 0;
        uint8_t hops = 0, nh[16] = {};
        if (rs_handheld_rns_path_info(_d.ctx, src, _d.clock->nowMs(), &hp, &hops, nh, &hn, pub) ==
            RS_HANDHELD_OK && hp) havePub = true;
    }
    if (!havePub) {
        requestUnknownSource(src);
        return {Code::Rejected, {}, Error::SourceUnknown};
    }
    rs_handheld_lxmf_view_t parsed = {};
    if (rs_handheld_rns_lxmf_parse_link_view(_d.ctx, packed, len, pub, &parsed) != RS_HANDHELD_OK ||
        parsed.title_offset > len || parsed.title_len > len - parsed.title_offset ||
        parsed.content_offset > len || parsed.content_len > len - parsed.content_offset ||
        !handheld::storage::Budget::validBody(parsed.title_len, parsed.content_len))
        return {Code::Rejected, {}, Error::Invalid};
    if (_d.keymap) _d.keymap->learn(parsed.source_hash, pub, _d.clock->nowMs());
    const uint64_t epoch = RustClock::epochSecs();
    RustIncomingDelivery::MessageView view;
    view.messageId = parsed.message_id; view.source = parsed.source_hash;
    view.title = packed + parsed.title_offset; view.titleLength = parsed.title_len;
    view.content = packed + parsed.content_offset; view.contentLength = parsed.content_len;
    view.timestamp = epoch ? double(epoch) : parsed.timestamp; view.reaction = parsed.is_reaction != 0;
    return _incoming.accept(view, seed);
}

void RustLxmfEngine::requestUnknownSource(const uint8_t source[16]) {
    const unsigned long now = _d.clock ? (unsigned long)_d.clock->nowMs() : millis();
    SourceRequest* slot = nullptr;
    for (auto& request : _sourceRequests) {
        if (request.used && memcmp(request.source, source, 16) == 0) {
            if (now - request.sentMs < SOURCE_REQUEST_THROTTLE_MS) return;
            slot = &request;
            break;
        }
        if (!request.used || now - request.sentMs >= SOURCE_REQUEST_THROTTLE_MS) {
            if (!slot) slot = &request;
        }
    }
    // Under a >16-source burst, suppress new requests until an existing throttle
    // slot expires rather than permit an attacker to evict and immediately retry.
    if (!slot) return;
    slot->used = true;
    memcpy(slot->source, source, 16);
    slot->sentMs = now;
    uint8_t tag[16];
    RustEntropy::fill(tag, sizeof(tag));
    rs_handheld_rns_request_path(_d.ctx, source, tag, 0, now);
}

// Propagation uses the existing row, storage executor, codec scratch and Link /
// Resource pools. Only hashes, public keys and one incremental stamp are retained.
void RustLxmfEngine::resetRelay() {
    if (_d.propagation) _d.propagation->release(_relay.nodeOwner);
    _relay.nodeOwner = 0;
    _relay.work.reset(); _relay.ticket = {};
    _relay.born = _relay.requestAt = _relay.sentAt = _relay.linkSince = 0;
    _relay.stage = RelayWork::Stage::Select;
    _relay.recipientCost = _relay.nodeCost = _relay.stampedCost = 0;
    _relay.direct = _relay.haveRecipientStamp = false;
    _relay.prepared = _relay.verified = _relay.emitted = _relay.packet = false;
    _relay.waitMs = 0;
    memset(_relay.node, 0, sizeof _relay.node);
    memset(_relay.nodeKey, 0, sizeof _relay.nodeKey);
    memset(_relay.transient, 0, sizeof _relay.transient);
    memset(_relay.preparedId, 0, sizeof _relay.preparedId);
    memset(_relay.recipientStamp, 0, sizeof _relay.recipientStamp);
    memset(_relay.nodeStamp, 0, sizeof _relay.nodeStamp);
    memset(_relay.packetHash, 0, sizeof _relay.packetHash);
}

bool RustLxmfEngine::relayAllowed() const {
    if (_relay.direct) return _accepting;
    if (!_d.propagation || !_d.propagation->settings().enabled) return false;
    const auto& settings = _d.propagation->settings();
    return !_relay.ticket.valid() || _relay.stage == RelayWork::Stage::Select ||
        settings.selection != handheld::propagation::Selection::Manual ||
        (settings.hasManual && !memcmp(settings.manual, _relay.node, 16));
}

bool RustLxmfEngine::beginRelay(Ticket ticket) {
    auto* value = row(ticket);
    if (!value || !(value->flags & PolicyAuto) || value->flags & (RelayRequired | Suppressed) ||
        !_d.propagation || !_d.propagation->settings().enabled) return false;
    releaseBody(ticket);
    if (_relay.ticket == ticket && _relay.direct) {
        _relay.direct = false; _relay.stage = RelayWork::Stage::Select;
        _relay.born = _d.clock->nowMs(); _relay.requestAt = 0;
        _relay.work.reset(); // Completed recipient nonce is retained with its MID.
    }
    value->flags |= RelayRequired;
    value->flags &= ~Rediscover;
    value->receiptMask = 0; // Invalidate queued direct attempts before committing fallback.
    value->phase = Phase::Relay; value->nextAttempt = 0; value->linkSince = 0;
    if (value->proofCount) {
        value->flags |= ProofGrace; value->receiptSince = _d.clock->nowMs();
    }
    setStatus(ticket, LXMFStatus::PROP_QUEUED);
    return true;
}

void RustLxmfEngine::finishRelay(Ticket ticket, LXMFStatus status, bool networkFailure) {
    auto* value = row(ticket);
    if (!value || _relay.ticket != ticket) return;
    const uint64_t now = _d.clock->nowMs();
    if (_d.propagation && (networkFailure || status == LXMFStatus::PROPAGATED))
        _d.propagation->outcome(_relay.node, status == LXMFStatus::PROPAGATED, now);
    value->receiptMask &= ~8;
    value->phase = value->proofCount && (value->flags & ProofGrace) &&
        now - value->receiptSince <= LATE_PROOF_GRACE_MS ? Phase::Grace : Phase::Settled;
    releaseBody(ticket);
    // Set terminal state before cancellation can synchronously report an outcome.
    setStatus(ticket, status);
    if (_d.resources) _d.resources->cancelSend(ticket);
    resetRelay();
}

void RustLxmfEngine::advanceRelay(Ticket ticket) {
    using Stage = RelayWork::Stage;
    auto* value = row(ticket);
    if (!value || !_d.propagation || !_d.propagation->settings().enabled || !_d.links || !_d.resources) return;
    if (_relay.ticket.valid() && _relay.ticket != ticket) return;
    if (!_relay.nodeOwner) {
        if (_d.resources->sending() || _d.links->requestPending()) return;
        _relay.nodeOwner = _d.propagation->claim();
        if (!_relay.nodeOwner) return;
        _relay.born = _d.clock->nowMs();
    }
    if (!_relay.ticket.valid()) {
        if (_d.resources->sending() || _d.links->requestPending()) return;
        _relay.ticket = ticket; _relay.born = _d.clock->nowMs();
    }
    if (!relayAllowed()) return;
    const uint64_t now = _d.clock->nowMs();
    auto requestPath = [&](const uint8_t address[16]) {
        uint8_t tag[16]; RustEntropy::fill(tag, sizeof tag);
        rs_handheld_rns_request_path(_d.ctx, address, tag, 0, now);
    };
    if (_relay.stage == Stage::Select) {
        const auto* node = _d.propagation->select(now, true, _relay.nodeOwner);
        const auto& selected = _d.propagation->settings();
        const auto* manual = selected.selection == handheld::propagation::Selection::Manual && selected.hasManual
            ? _d.propagation->find(selected.manual) : nullptr;
        if (!node && manual && manual->cost > RustStampWork::MaxCost) {
            finishRelay(ticket, LXMFStatus::STAMP_COST_HIGH); return;
        }
        uint8_t cost = 0;
        const bool haveKey = _d.keymap && _d.keymap->recall(value->peer, value->publicKey);
        const bool known = haveKey && _d.keymap->recallCost(value->peer, RustClock::synchronizedEpochSecs(), now, cost);
        if (!node || !known) {
            if (now - _relay.born >= LINK_WAIT_TIMEOUT_MS) {
                finishRelay(ticket, !haveKey ? LXMFStatus::RECIPIENT_UNKNOWN :
                    !known ? LXMFStatus::STAMP_UNKNOWN : LXMFStatus::PROP_UNAVAILABLE, known && !node); return;
            }
            if (now >= _relay.requestAt) {
                if (!known) requestPath(value->peer);
                const auto& settings = _d.propagation->settings();
                if (!node && settings.selection == handheld::propagation::Selection::Manual && settings.hasManual)
                    requestPath(settings.manual);
                _relay.requestAt = now + DISCOVERY_RETRY_MS;
            }
            return;
        }
        if (cost > RustStampWork::MaxCost) { finishRelay(ticket, LXMFStatus::STAMP_COST_HIGH); return; }
        memcpy(_relay.node, node->address, 16); memcpy(_relay.nodeKey, node->publicKey, 64);
        _relay.nodeCost = node->cost; _relay.recipientCost = cost;
        _relay.stage = Stage::Load;
    }
    if (_relay.stage == Stage::Load || _relay.stage == Stage::Reload) {
        if (_relay.stage == Stage::Reload && _d.resources->sending()) return;
        const auto admission = _d.store->requestPrepared(key(*value), _d.ourDestHash);
        if (admission.accepted()) hold(*value, admission, Operation::LoadPrepared);
        return;
    }
    if (_relay.stage == Stage::Read || _relay.stage == Stage::Encrypt) {
        if (value->phase == Phase::Reading) { prepareRelay(ticket); return; }
        if (_body.slot != UINT8_MAX) return;
        const auto admission = _d.store->requestRecord(key(*value));
        if (admission.accepted()) {
            _body.slot = ticket.slot; _body.generation = ticket.generation;
            hold(*value, admission, Operation::ReadRecord); value->phase = Phase::Reading;
        }
        return;
    }
    if (_relay.stage == Stage::RecipientStamp || _relay.stage == Stage::NodeStamp) {
        const bool recipient = _relay.stage == Stage::RecipientStamp;
        if (_relay.work.state() == RustStampWork::State::Idle &&
            !_relay.work.start(recipient ? value->messageId : _relay.transient, recipient ? 0 : 1,
                               recipient ? _relay.recipientCost : _relay.nodeCost, now)) {
            finishRelay(ticket, LXMFStatus::STAMP_FAILED); return;
        }
        if (_relay.work.busy()) return;
        if (_relay.work.state() != RustStampWork::State::Complete) { finishRelay(ticket, LXMFStatus::STAMP_FAILED); return; }
        memcpy(recipient ? _relay.recipientStamp : _relay.nodeStamp, _relay.work.progress().stamp, 32);
        if (recipient) {
            _relay.haveRecipientStamp = true;
            _relay.stampedCost = uint8_t(std::min<uint16_t>(255, _relay.work.progress().value));
            memcpy(_relay.preparedId, value->messageId, 32);
        }
        _relay.work.reset();
        _relay.stage = recipient ? Stage::Encrypt : Stage::Link;
        _relay.born = now; _relay.requestAt = 0;
        return;
    }
    if (_relay.stage == Stage::Link) {
        const auto* node = _d.propagation->find(_relay.node);
        // One bounded PN stamp job per attempt. An authenticated policy rise
        // cannot repeatedly restart CPU work while a packet waits for a Link.
        if (node && node->cost > _relay.nodeCost) {
            finishRelay(ticket, node->cost > RustStampWork::MaxCost ? LXMFStatus::STAMP_COST_HIGH : LXMFStatus::STAMP_FAILED);
            return;
        }
        rs_handheld_route_t route{};
        const bool usable = node && handheld::propagation::Nodes::usable(*node, now) &&
            !memcmp(node->publicKey, _relay.nodeKey, 64) &&
            rs_handheld_rns_route(_d.ctx, _relay.node, now, &route) == RS_HANDHELD_OK &&
            route.kind == RS_HANDHELD_ROUTE_DIRECT && _d.pump->interfaceGeneration(route.interface_id);
        if (!usable) {
            if (now - _relay.born >= LINK_WAIT_TIMEOUT_MS) { finishRelay(ticket, LXMFStatus::PROP_UNAVAILABLE, true); return; }
            if (now >= _relay.requestAt) { requestPath(_relay.node); _relay.requestAt = now + DISCOVERY_RETRY_MS; }
            return;
        }
        _relay.born = now; // Local contention cannot age a future path-discovery window.
        // ensureLink owns the admitted-handshake deadline. Local pool or driver
        // refusal has no peer-failure deadline and consumes no stamp retry.
        const bool active = _d.links->ensureLink(_relay.node, _relay.nodeKey, route);
        if (_relay.ticket != ticket) return; // Failed setup may synchronously retire us.
        if (!active && _d.links->linkEstablishing(_relay.node)) {
            if (!_relay.linkSince) _relay.linkSince = now + 1;
            else if (now >= _relay.linkSince && now - _relay.linkSince > LINK_WAIT_TIMEOUT_MS +
                     _d.pump->interfaceTxWaitMs(route.interface_id, 4)) {
                finishRelay(ticket, LXMFStatus::PROP_UNAVAILABLE, true); return;
            }
        }
        if (!active || _d.resources->sending()) { value->nextAttempt = now + TX_RETRY_MS; return; }
        _relay.linkSince = 0;
        _relay.stage = Stage::Reload;
        return;
    }
    if (_relay.stage == Stage::Await && _relay.packet && now - _relay.sentAt > _relay.waitMs)
        finishRelay(ticket, _relay.emitted ? LXMFStatus::PROP_UNCONFIRMED : LXMFStatus::PROP_UNAVAILABLE, true);
}

void RustLxmfEngine::prepareRelay(Ticket ticket) {
    using Stage = RelayWork::Stage;
    auto* value = row(ticket);
    if (!value || _relay.ticket != ticket || _body.slot != ticket.slot || _body.generation != ticket.generation) return;
    auto& packed = _d.resources->_codec;
    const auto& header = _body.header;
    uint8_t destination[16], messageId[32]; size_t length = 0, entrySize = 0, uploadSize = 0;
    const bool valid = rs_handheld_rns_lxmf_build_link(_d.ctx, value->publicKey, value->timestamp,
        _body.bytes, header.titleLength, _body.bytes + header.titleLength, header.contentLength,
        packed, RS_HANDHELD_RESOURCE_DATA_MAX, &length, destination, messageId) == RS_HANDHELD_OK &&
        !memcmp(destination, value->peer, 16) &&
        (!header.hasMessageId || !memcmp(header.messageId, messageId, 32)) &&
        (!_relay.prepared || !memcmp(_relay.preparedId, messageId, 32)) &&
        (_relay.stage != Stage::Encrypt || !memcmp(value->messageId, messageId, 32));
    releaseBody(ticket); value->phase = Phase::Relay;
    if (!valid) { finishRelay(ticket, LXMFStatus::PROP_INVALID); return; }
    if (rs_handheld_lxmf_relay_size(length, _relay.recipientCost != 0, &entrySize, &uploadSize) != RS_HANDHELD_OK) {
        finishRelay(ticket, LXMFStatus::PROP_TOO_LARGE); return;
    }
    const auto* node = _d.propagation->find(_relay.node);
    if (!node || uploadSize > node->transferBytes) { finishRelay(ticket, LXMFStatus::PROP_TOO_LARGE); return; }
    memcpy(value->messageId, messageId, 32);
    if (_relay.prepared) { _relay.verified = true; _relay.stage = Stage::NodeStamp; return; }
    if (_relay.stage == Stage::Read && _relay.recipientCost &&
        !(_relay.haveRecipientStamp && _relay.stampedCost >= _relay.recipientCost &&
          !memcmp(_relay.preparedId, messageId, 32))) {
        _relay.stage = Stage::RecipientStamp; return;
    }
    if (_relay.recipientCost && rs_handheld_lxmf_append_stamp(packed, length, RS_HANDHELD_RESOURCE_DATA_MAX,
            _relay.recipientStamp, &length) != RS_HANDHELD_OK) { finishRelay(ticket, LXMFStatus::PROP_INVALID); return; }
    uint8_t ephemeral[32], iv[16], transient[32];
    RustEntropy::fill(ephemeral, sizeof ephemeral); RustEntropy::fill(iv, sizeof iv);
    const auto result = rs_handheld_lxmf_relay_encrypt(_d.ctx, value->publicKey,
        RustClock::synchronizedEpochSecs(), _d.clock->nowMs(), ephemeral, iv,
        packed, length, RS_HANDHELD_RESOURCE_DATA_MAX, &length, transient);
    secureZero(ephemeral, sizeof ephemeral); secureZero(iv, sizeof iv);
    if (result != RS_HANDHELD_OK) { finishRelay(ticket, LXMFStatus::PROP_INVALID); return; }
    const auto admission = _d.store->requestPrepare(key(*value), _d.ourDestHash, value->messageId,
        transient, packed, length, _relay.recipientCost);
    if (admission.accepted()) {
        _relay.verified = true; hold(*value, admission, Operation::WritePrepared);
    } else {
        _relay.stage = Stage::Encrypt; value->nextAttempt = _d.clock->nowMs() + TX_RETRY_MS;
    }
}

bool RustLxmfEngine::settleRelayStorage(Ticket ticket) {
    using Stage = RelayWork::Stage;
    namespace prepared = handheld::storage::prepared;
    auto* value = row(ticket);
    if (!value || (value->storageOperation != Operation::LoadPrepared && value->storageOperation != Operation::WritePrepared)) return false;
    handheld::storage::Result result;
    const auto held = storageTicket(*value);
    if (!_d.store->peekResult(held, result)) return true;
    const auto operation = value->storageOperation;
    const bool live = _relay.ticket == ticket && !(value->flags & Suppressed) && value->desired != LXMFStatus::DELIVERED;
    bool valid = result.outcome == Outcome::Committed;
    uint8_t header[prepared::Header], transient[32]; size_t length = 0;
    if (live && valid && result.length) {
        auto& entry = _d.resources->_codec;
        valid = result.length >= prepared::Header + 112 && result.length <= prepared::Max;
        if (valid) {
            length = result.length - prepared::Header;
            valid = _d.store->readPayload(held, header, sizeof header) &&
                _d.store->readPayload(held, entry, length, sizeof header) &&
                prepared::validParts(header, entry, length, key(*value), _d.ourDestHash) &&
                rs_handheld_lxmf_transient_id(entry, length, transient) == RS_HANDHELD_OK &&
                !memcmp(transient, header + 72, 32) && header[106] >= _relay.recipientCost &&
                (!_relay.verified || !memcmp(header + 40, value->messageId, 32));
            if (valid && _relay.prepared) valid = !memcmp(transient, _relay.transient, 32);
        }
    }
    _d.store->releaseResult(held);
    value->storageSequence = 0; value->storageSlot = UINT8_MAX;
    if (!live) return true;
    if (!valid || (!length && (operation == Operation::WritePrepared || _relay.prepared))) {
        value->error = result.error == Error::None ? Error::InvalidRecord : result.error;
        finishRelay(ticket, LXMFStatus::PROP_INVALID); return true;
    }
    value->revision = result.revision;
    if (!length) { _relay.stage = Stage::Read; return true; }
    memcpy(_relay.transient, transient, 32); memcpy(_relay.preparedId, header + 40, 32);
    _relay.prepared = true;
    if (_relay.stage == Stage::Reload) sendRelay(ticket, length);
    else _relay.stage = _relay.verified ? Stage::NodeStamp : Stage::Read;
    return true;
}

void RustLxmfEngine::sendRelay(Ticket ticket, size_t entryLength) {
    auto* value = row(ticket);
    if (!value || _relay.ticket != ticket || !relayAllowed()) return;
    const uint64_t now = _d.clock->nowMs();
    const auto* node = _d.propagation->find(_relay.node);
    rs_handheld_route_t route{};
    uint8_t recipientCost = 0;
    if (!_d.keymap->recallCost(value->peer, RustClock::synchronizedEpochSecs(), now, recipientCost) ||
        recipientCost > _relay.recipientCost) { finishRelay(ticket, LXMFStatus::STAMP_FAILED); return; }
    if (!node || !handheld::propagation::Nodes::usable(*node, now) || node->cost > _relay.nodeCost ||
        rs_handheld_rns_route(_d.ctx, _relay.node, now, &route) != RS_HANDHELD_OK ||
        !_d.links->linkOnRoute(_relay.node, route) || _d.resources->sending()) {
        _relay.stage = RelayWork::Stage::Link; return;
    }
    auto& packed = _d.resources->_codec; size_t length = 0;
    const double timestamp = RustClock::epochSecs() ? double(RustClock::epochSecs()) : double(now) / 1000.0;
    if (rs_handheld_lxmf_relay_upload(packed, entryLength, RS_HANDHELD_RESOURCE_DATA_MAX, timestamp,
            _relay.nodeStamp, &length) != RS_HANDHELD_OK || length > node->transferBytes) {
        finishRelay(ticket, LXMFStatus::PROP_TOO_LARGE); return;
    }
    const uint8_t iface = _d.links->activeLinkIface(_relay.node);
    _relay.stage = RelayWork::Stage::Await; _relay.sentAt = now;
    if (length <= RS_HANDHELD_LINK_MDU) {
        uint8_t raw[500]; size_t rawLength = 0;
        handheld::TxLease lease;
        if (!_d.links->buildLinkDataPacket(_relay.node, packed, length, raw, sizeof raw, rawLength) ||
            !_d.pump->captureLeaseAt(iface, raw, rawLength, now, 120000, lease) ||
            rs_handheld_rns_packet_hash(raw, rawLength, raw[0] & 0x40 ? 1 : 0, _relay.packetHash) != RS_HANDHELD_OK) {
            _relay.stage = RelayWork::Stage::Link; value->nextAttempt = now + TX_RETRY_MS; return;
        }
        _relay.packet = true;
        _relay.waitMs = PROOF_TIMEOUT_MS + proofJitterMs() + _d.pump->interfaceTxWaitMs(iface, 2);
        lease.setReceipt({ticket.generation, uint8_t(ReceiptBase + ticket.slot * 4 + 3)});
        value->receiptMask |= 8; ++value->queuedReceipts;
        const auto offer = _d.pump->offerReceipt(raw, rawLength, lease);
        value = row(ticket);
        if (!value) return;
        if (offer == handheld::TxOffer::Blocked) --value->queuedReceipts;
        if (_relay.ticket != ticket) return;
        if (offer != handheld::TxOffer::Started && offer != handheld::TxOffer::Queued) {
            value->receiptMask &= ~8; _relay.stage = RelayWork::Stage::Link;
            value->nextAttempt = now + TX_RETRY_MS;
        }
    } else {
        const auto* link = _d.links->activeLinkId(_relay.node);
        const auto* session = _d.links->activeLinkKey(_relay.node);
        _relay.packet = false; value->phase = Phase::Resource;
        const bool started = link && session && _d.resources->startSend(ticket, _relay.node, link, session, iface, packed, length);
        value = row(ticket);
        if (value && !started && _relay.ticket == ticket && value->phase == Phase::Resource) {
            value->phase = Phase::Relay; _relay.stage = RelayWork::Stage::Link;
            value->nextAttempt = now + TX_RETRY_MS;
        }
    }
}

bool RustLxmfEngine::directStamp(Ticket ticket, uint8_t cost) {
    auto* value = row(ticket);
    if (!value) return false;
    const uint64_t now = _d.clock->nowMs();
    if (_relay.ticket.valid() && _relay.ticket != ticket) {
        releaseBody(ticket); value->phase = Phase::Ready; value->nextAttempt = now + TX_RETRY_MS; return false;
    }
    if (!_relay.ticket.valid()) {
        _relay.ticket = ticket; _relay.direct = true; _relay.born = now; _relay.recipientCost = cost;
        memcpy(_relay.preparedId, value->messageId, 32);
        if (!_relay.work.start(value->messageId, 0, cost, now)) {
            releaseBody(ticket); value->phase = Phase::Settled; setStatus(ticket, LXMFStatus::STAMP_FAILED); return false;
        }
    }
    if (!_relay.direct || memcmp(_relay.preparedId, value->messageId, 32) || cost > _relay.recipientCost) {
        releaseBody(ticket); value->phase = Phase::Settled; setStatus(ticket, LXMFStatus::STAMP_FAILED); return false;
    }
    if (_relay.work.busy()) {
        releaseBody(ticket); value->phase = Phase::Stamp; return false;
    }
    if (_relay.work.state() != RustStampWork::State::Complete) {
        releaseBody(ticket); value->phase = Phase::Settled; setStatus(ticket, LXMFStatus::STAMP_FAILED); return false;
    }
    memcpy(_relay.recipientStamp, _relay.work.progress().stamp, 32);
    _relay.stampedCost = uint8_t(std::min<uint16_t>(255, _relay.work.progress().value));
    _relay.haveRecipientStamp = true;
    return true;
}
