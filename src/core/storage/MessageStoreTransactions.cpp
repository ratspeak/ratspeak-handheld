#include "MessageStore.h"
#include "MessageTransactions.h"
#include "runtime/TaskOwner.h"
#include <algorithm>
#include <cmath>

using namespace handheld::storage;

static_assert(sizeof(MessageTransactions) <= 64 + Budget::conversationSummaryBytes(WriteQueue::CompactProfile) &&
              alignof(MessageTransactions) <= alignof(std::max_align_t),
              "Review the inline semantic executor representation");

MessageStore::MessageStore() { new (_transactionState) MessageTransactions(); }
MessageStore::~MessageStore() {
    configASSERT(_writeQueue.stopped() && _writeQueue.drainCount() == 0);
    transactions().~MessageTransactions();
}
MessageTransactions& MessageStore::transactions() const {
    return *std::launder(reinterpret_cast<MessageTransactions*>(_transactionState));
}

MessageStore::Submission MessageStore::submit(Request request, const WriteQueue::PayloadPart* parts,
                                              size_t count, size_t capacity) {
    handheld::assertDeviceOwner();
    if (_deleteFence.valid() && !memcmp(_fencedPeer, request.key.peer, 16))
        return {{}, Rejection::Fenced};
    const auto submission = _writeQueue.submit(request, parts, count, capacity);
    if (submission.accepted() && request.operation == Operation::DeleteConversation) {
        _deleteFence = submission.ticket; memcpy(_fencedPeer, request.key.peer, 16);
    }
    return submission;
}

MessageStore::Submission MessageStore::requestSave(const LXMFMessage& message,
                                                   uint32_t identityGeneration, uint32_t peerGeneration) {
    if (!Budget::validBody(message.title.size(), message.content.size())) return {{}, Rejection::TooLarge};
    if (message.sourceHash.size() != 16 || message.destHash.size() != 16 ||
        (message.messageId.size() != 0 && message.messageId.size() != 32) ||
        !std::isfinite(message.timestamp) || uint8_t(message.status) > 6) return {{}, Rejection::Invalid};
    Request request;
    request.operation = message.incoming ? Operation::CreateIncoming : Operation::CreateOutgoing;
    request.key.incoming = message.incoming;
    memcpy(request.source, message.sourceHash.data(), 16); memcpy(request.destination, message.destHash.data(), 16);
    memcpy(request.key.peer, message.incoming ? request.source : request.destination, 16);
    request.hasMessageId = message.messageId.size() == 32;
    if (request.hasMessageId) memcpy(request.messageId, message.messageId.data(), 32);
    request.timestamp = message.timestamp; request.status = uint8_t(message.status); request.read = message.read;
    request.titleLength = uint16_t(message.title.size()); request.contentLength = uint16_t(message.content.size());
    request.identityGeneration = identityGeneration; request.peerGeneration = peerGeneration;
    return requestSave(request, message.title.data(), message.content.data());
}

MessageStore::Submission MessageStore::requestSave(const Request& request, const void* title, const void* content) {
    if (!Budget::validBody(request.titleLength, request.contentLength)) return {{}, Rejection::TooLarge};
    if ((request.operation != Operation::CreateIncoming && request.operation != Operation::CreateOutgoing) ||
        request.key.counter || request.key.incoming != (request.operation == Operation::CreateIncoming) ||
        memcmp(request.key.peer, request.key.incoming ? request.source : request.destination, 16) ||
        !std::isfinite(request.timestamp) || request.status > 6 ||
        (request.titleLength && !title) || (request.contentLength && !content)) return {{}, Rejection::Invalid};
    const WriteQueue::PayloadPart parts[] = {{title, request.titleLength}, {content, request.contentLength}};
    return submit(request, parts, 2);
}

MessageStore::Submission MessageStore::requestStatus(const RecordKey& key, LXMFStatus status,
                                                     uint32_t identityGeneration, uint32_t peerGeneration) {
    if (!key.counter || uint8_t(status) > 6) return {{}, Rejection::Invalid};
    Request request; request.operation = Operation::UpdateStatus; request.key = key; request.status = uint8_t(status);
    request.identityGeneration = identityGeneration; request.peerGeneration = peerGeneration;
    return submit(request);
}

