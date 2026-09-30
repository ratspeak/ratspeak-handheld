
#include "protocol/RustLxmfEngine.h"
#include "protocol/RustClock.h"
#include "protocol/RustEntropy.h"
#include "protocol/RustKeyMap.h"
#include "protocol/RustInterfacePump.h"
#include "protocol/RustWire.h"
#include "protocol/RustLinkManager.h"
#include "protocol/RustResourceEngine.h"
#include "storage/MessageStore.h"
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
    request.status = uint8_t(LXMFStatus::QUEUED);
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
    if (status == LXMFStatus::DELIVERED) {
        value->receiptMask = 0; value->phase = Phase::Settled; releaseBody(ticket);
        if (_d.resources) _d.resources->cancelSend(ticket);
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
    ++_statusRevision;
    return true;
}
bool RustLxmfEngine::beginPeerDelete(const uint8_t peer[16]) {
    if (!_accepting || _deleting || !peer) return false;
    _deleting = true; memcpy(_deletingPeer, peer, 16);
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
    if (_deleting) return false;
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
            value->phase = value->flags & Suppressed ? Phase::Settled : Phase::Ready;
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
                    // Relay records require their dedicated preparation owner.
                    // Never feed them to direct recovery, including AUTO after fallback.
                    if (header.deliveryPolicy == handheld::messaging::DeliveryPolicy::Always ||
                        handheld::messaging::relayStatus(header.status)) {
                        value->desired = value->durable;
                        value->flags |= Suppressed; value->phase = Phase::Settled;
                        ++_statusRevision; return;
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
            value->phase = Phase::Ready; value->nextAttempt = _d.clock->nowMs() + TX_RETRY_MS;
        } else if (validHeader && (header.deliveryPolicy == handheld::messaging::DeliveryPolicy::Always ||
                                  handheld::messaging::relayStatus(header.status))) {
            releaseBody(ticket); value->flags |= Suppressed; value->phase = Phase::Settled;
            value->desired = value->durable = LXMFStatus(header.status); value->revision = header.revision;
        } else if (foreignIdentity) {
            // A record replaced/restored from another identity is preserved.
            // It cannot be transmitted or rewritten as this active author.
            releaseBody(ticket); value->error = Error::InvalidRecord;
            value->flags |= Suppressed | BlockedRecord; value->phase = Phase::Settled;
            value->receiptMask = 0; value->proofCount = 0; value->desired = value->durable;
        } else if (!validHeader) {
            releaseBody(ticket); value->error = Error::InvalidRecord; value->phase = Phase::Settled;
            value->flags |= Suppressed | BlockedRecord;
            setStatus(ticket, LXMFStatus::FAILED);
        } else {
            value->revision = result.revision; value->timestamp = header.timestamp;
            value->error = Error::None; value->phase = Phase::Reading;
        }
    }
    ++_statusRevision;
}

bool RustLxmfEngine::receiptHook(void* context, handheld::TxReceipt receipt, handheld::TxReceiptEvent event) {
    auto& owner = *static_cast<RustLxmfEngine*>(context);
    if (receipt.slot < RustIncomingDelivery::ReceiptCount)
        return RustIncomingDelivery::receiptHook(&owner._incoming, receipt, event);
    if (receipt.slot < ReceiptBase || receipt.slot >= ReceiptBase + RowCount * 4) return false;
    const Ticket ticket{receipt.generation, uint8_t((receipt.slot - ReceiptBase) / 4)};
    const uint8_t variant = (receipt.slot - ReceiptBase) % 4;
    auto* value = owner.row(ticket);
    if (!value) return false;
    if (event == handheld::TxReceiptEvent::Validate) return owner._accepting &&
        value->identityGeneration == owner._identityGeneration && !(value->flags & (Suppressed | Deleted)) &&
        value->desired != LXMFStatus::DELIVERED && (value->receiptMask & (1u << variant));
    if (value->queuedReceipts) --value->queuedReceipts;
    if (event == handheld::TxReceiptEvent::Started) owner.setStatus(ticket, LXMFStatus::SENT);
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
    const auto* active = _d.links->activeLinkId(value->peer);
    uint8_t slot = UINT8_MAX; uint32_t generation = 0;
    if (!active || _d.links->activeLinkIface(value->peer) != iface || memcmp(active, linkId, 16) ||
        !_d.links->receiptBinding(iface, linkId, slot, generation)) return 0;
    return (uint64_t(generation) << 8) | slot;
}

void RustLxmfEngine::finishRouteFailure(Ticket ticket) {
    auto* value = row(ticket);
    if (!value) return;
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
            value->phase = Phase::Settled; setStatus(ticket, LXMFStatus::FAILED);
        }
        return;
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
        if (!value->linkSince) value->linkSince = now;
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
                value->phase = Phase::Grace; value->flags |= ProofGrace;
                value->receiptSince = now; setStatus(ticket, LXMFStatus::UNCONFIRMED);
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
