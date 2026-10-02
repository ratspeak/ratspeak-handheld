#pragma once

#include "StorageContract.h"

namespace handheld::storage::memo {

constexpr uint8_t Mode = 0x03;
constexpr size_t FrameBytes = 4, FrameMs = 40;
constexpr size_t RecordBytes = 1500, PlaybackBytes = 3000, ChunkBytes = 256;

enum class State : uint8_t { Empty, Recording, Ready, Promoting, Sent };

// Returned before a bounded audio slice. Revision is an opaque durable CAS
// token, independent of UI/view generations. An interrupted working copy never
// replaces the saved copy; only explicit Seal can make it Ready.
struct Snapshot {
    uint32_t revision = 0, workingRevision = 0, outboxCounter = 0;
    uint16_t length = 0, workingLength = 0;
    State state = State::Empty;
    messaging::DeliveryPolicy policy = messaging::DeliveryPolicy::DirectOnly;
    bool interrupted = false;
};
static_assert(sizeof(Snapshot) <= 20, "Memo result metadata budget changed");

inline bool operation(Operation op) {
    return op >= Operation::MemoRead && op <= Operation::MemoCancel;
}

// Read: revision zero inspects the current draft, otherwise exact revision.
// Begin/Clear/Promote: revision must match the saved snapshot. Begin also binds
// offset to the last observed workingRevision, so stale starts cannot replace
// a newer capture session.
// Append/Seal/Cancel: revision must match the working snapshot. Append's offset
// is the exact number of bytes already committed, making retries idempotent.
// Read returns saved audio at offset, never an incomplete working recording.
// Promote freezes timestamp/policy on its first successful durable reservation;
// retries ignore later changes to those parameters and reuse its outbox counter.
struct Command {
    uint8_t local[16] = {}, peer[16] = {};
    uint32_t revision = 0, offset = 0;
    double timestamp = 0;
    messaging::DeliveryPolicy policy = messaging::DeliveryPolicy::DirectOnly;
};

} // namespace handheld::storage::memo