MessageStore::Submission MessageStore::requestMarkRead(const std::string& peer,
                                                      uint32_t identityGeneration, uint32_t peerGeneration) {
    Request request; request.operation = Operation::MarkRead;
    if (!decodeHex(peer.data(), peer.size(), request.key.peer, 16)) return {{}, Rejection::Invalid};
    request.identityGeneration = identityGeneration; request.peerGeneration = peerGeneration;
    return submit(request);
}

MessageStore::Submission MessageStore::requestDelete(const std::string& peer,
                                                    uint32_t identityGeneration, uint32_t peerGeneration) {
    Request request; request.operation = Operation::DeleteConversation;
    if (!decodeHex(peer.data(), peer.size(), request.key.peer, 16)) return {{}, Rejection::Invalid};
    request.identityGeneration = identityGeneration; request.peerGeneration = peerGeneration;
    return submit(request);
}

MessageStore::Submission MessageStore::requestRecord(const RecordKey& key, uint32_t offset, uint16_t capacity) {
    if (!key.counter || capacity < sizeof(StoredRecordHeader)) return {{}, Rejection::Invalid};
    Request request; request.operation = Operation::ReadRecord; request.key = key; request.offset = offset;
    return submit(request, nullptr, 0, capacity);
}

MessageStore::Submission MessageStore::requestPending(const RecordKey& after) {
    if (after.incoming) return {{}, Rejection::Invalid};
    Request request; request.operation = Operation::ReadPending; request.key = after;
    return submit(request, nullptr, 0, sizeof(StoredRecordHeader));
}

MessageStore::Submission MessageStore::requestHistoryPage(const std::string& peer, HistoryEntry cursor, uint8_t limit,
                                                         HistoryDirection direction) {
    Request request; request.operation = Operation::ReadHistoryPage;
    request.key.counter = cursor.counter; request.key.incoming = cursor.incoming;
    request.historyDirection = direction;
    if ((direction != HistoryDirection::Before && direction != HistoryDirection::After) ||
        !limit || limit > 48 || !decodeHex(peer.data(), peer.size(), request.key.peer, 16))
        return {{}, Rejection::Invalid};
    return submit(request, nullptr, 0, size_t(limit) * sizeof(HistoryEntry));
}

MessageStore::Submission MessageStore::requestTrim(const std::string& peer) {
    Request request; request.operation = Operation::Trim;
    if (!decodeHex(peer.data(), peer.size(), request.key.peer, 16)) return {{}, Rejection::Invalid};
    return submit(request);
}

MessageStore::Submission MessageStore::requestConversationPage(ConversationCursor cursor, bool hasCursor,
    ConversationOrder order, ConversationDirection direction, uint8_t limit) {
    if (!limit || limit > 64 || !validConversationOrder(order) || !validConversationDirection(direction) ||
        !std::isfinite(cursor.timestamp)) return {{}, Rejection::Invalid};
    Request request; request.operation = Operation::ReadConversationPage;
    request.timestamp = cursor.timestamp; memcpy(request.key.peer, cursor.peer, 16);
    request.hasConversationCursor = hasCursor; request.conversationOrder = order;
    request.conversationDirection = direction;
    return submit(request, nullptr, 0, size_t(limit) * sizeof(ConversationSelector));
}

MessageStore::Submission MessageStore::requestConversation(const ConversationSelector& selector) {
    if (!validConversationSelector(selector) || !selector.counter || selector.error != Error::None)
        return {{}, Rejection::Invalid};
    Request request; request.operation = Operation::ReadConversation;
    request.timestamp = selector.cursor.timestamp; memcpy(request.key.peer, selector.cursor.peer, 16);
    request.key.counter = selector.counter; request.key.incoming = selector.incoming;
    return submit(request, nullptr, 0, sizeof(ConversationView));
}

void MessageStore::poll() {
    handheld::assertDeviceOwner();
    for (;;) {
        const Ticket ticket = _writeQueue.nextReady(_settledThrough);
        if (!ticket.valid()) break;
        Request request; Result result;
        if (!_writeQueue.peekResult(ticket, result, &request)) break;
        settle(request, result);
        // Settlement never removes the consumer's result or terminal credit.
        _settledThrough = ticket.sequence;
        if (_deleteFence == ticket) _deleteFence = {};
    }
}

