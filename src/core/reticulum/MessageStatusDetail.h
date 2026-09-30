#pragma once

#include "LXMFMessage.h"

// Shared phase wording; these labels describe the existing owner status facts.
inline const char* messageStatusLabel(LXMFStatus status) {
    switch (status) {
        case LXMFStatus::QUEUED: return "queued";
        case LXMFStatus::SENDING: return "sending";
        case LXMFStatus::SENT: return "sent";
        case LXMFStatus::DELIVERED: return "delivered";
        case LXMFStatus::FAILED: return "failed";
        case LXMFStatus::UNCONFIRMED: return "unconfirmed";
        case LXMFStatus::PROP_QUEUED: return "prop queued";
        case LXMFStatus::PROP_SENDING: return "prop sending";
        case LXMFStatus::PROPAGATED: return "propagated";
        case LXMFStatus::PROP_UNAVAILABLE: return "prop unavailable";
        case LXMFStatus::PROP_UNCONFIRMED: return "prop unconfirmed";
        case LXMFStatus::RECIPIENT_UNKNOWN: return "recipient unknown";
        case LXMFStatus::STAMP_UNKNOWN: return "stamp unknown";
        case LXMFStatus::STAMP_COST_HIGH: return "stamp cost high";
        case LXMFStatus::STAMP_FAILED: return "stamp failed";
        case LXMFStatus::PROP_TOO_LARGE: return "prop too large";
        case LXMFStatus::PROP_REJECTED: return "prop rejected";
        case LXMFStatus::PROP_INVALID: return "prop invalid";
        default: return "draft";
    }
}

// Shared transient caption; persisted/wire status values remain unchanged.
inline const char* messageStatusDetail(LXMFStatus status, bool pending,
        handheld::storage::Error error, bool suppressed) {
    if (pending)
        return error == handheld::storage::Error::None ? "saving status" : "save retry";
    if (suppressed && status != LXMFStatus::DELIVERED) return "not sending";
    if (error != handheld::storage::Error::None) return "storage error";
    return nullptr;
}
inline const char* messageStatusDetail(const LXMFMessage& message) {
    return messageStatusDetail(message.status, message.statusPending, message.statusError, message.txSuppressed);
}
