#include "WriteQueue.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <algorithm>
#include <cstring>
#include <new>

using namespace handheld::storage;

void WriteQueue::assertOwner() const {
    configASSERT(!_owner || _owner == xTaskGetCurrentTaskHandle());
}

WriteQueue::~WriteQueue() {
    // Shutdown is explicit and cooperative. Never kill another task or spin
    // here while its File/slot ownership may still be live.
    configASSERT(stopped() && _outstanding == 0);
    if (_queue) vQueueDelete(static_cast<QueueHandle_t>(_queue));
}

bool WriteQueue::begin(Executor& executor, Execution execution) {
    assertOwner();
    if (execution > Execution::Deferred) return false;
    if (!finishStop()) return false;
    _owner = xTaskGetCurrentTaskHandle();
    _executor = &executor;
    _execution = execution;
    _workerStackFree.store(UINT32_MAX, std::memory_order_relaxed);
    _stopRequested = false;
    if (execution == Execution::Deferred) {
        // Independent sentinel credit: shutdown does not wait for the owner to
        // release completed slots, even when all request slots are occupied.
        _queue = xQueueCreate(Budget::SlotCount + 1, sizeof(uint8_t));
        if (!_queue) return false;
        _workerStopped.store(false, std::memory_order_release);
        TaskHandle_t task = nullptr;
        if (xTaskCreatePinnedToCore(taskFunc, "WriteQ", Budget::WorkerStack, this,
                                   1, &task, 1) != pdPASS) {
            _workerStopped.store(true, std::memory_order_release);
            vQueueDelete(static_cast<QueueHandle_t>(_queue)); _queue = nullptr; _task = nullptr;
            return false;
        }
        _task = task;
    }
    _accepting = true;
    return true;
}

bool WriteQueue::adoptOwner() {
    if (_outstanding || (_stopRequested && !stopped())) return false;
    _owner = xTaskGetCurrentTaskHandle();
    return true;
}

uint8_t* WriteQueue::payload(uint8_t slot) {
    if (slot >= Budget::NormalSlots) return nullptr;
    const size_t offset = CompactProfile ? slot * Budget::SmallPayload : slot * Budget::LargePayload;
    return _payload + offset;
}
const uint8_t* WriteQueue::payload(uint8_t slot) const {
    return const_cast<WriteQueue*>(this)->payload(slot);
}

WriteQueue::Submission WriteQueue::submit(const Request& request, const PayloadPart* parts,
                                         size_t partCount, size_t resultCapacity) {
    assertOwner();
    auto reject = [](Rejection reason) { return Submission{{}, reason}; };
    if (!_accepting) return reject(Rejection::Unavailable);
    if (_nextSequence == UINT64_MAX) return reject(Rejection::Exhausted);
    if (request.operation > Operation::MemoCancel || partCount > 3 || (partCount && !parts))
        return reject(Rejection::Invalid);
    size_t length = 0;
    for (size_t i = 0; i < partCount; ++i) {
        if (parts[i].length && !parts[i].data) return reject(Rejection::Invalid);
        if (parts[i].length > Budget::LargePayload - length) return reject(Rejection::TooLarge);
        length += parts[i].length;
    }
    if (resultCapacity > Budget::LargePayload) return reject(Rejection::TooLarge);
    if (request.operation == Operation::CreateIncoming || request.operation == Operation::CreateOutgoing) {
        if (!Budget::validBody(request.titleLength, request.contentLength) ||
            request.audio.length > Budget::MaxMessageBody - request.titleLength - request.contentLength)
            return reject(Rejection::TooLarge);
        if (length != size_t(request.titleLength) + request.contentLength + request.audio.length)
            return reject(Rejection::Invalid);
    }
    size_t first = 0, end = Budget::NormalSlots;
    if (request.operation == Operation::DeleteConversation) {
        first = Budget::NormalSlots; end = first + 1;
    } else if (request.operation == Operation::UpdateStatus || request.operation == Operation::MarkRead) {
        first = Budget::NormalSlots + 1; end = first + 1;
    }
    if (first >= Budget::NormalSlots && (length || resultCapacity)) return reject(Rejection::Invalid);
    const size_t needed = std::max(length, resultCapacity);
    uint8_t index = first;
    while (index < end && (_slots[index].state.load(std::memory_order_acquire) != State::Free ||
                          capacity(index) < needed)) ++index;
    if (index == end) return reject(Rejection::Busy);
    auto& slot = _slots[index];
    slot.request = request;
    slot.request.readCapacity = static_cast<uint16_t>(resultCapacity);
    slot.result = {};
    slot.sequence = ++_nextSequence;
    _lengths[index] = static_cast<uint16_t>(length);
    _resultCapacities[index] = static_cast<uint16_t>(resultCapacity);
    uint8_t* destination = payload(index);
    if (capacity(index)) std::memset(destination, 0, capacity(index));
    for (size_t i = 0; i < partCount; ++i) {
        if (parts[i].length) {
            std::memcpy(destination, parts[i].data, parts[i].length);
            destination += parts[i].length;
        }
    }
    ++_outstanding; // reserve before worker publication/preemption
    slot.state.store(State::Queued, std::memory_order_release);
    if (_execution == Execution::Deferred) {
        if (xQueueSend(static_cast<QueueHandle_t>(_queue), &index, 0) != pdTRUE) {
            if (capacity(index)) std::memset(payload(index), 0, capacity(index));
            _lengths[index] = 0; _resultCapacities[index] = 0;
            slot.state.store(State::Free, std::memory_order_release);
            --_outstanding;
            return reject(Rejection::Busy);
        }
    } else executeSlot(index);
    return Submission{{slot.sequence, index}, Rejection::None};
}

