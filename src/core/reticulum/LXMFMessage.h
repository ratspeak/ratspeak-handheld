#pragma once

#include <stdint.h>

#include <string>

#include "util/Bytes.h"
#include "storage/StorageContract.h"
#include "LXMFStatus.h"

// Application and storage record; wire encoding lives in the Rust protocol library.
// Preserve field meanings and the on-disk JSON schema when changing this type.
// Append new values: existing numeric statuses are persisted on disk.

struct LXMFMessage {
    rs::Bytes sourceHash;
    rs::Bytes destHash;
    double timestamp = 0;
    std::string content;
    std::string title;
    rs::Bytes signature;

    LXMFStatus status = LXMFStatus::DRAFT;
    handheld::messaging::DeliveryPolicy deliveryPolicy = handheld::messaging::DeliveryPolicy::DirectOnly;
    bool incoming = false;
    bool read = false;
    int retries = 0;
    unsigned long lastRetryMs = 0;
    uint32_t savedCounter = 0;
    uint32_t receiveCounter = 0;  // Monotonic receive order (used by Ratcom)
    rs::Bytes messageId;

    // Transient view metadata only. Storage/wire codecs never serialize these
    // fields; status above presents the desired fact while persistence retries.
    LXMFStatus durableStatus = LXMFStatus::DRAFT;
    handheld::storage::Error statusError = handheld::storage::Error::None;
    bool statusPending = false;
    bool txSuppressed = false;

    const char* statusStr() const {
        switch (status) {
            case LXMFStatus::DRAFT: return "DRAFT";
            case LXMFStatus::QUEUED: return "QUEUED";
            case LXMFStatus::SENDING: return "SENDING";
            case LXMFStatus::SENT: return "SENT";
            case LXMFStatus::DELIVERED: return "DELIVERED";
            case LXMFStatus::FAILED: return "FAILED";
            case LXMFStatus::UNCONFIRMED: return "UNCONFIRMED";
            case LXMFStatus::PROP_QUEUED: return "PROP QUEUED";
            case LXMFStatus::PROP_SENDING: return "PROP SENDING";
            case LXMFStatus::PROPAGATED: return "PROPAGATED";
            case LXMFStatus::PROP_UNAVAILABLE: return "PROP UNAVAILABLE";
            case LXMFStatus::PROP_UNCONFIRMED: return "PROP UNCONFIRMED";
            case LXMFStatus::RECIPIENT_UNKNOWN: return "RECIPIENT UNKNOWN";
            case LXMFStatus::STAMP_UNKNOWN: return "STAMP UNKNOWN";
            case LXMFStatus::STAMP_COST_HIGH: return "STAMP COST HIGH";
            case LXMFStatus::STAMP_FAILED: return "STAMP FAILED";
            case LXMFStatus::PROP_TOO_LARGE: return "PROP TOO LARGE";
            case LXMFStatus::PROP_REJECTED: return "PROP REJECTED";
            case LXMFStatus::PROP_INVALID: return "PROP INVALID";
        }
        return "?";
    }
};
