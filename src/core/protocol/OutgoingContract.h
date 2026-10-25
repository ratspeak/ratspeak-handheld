#pragma once

#include "storage/StorageContract.h"
#include "reticulum/LXMFMessage.h"
#include <cstddef>
#include <cstdint>

namespace handheld::outgoing {
struct Ticket {
    uint32_t generation = 0;
    uint8_t slot = UINT8_MAX;
    constexpr bool valid() const { return generation != 0 && slot < 20; }
};
constexpr bool operator==(Ticket a, Ticket b) { return a.generation == b.generation && a.slot == b.slot; }
constexpr bool operator!=(Ticket a, Ticket b) { return !(a == b); }
enum class Rejection : uint8_t {
    None, Busy, Invalid, TooLarge, NoMemory, Unavailable, Fenced, Exhausted, Recovering, Stopped
};
struct Submission {
    Ticket ticket;
    Rejection rejection = Rejection::Unavailable;
    constexpr bool accepted() const { return rejection == Rejection::None && ticket.valid(); }
};
enum class Poll : uint8_t { Invalid, Pending, Ready };
// Explicit rejection and local cancellation must not become network fallback.
enum class ResourceOutcome : uint8_t { Delivered, NetworkFailure, Rejected, Cancelled };
// Immutable until acknowledge(). Committed remains authoritative when error
// describes an optional mirror failure or cancellation suppressed network TX.
struct InitialResult {
    storage::Outcome outcome = storage::Outcome::Failed;
    storage::Error error = storage::Error::None;
    storage::RecordKey key;
    uint32_t revision = 0;
    bool txSuppressed = false;
};
struct StatusView {
    LXMFStatus desired = LXMFStatus::QUEUED, durable = LXMFStatus::QUEUED;
    storage::Error error = storage::Error::None;
    bool pending = false, txSuppressed = false;
};
} // namespace handheld::outgoing