bool MessageStore::peekResult(Ticket ticket, Result& result, Request* request) const {
    return ticket.sequence <= _settledThrough && _writeQueue.peekResult(ticket, result, request);
}
bool MessageStore::readPayload(Ticket ticket, void* output, size_t length, size_t offset) const {
    return ticket.sequence <= _settledThrough && _writeQueue.readPayload(ticket, output, length, offset);
}
bool MessageStore::releaseResult(Ticket ticket) {
    return ticket.sequence <= _settledThrough && _writeQueue.releaseResult(ticket);
}
bool MessageStore::finishStop() {
    poll();
    if (!_writeQueue.finishStop()) return false;
    clearWorker(); return true;
}
bool MessageStore::setExternalStorageEnabled(bool enabled) {
    handheld::assertDeviceOwner();
    if (_writeQueue.drainCount() != 0) return false;
    _externalStorageEnabled = enabled; transactions().setExternal(enabled); return true;
}

void MessageStore::settle(const Request& request, const Result& result) noexcept {
    if (result.outcome != Outcome::Committed) return;
    if (request.operation == Operation::ReadRecord || request.operation == Operation::ReadHistoryPage ||
        request.operation == Operation::ReadConversationPage || request.operation == Operation::ReadConversation ||
        request.operation == Operation::ReadPending) return;
    if (result.duplicate) return;
    // Only committed aggregate deltas and invalidation live here. There is no
    // presentation cache or allocation between persistence and result visibility.
    if ((request.operation == Operation::CreateIncoming || request.operation == Operation::CreateOutgoing ||
         request.operation == Operation::DeleteConversation || request.operation == Operation::Trim) &&
        _historyRevision < UINT32_MAX) ++_historyRevision;
    if (result.conversationDelta > 0 && _totalConversations < UINT32_MAX) ++_totalConversations;
    if (result.conversationDelta < 0 && _totalConversations) --_totalConversations;
    _totalUnread = result.unreadDelta > 0 && result.unreadDelta > INT_MAX - _totalUnread ? INT_MAX :
        std::max(0, _totalUnread + result.unreadDelta);
    bumpRevision();
}

bool MessageStore::consumeImmediate(Submission submission, Result& result) {
    if (!submission.accepted()) return false;
    poll();
    if (!peekResult(submission.ticket, result)) return false;
    const bool committed = result.outcome == Outcome::Committed && result.error == Error::None;
    releaseResult(submission.ticket); return committed;
}

bool MessageStore::saveMessage(LXMFMessage& message) {
#if STORAGE_DEFERRED_IO
    (void)message;
    Serial.println("[MSGSTORE] Deferred save requires requestSave/ticket settlement");
    return false;
#else
    Result result;
    if (!consumeImmediate(requestSave(message), result)) return false;
    message.savedCounter = result.key.counter; return true;
#endif
}

bool MessageStore::deleteConversation(const std::string& peer) {
#if STORAGE_DEFERRED_IO
    (void)peer; Serial.println("[MSGSTORE] Deferred delete requires requestDelete/ticket settlement"); return false;
#else
    Result result; return consumeImmediate(requestDelete(peer), result);
#endif
}

bool MessageStore::markConversationRead(const std::string& peer) {
#if STORAGE_DEFERRED_IO
    (void)peer; Serial.println("[MSGSTORE] Deferred read marker requires requestMarkRead/ticket settlement"); return false;
#else
    Result result; return consumeImmediate(requestMarkRead(peer), result);
#endif
}

bool MessageStore::updateMessageStatusByCounter(const std::string& peer, uint32_t counter,
                                               bool incoming, LXMFStatus status) {
#if STORAGE_DEFERRED_IO
    (void)peer; (void)counter; (void)incoming; (void)status;
    Serial.println("[MSGSTORE] Deferred status requires requestStatus/ticket settlement"); return false;
#else
    RecordKey key; key.counter = counter; key.incoming = incoming;
    if (!decodeHex(peer.data(), peer.size(), key.peer, 16)) return false;
    Result result; return consumeImmediate(requestStatus(key, status), result);
#endif
}
