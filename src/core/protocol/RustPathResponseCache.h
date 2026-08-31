#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// One bounded exact-packet cache for Python-compatible tagged path-response replay. Today's
// transport tag gate suppresses duplicate tags before C++, so one slot is sufficient and costs
// 680 bytes of BSS on the no-PSRAM board. If multi-path later allows concurrent duplicate tags,
// this slot already preserves the required same-tag byte identity and ratchet context flag.
class RustPathResponseCache {
public:
    static constexpr size_t MAX_PACKET = 640;
    static constexpr uint64_t TAG_WINDOW_MS = 30000;

    void clear() {
        memset(_tag, 0, sizeof(_tag));
        memset(_raw, 0, sizeof(_raw));
        _tagLen = 0;
        _rawLen = 0;
        _storedMs = 0;
    }

    bool store(const uint8_t* tag, size_t tagLen, const uint8_t* raw, size_t rawLen,
               uint64_t nowMs) {
        if (!tag || tagLen == 0 || tagLen > sizeof(_tag) || !raw || rawLen == 0 ||
            rawLen > sizeof(_raw)) {
            return false;
        }
        clear();
        memcpy(_tag, tag, tagLen);
        memcpy(_raw, raw, rawLen);
        _tagLen = tagLen;
        _rawLen = rawLen;
        _storedMs = nowMs;
        return true;
    }

    bool recall(const uint8_t* tag, size_t tagLen, uint64_t nowMs, const uint8_t*& outRaw,
                size_t& outRawLen) const {
        outRaw = nullptr;
        outRawLen = 0;
        if (!tag || tagLen == 0 || tagLen != _tagLen || _rawLen == 0 || nowMs < _storedMs ||
            nowMs - _storedMs > TAG_WINDOW_MS || memcmp(tag, _tag, tagLen) != 0) {
            return false;
        }
        outRaw = _raw;
        outRawLen = _rawLen;
        return true;
    }

private:
    uint8_t _tag[16] = {};
    uint8_t _raw[MAX_PACKET] = {};
    size_t _tagLen = 0;
    size_t _rawLen = 0;
    uint64_t _storedMs = 0;
};


