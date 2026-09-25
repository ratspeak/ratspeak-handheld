#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "transport/TxLease.h"

// One bounded exact-packet cache utility. Runtime
// response ownership lives in ProtocolRuntime's per-interface slots, including fresh followups
// while a replay is blocked. The Rust tag cache is bounded and can evict earlier duplicates.
// The original queue lifetime travels with these bytes; replay may bind a new output
// interface, but cannot renew the packet's age.
class RustPathResponseCache {
public:
    static constexpr size_t MAX_PACKET = 500;
    static constexpr uint64_t TAG_WINDOW_MS = 30000;

    void clear() {
        memset(_tag, 0, sizeof(_tag));
        memset(_raw, 0, sizeof(_raw));
        _tagLen = 0;
        _rawLen = 0;
        _storedMs = 0;
        _lease = {};
    }

    bool store(const uint8_t* tag, size_t tagLen, const uint8_t* raw, size_t rawLen,
               uint64_t nowMs, const handheld::TxLease& lease) {
        if (!tag || tagLen == 0 || tagLen > sizeof(_tag) || !raw || rawLen == 0 ||
            rawLen > sizeof(_raw) || !lease.generation) {
            return false;
        }
        clear();
        memcpy(_tag, tag, tagLen);
        memcpy(_raw, raw, rawLen);
        _tagLen = tagLen;
        _rawLen = rawLen;
        _storedMs = nowMs;
        _lease = lease;
        return true;
    }

    bool recall(const uint8_t* tag, size_t tagLen, uint64_t nowMs, const uint8_t*& outRaw,
                size_t& outRawLen, const handheld::TxLease*& outLease) const {
        outRaw = nullptr;
        outRawLen = 0;
        outLease = nullptr;
        if (!tag || tagLen == 0 || tagLen != _tagLen || _rawLen == 0 || nowMs < _storedMs ||
            nowMs - _storedMs > TAG_WINDOW_MS || memcmp(tag, _tag, tagLen) != 0) {
            return false;
        }
        outRaw = _raw;
        outRawLen = _rawLen;
        outLease = &_lease;
        return true;
    }

private:
    uint8_t _tag[16] = {};
    uint8_t _raw[MAX_PACKET] = {};
    size_t _tagLen = 0;
    size_t _rawLen = 0;
    uint64_t _storedMs = 0;
    handheld::TxLease _lease;
};
static_assert(sizeof(RustPathResponseCache) <= 688, "Review path-response cache budget");

