#pragma once

#include <stdint.h>

#include <string>

#include "util/Bytes.h"

// Application and storage record; wire encoding lives in the Rust protocol library.
// Preserve field meanings and the on-disk JSON schema when changing this type.
enum class LXMFStatus : uint8_t { DRAFT = 0, QUEUED, SENDING, SENT, DELIVERED, FAILED };

struct LXMFMessage {
    rs::Bytes sourceHash;
    rs::Bytes destHash;
    double timestamp = 0;
    std::string content;
    std::string title;
    rs::Bytes signature;

    LXMFStatus status = LXMFStatus::DRAFT;
    bool incoming = false;
    bool read = false;
    int retries = 0;
    unsigned long lastRetryMs = 0;
    uint32_t savedCounter = 0;
    uint32_t receiveCounter = 0;  // Monotonic receive order (used by Ratcom)
    rs::Bytes messageId;

    const char* statusStr() const {
        switch (status) {
            case LXMFStatus::DRAFT: return "DRAFT";
            case LXMFStatus::QUEUED: return "QUEUED";
            case LXMFStatus::SENDING: return "SENDING";
            case LXMFStatus::SENT: return "SENT";
            case LXMFStatus::DELIVERED: return "DELIVERED";
            case LXMFStatus::FAILED: return "FAILED";
        }
        return "?";
    }
};
