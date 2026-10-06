#include "MessageStore.h"
#include "MessageTransactions.h"
#include "PreparedEnvelope.h"
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
    if (!rrc::operation(request.operation) && _deleteFence.valid() && !memcmp(_fencedPeer, request.key.peer, 16))
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
        !std::isfinite(message.timestamp) ||
        !handheld::messaging::validDelivery(uint8_t(message.status), message.deliveryPolicy, message.incoming))
        return {{}, Rejection::Invalid};
    Request request;
    request.operation = message.incoming ? Operation::CreateIncoming : Operation::CreateOutgoing;
    request.key.incoming = message.incoming;
    memcpy(request.source, message.sourceHash.data(), 16); memcpy(request.destination, message.destHash.data(), 16);
    memcpy(request.key.peer, message.incoming ? request.source : request.destination, 16);
    request.hasMessageId = message.messageId.size() == 32;
    if (request.hasMessageId) memcpy(request.messageId, message.messageId.data(), 32);
    request.timestamp = message.timestamp; request.status = uint8_t(message.status); request.read = message.read;
    request.deliveryPolicy = message.deliveryPolicy;
    request.titleLength = uint16_t(message.title.size()); request.contentLength = uint16_t(message.content.size());
    request.identityGeneration = identityGeneration; request.peerGeneration = peerGeneration;
    return requestSave(request, message.title.data(), message.content.data());
}

MessageStore::Submission MessageStore::requestSave(const Request& request, const void* title, const void* content,
                                                  const void* media) {
    if (!audio::validBody(request.titleLength, request.contentLength, request.audio)) return {{}, Rejection::TooLarge};
    if ((request.operation != Operation::CreateIncoming && request.operation != Operation::CreateOutgoing) ||
        request.key.counter || request.key.incoming != (request.operation == Operation::CreateIncoming) ||
        memcmp(request.key.peer, request.key.incoming ? request.source : request.destination, 16) ||
        !std::isfinite(request.timestamp) ||
        !handheld::messaging::validDelivery(request.status, request.deliveryPolicy, request.key.incoming) ||
        (request.titleLength && !title) || (request.contentLength && !content) ||
        (request.audio.length && !media) || request.audio.checksum) return {{}, Rejection::Invalid};
    const WriteQueue::PayloadPart parts[] = {{title, request.titleLength}, {content, request.contentLength},
                                           {media, request.audio.length}};
    return submit(request, parts, 3);
}