void WriteQueue::executeSlot(uint8_t index) {
    if (index >= Budget::SlotCount) return;
    auto& slot = _slots[index];
    State expected = State::Queued;
    if (!slot.state.compare_exchange_strong(expected, State::Running, std::memory_order_acq_rel)) {
        if (expected != State::CancelQueued) return;
        slot.result = {};
        slot.result.key = slot.request.key;
        slot.result.outcome = Outcome::Cancelled;
        slot.result.error = Error::Cancelled;
    } else {
        Result result;
        result.key = slot.request.key;
        try {
            _executor->execute(slot.request, payload(index), _lengths[index], capacity(index), result);
        } catch (const std::bad_alloc&) {
            // A cleanup/mirror allocation failure cannot erase an already
            // committed transaction or its owner-visible deltas.
            result.error = Error::Allocation;
        } catch (...) {
            result.error = Error::Internal;
        }
        if (result.length > _resultCapacities[index]) {
            const auto key = result.key;
            result = {}; result.key = key; result.error = Error::InvalidRecord;
        }
        _lengths[index] = std::max(_lengths[index], result.length);
        slot.result = result;
    }
    slot.state.store(State::Ready, std::memory_order_release);
}

void WriteQueue::taskFunc(void* argument) {
    auto* self = static_cast<WriteQueue*>(argument);
    for (;;) {
        self->_workerStackFree.store(uxTaskGetStackHighWaterMark(nullptr), std::memory_order_release);
        uint8_t index;
        if (xQueueReceive(static_cast<QueueHandle_t>(self->_queue), &index, portMAX_DELAY) != pdTRUE) continue;
        if (index == StopSlot) {
            self->_task = nullptr;
            self->_workerStopped.store(true, std::memory_order_release);
            vTaskDelete(nullptr); // this task only; no access to self afterward
            return;
        }
        self->executeSlot(index);
    }
}

bool WriteQueue::matches(Ticket ticket) const {
    return ticket.valid() && ticket.slot < Budget::SlotCount &&
           _slots[ticket.slot].sequence == ticket.sequence;
}
bool WriteQueue::peekResult(Ticket ticket, Result& result, Request* request) const {
    assertOwner();
    if (!matches(ticket) || _slots[ticket.slot].state.load(std::memory_order_acquire) != State::Ready)
        return false;
    result = _slots[ticket.slot].result;
    if (request) *request = _slots[ticket.slot].request;
    return true;
}
bool WriteQueue::readPayload(Ticket ticket, void* destination, size_t length, size_t offset) const {
    assertOwner();
    if (!matches(ticket) || _slots[ticket.slot].state.load(std::memory_order_acquire) != State::Ready ||
        offset > _lengths[ticket.slot] || length > _lengths[ticket.slot] - offset ||
        (length && !destination)) return false;
    if (length) std::memcpy(destination, payload(ticket.slot) + offset, length);
    return true;
}
bool WriteQueue::releaseResult(Ticket ticket) {
    assertOwner();
    if (!matches(ticket) || _slots[ticket.slot].state.load(std::memory_order_acquire) != State::Ready)
        return false;
    if (capacity(ticket.slot)) std::memset(payload(ticket.slot), 0, capacity(ticket.slot));
    _lengths[ticket.slot] = 0; _resultCapacities[ticket.slot] = 0;
    _slots[ticket.slot].state.store(State::Free, std::memory_order_release);
    --_outstanding;
    return true;
}
bool WriteQueue::cancel(Ticket ticket) {
    assertOwner();
    if (!matches(ticket)) return false;
    State queued = State::Queued;
    return _slots[ticket.slot].state.compare_exchange_strong(queued, State::CancelQueued,
                                                            std::memory_order_acq_rel);
}
WriteQueue::Ticket WriteQueue::nextReady(uint64_t afterSequence) const {
    assertOwner();
    Ticket first;
    for (uint8_t index = 0; index < Budget::SlotCount; ++index) {
        const auto& slot = _slots[index];
        // Select the oldest outstanding ticket before checking readiness. A
        // concurrent worker can complete an earlier slot after we scanned it;
        // scanning only Ready slots could expose a later completion first.
        const State state = slot.state.load(std::memory_order_acquire);
        if (state != State::Free && state != State::Retired && slot.sequence > afterSequence &&
            (!first.valid() || slot.sequence < first.sequence)) first = {slot.sequence, index};
    }
    if (first.valid() && _slots[first.slot].state.load(std::memory_order_acquire) != State::Ready)
        return {};
    return first;
}
void WriteQueue::requestStop() {
    assertOwner();
    _accepting = false;
    if (_stopRequested) return;
    _stopRequested = true;
    if (_execution == Execution::Deferred && _queue && !stopped()) {
        uint8_t stop = StopSlot;
        const bool sent = xQueueSend(static_cast<QueueHandle_t>(_queue), &stop, 0) == pdTRUE;
        configASSERT(sent); // queue reserves an independent sentinel credit
    }
}
bool WriteQueue::finishStop() {
    assertOwner();
    if (_accepting || !stopped() || _outstanding) return false;
    if (_queue) { vQueueDelete(static_cast<QueueHandle_t>(_queue)); _queue = nullptr; }
    _executor = nullptr; _task = nullptr;
    return true;
}
bool WriteQueue::isFull() const {
    assertOwner();
    if (!_accepting) return true;
    for (uint8_t i = 0; i < Budget::NormalSlots; ++i)
        if (_slots[i].state.load(std::memory_order_acquire) == State::Free) return false;
    return true;
}
size_t WriteQueue::retainedBytes() const {
    assertOwner();
    size_t retained = 0;
    for (uint8_t i = 0; i < Budget::SlotCount; ++i)
        if (_slots[i].state.load(std::memory_order_acquire) != State::Free) retained += capacity(i);
    return retained;
}
