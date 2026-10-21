#pragma once

#include <stdint.h>
#include <string>
#include "ratspeak_protocol.h"
#include "protocol/RustLinkManager.h"

class RustClock;
class RustInterfacePump;

namespace handheld {

// One-shot NomadNet page fetch (D1L Peers-menu feature): resolve a saved peer's
// nomadnetwork.node destination directly from their already-known public key (no private
// key, no live nomadnetwork.node announce needed -- see protocol/src/nomad.rs), establish
// a Link, optionally identify to the node (off by default -- see start()'s `identify`
// param), fetch one page, surface the raw decoded text. Deliberately
// NOT Micron-rendered yet (see ratspeak-handheld TODO.md's "Nomad browser from Peers
// menu" -- raw text first, on purpose, to prove this plumbing works before investing in a
// markup parser). One browse in flight at a time, sharing the same RustLinkManager request
// slot LXMF propagation's startGet() uses -- never concurrent with a propagation sync.
class NomadBrowseClient : public RustLinkManager::RequestSink {
public:
    enum class Status : uint8_t { Idle, ResolvingPath, Connecting, Requesting, Done, Failed };

    struct Deps {
        rs_handheld_rns_t* ctx = nullptr;
        RustClock* clock = nullptr;
        RustInterfacePump* pump = nullptr;
        RustLinkManager* links = nullptr;
    };
    void begin(const Deps& deps) { _d = deps; }

    // publicKey: 64 bytes, from an already-saved DiscoveredNode. path: e.g.
    // "/page/index.mu". requestId: the mailbox request id this browse answers for --
    // settlement is reported tagged with this id, so a caller can tell a late poll of an
    // old request apart from the current one. fieldData/fieldDataLen: OPTIONAL,
    // ALREADY-msgpack-encoded form field data (a fixmap of name->value strings) --
    // NULL/0 for a plain page fetch. identify: whether to send a Link-Identify frame
    // before the request -- off by default. Every browse tears down and re-establishes
    // a fresh Link (see loop()), so this isn't a one-time cost amortized across a
    // session: it's an extra full LoRa transmission on every single page load, for a
    // server identity most nodes never even read (see TODO.md). Copied into this
    // object (not just referenced), since it must survive for the lifetime of the
    // browse, including retries.
    bool start(const uint8_t publicKey[64], const char* path, uint32_t requestId,
               const uint8_t* fieldData = nullptr, size_t fieldDataLen = 0, bool identify = false);
    void cancel();
    void loop();

    Status status() const { return _status; }
    uint32_t requestId() const { return _requestId; }
    bool settled() const { return _status == Status::Done || _status == Status::Failed; }
    // The real nomadnetwork.node destination being browsed (derived from the peer's public
    // key, NOT their lxmf.delivery hash -- see start()). Valid as soon as start() returns
    // true; all-zero before the first successful start().
    const uint8_t* destHash() const { return _destHash; }
    // Copies up to `capacity` bytes of the fetched page text (truncated, never
    // null-terminated by this call); returns the actual page length.
    size_t pageText(char* out, size_t capacity) const;
    const char* failureReason() const { return _failureReason; }
    // 1-based. A busy mesh can lose either half of a 2-piece radio-level split
    // response with no retransmission at that layer (confirmed on real hardware: a
    // Link request/response rides one Reticulum packet, so losing one LoRa PHY frame
    // loses the whole response) -- one silent retry covers the common case cheaply.
    static constexpr uint8_t kMaxAttempts = 3;
    uint8_t attempt() const { return _attempt; }

private:
    void onLinkResponse(const uint8_t* value, size_t length) override;
    void onLinkRequestFailed(RustLinkManager::RequestError) override;
    void fail(const char* reason);
    // Attempt 1 only gets kFirstAttemptTimeoutMs, not the full
    // kRetryTimeoutMs -- real-hardware testing found the ordinary-RF-
    // collision pattern (an unrelated nearby transmission's split-sequence
    // nibble colliding with this attempt's deterministic first-try timing)
    // consistently hits the FIRST attempt specifically; subsequent attempts
    // keep the full 30s, since that value is itself empirically grounded
    // (meshpoint's own NomadNet browser found 20s too short for a real
    // round trip on a slow LoRa link with split-frame reassembly on both
    // legs) -- shortening every attempt would risk needlessly abandoning a
    // genuinely slow but otherwise-working link's very first try.
    uint32_t requestTimeoutMs() const { return _attempt == 1 ? kFirstAttemptTimeoutMs : kRetryTimeoutMs; }
    // Back to 15000 after real-hardware testing of 5000 and 10000 both felt
    // worse -- this is the value that actually worked, see this constant's
    // use in requestTimeoutMs()'s doc comment above for why it isn't lower.
    static constexpr uint32_t kFirstAttemptTimeoutMs = 15000;
    static constexpr uint32_t kRetryTimeoutMs = 30000;

    Deps _d;
    Status _status = Status::Idle;
    uint32_t _requestId = 0;
    uint8_t _destHash[16] = {};
    uint8_t _publicKey[64] = {};
    char _path[RS_HANDHELD_NOMAD_PATH_MAX + 1] = {};
    uint8_t _pathLen = 0;
    uint8_t _fieldData[RS_HANDHELD_NOMAD_FIELD_DATA_MAX] = {};
    uint16_t _fieldDataLen = 0;
    bool _identify = false;
    uint64_t _requestAt = 0;
    uint64_t _stepSince = 0;
    uint8_t _attempt = 1;
    std::string _pageText;
    const char* _failureReason = nullptr;
};

} // namespace handheld