MessageStore::Submission MessageStore::requestStatus(const RecordKey& key, LXMFStatus status,
                                                     uint32_t identityGeneration, uint32_t peerGeneration) {
    if (!key.counter || uint8_t(status) > handheld::messaging::LastStatus) return {{}, Rejection::Invalid};
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

MessageStore::Submission MessageStore::requestRetry(const RecordKey& key,const uint8_t local[16],uint32_t revision) {
    if(!key.counter || key.incoming || !local || !revision) return {{},Rejection::Invalid};
    Request request;request.operation=Operation::RetryOutgoing;request.key=key;request.offset=revision;
    memcpy(request.source,local,16);return submit(request);
}

MessageStore::Submission MessageStore::requestDeleteRecord(const RecordKey& key,const uint8_t local[16],uint32_t revision) {
    if(!key.counter || !revision || !local) return {{},Rejection::Invalid};
    Request request;request.operation=Operation::DeleteRecord;request.key=key;request.offset=revision;
    memcpy(request.source,local,16);return submit(request);
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

MessageStore::Submission MessageStore::requestAudio(const RecordKey& key, uint32_t offset, uint16_t capacity) {
    if (!key.counter || capacity <= sizeof(StoredRecordHeader)) return {{}, Rejection::Invalid};
    Request request; request.operation = Operation::ReadAudio; request.key = key; request.offset = offset;
    return submit(request, nullptr, 0, capacity);
}

MessageStore::Submission MessageStore::requestMemo(Operation op, const memo::Command& command,
    const uint8_t* chunk, size_t length, uint16_t capacity) {
    if (!memo::operation(op) || length > memo::ChunkBytes || length % memo::FrameBytes ||
        (length && (!chunk || op != Operation::MemoAppend)) ||
        (op == Operation::MemoAppend && !length) || capacity < sizeof(memo::Snapshot) ||
        !std::isfinite(command.timestamp) || command.policy > handheld::messaging::DeliveryPolicy::Always)
        return {{}, Rejection::Invalid};
    Request request; request.operation = op;
    memcpy(request.source, command.local, 16); memcpy(request.key.peer, command.peer, 16);
    request.offset = command.revision; request.peerGeneration = command.offset;
    request.timestamp = command.timestamp; request.deliveryPolicy = command.policy;
    const WriteQueue::PayloadPart part{chunk, length};
    // Promotion reuses the queue's existing large credit to construct the
    // message body; no permanent full-clip buffer is added to any owner.
    return submit(request, &part, 1, op == Operation::MemoPromote ? Budget::LargePayload : capacity);
}

MessageStore::Submission MessageStore::requestPending(const RecordKey& after) {
    if (after.incoming) return {{}, Rejection::Invalid};
    Request request; request.operation = Operation::ReadPending; request.key = after;
    return submit(request, nullptr, 0, sizeof(StoredRecordHeader));
}

MessageStore::Submission MessageStore::requestPrepared(const RecordKey& key, const uint8_t source[16]) {
    if (!key.counter || key.incoming || !source) return {{}, Rejection::Invalid};
    Request request; request.operation = Operation::LoadPrepared; request.key = key;
    memcpy(request.source, source, 16);
    return submit(request, nullptr, 0, prepared::Max);
}

MessageStore::Submission MessageStore::requestPrepare(const RecordKey& key, const uint8_t source[16],
    const uint8_t messageId[32], const uint8_t transientId[32], const uint8_t* entry, size_t length,
    uint8_t recipientCost) {
    uint8_t header[prepared::Header];
    if (!prepared::make(header, key, source, messageId, transientId, entry, length, recipientCost)) return {{}, Rejection::Invalid};
    Request request; request.operation = Operation::WritePrepared; request.key = key;
    memcpy(request.source, source, 16); memcpy(request.messageId, messageId, 32); request.hasMessageId = true;
    const WriteQueue::PayloadPart parts[] = {{header, sizeof(header)}, {entry, length}};
    return submit(request, parts, 2, prepared::Max);
}

MessageStore::Submission MessageStore::requestPurgeJournal(const uint8_t local[16]) {
    if (!local) return {{}, Rejection::Invalid};
    Request request; request.operation = Operation::LoadPurge;
    memcpy(request.destination, local, 16);
    return submit(request, nullptr, 0, purge::Size);
}

MessageStore::Submission MessageStore::requestRrc(Operation operation, const rrc::Record& record,
    const uint8_t fileKey[16], uint32_t cursor, uint32_t expectedRevision, HistoryDirection direction,
    const uint8_t privateParticipant[16], const uint8_t observation[16]) {
    if (!rrc::operation(operation) || !record.valid(record.length()) ||
        (direction != HistoryDirection::Before && direction != HistoryDirection::After) ||
        ((operation == Operation::RrcAppend || operation == Operation::RrcStatus ||
          (operation == Operation::RrcRead && record.kind() == rrc::Kind::Message && !cursor)) && !fileKey))
        return {{}, Rejection::Invalid};
    Request request; request.operation = operation; request.key.counter = cursor;
    if (privateParticipant) {
        if (operation!=Operation::RrcAppend && (operation!=Operation::RrcWrite || record.kind()!=rrc::Kind::Draft))
            return {{},Rejection::Invalid};
        memcpy(request.source,privateParticipant,16);
    }
    if (observation) {
        if (operation!=Operation::RrcAppend && operation!=Operation::RrcObserve) return {{},Rejection::Invalid};
        memcpy(request.destination,observation,16);
    }
    request.offset = expectedRevision;
    request.hasMessageId = fileKey != nullptr; request.historyDirection = direction;
    if (fileKey) memcpy(request.messageId, fileKey, 16);
    const WriteQueue::PayloadPart part{record.bytes, record.length()};
    return submit(request, &part, 1, Budget::SmallPayload);
}

MessageStore::Submission MessageStore::requestWritePurge(const purge::Journal& journal) {
    if (!journal.valid()) return {{}, Rejection::Invalid};
    Request request; request.operation = Operation::WritePurge; request.key = journal.key();
    memcpy(request.destination, journal.local(), 16);
    const WriteQueue::PayloadPart part{journal.bytes, sizeof(journal.bytes)};
    return submit(request, &part, 1, purge::Size);
}

MessageStore::Submission MessageStore::requestClearPurge(const purge::Journal& journal) {
    if (!journal.valid()) return {{}, Rejection::Invalid};
    Request request; request.operation = Operation::ClearPurge; request.key = journal.key();
    memcpy(request.destination, journal.local(), 16);
    const WriteQueue::PayloadPart part{journal.bytes, sizeof(journal.bytes)};
    return submit(request, &part, 1, purge::Size);
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
    if (rrc::operation(request.operation)) return; // RRC owner publishes its own revision/unread.
    if (memo::operation(request.operation) && request.operation != Operation::MemoPromote) return;
    if (request.operation == Operation::ReadRecord || request.operation == Operation::ReadAudio || request.operation == Operation::ReadHistoryPage ||
        request.operation == Operation::ReadConversationPage || request.operation == Operation::ReadConversation ||
        request.operation == Operation::ReadPending || request.operation == Operation::LoadPurge ||
        request.operation == Operation::WritePurge || request.operation == Operation::ClearPurge) return;
    if (result.duplicate && !result.enriched) return;
    // Only committed aggregate deltas and invalidation live here. There is no
    // presentation cache or allocation between persistence and result visibility.
    if ((request.operation == Operation::CreateIncoming || request.operation == Operation::CreateOutgoing ||
         request.operation == Operation::DeleteConversation || request.operation == Operation::DeleteRecord || request.operation == Operation::Trim ||
         request.operation == Operation::MemoPromote) &&
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
