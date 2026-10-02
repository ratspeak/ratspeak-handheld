#pragma once
#include <cstdint>

// Append only: these values are persisted. Relay acceptance is not delivery.
enum class LXMFStatus : uint8_t {
    DRAFT = 0, QUEUED, SENDING, SENT, DELIVERED, FAILED, UNCONFIRMED,
    PROP_QUEUED, PROP_SENDING, PROPAGATED, PROP_UNAVAILABLE, PROP_UNCONFIRMED,
    RECIPIENT_UNKNOWN, STAMP_UNKNOWN, STAMP_COST_HIGH, STAMP_FAILED,
    PROP_TOO_LARGE, PROP_REJECTED, PROP_INVALID
};

namespace handheld::messaging {
// Frozen when a message is created. Settings changes cannot reinterpret it.
enum class DeliveryPolicy : uint8_t { DirectOnly = 0, Auto = 1, Always = 2 };
constexpr uint8_t LastStatus = uint8_t(LXMFStatus::PROP_INVALID);
constexpr bool pendingStatus(uint8_t value) { return value == 1 || value == 2 || value == 7 || value == 8; }
constexpr bool failedStatus(uint8_t value) { return value == 5 || value == 10 || (value >= 12 && value <= LastStatus); }
constexpr bool retryableStatus(uint8_t value) { return failedStatus(value) || value == 6 || value == 11; }
constexpr bool relayStatus(uint8_t value) { return (value >= 7 && value <= 11) || (value >= 16 && value <= LastStatus); }
constexpr bool validDelivery(uint8_t status, DeliveryPolicy policy, bool incoming) {
    if (status > LastStatus || policy > DeliveryPolicy::Always) return false;
    if (incoming) return policy == DeliveryPolicy::DirectOnly && status <= 6;
    if (relayStatus(status)) return policy != DeliveryPolicy::DirectOnly;
    // Failure/cancellation and recipient proof remain permissible in ALWAYS.
    return policy != DeliveryPolicy::Always || status == 4 || status == 5 || (status >= 12 && status <= 15);
}
}
