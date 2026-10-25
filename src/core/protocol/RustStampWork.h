#pragma once

#include <Arduino.h>
#include "RustEntropy.h"
#include "ratspeak_protocol.h"
#include "runtime/ResourceBudget.h"

// One job on the protocol owner, with no message body or storage lease retained.
// The elapsed slice is checked between bounded Rust operations; target latency of
// a single HKDF round must still be qualified on each release profile.
class RustStampWork {
public:
    enum class State : uint8_t { Idle, Preparing, Searching, Complete, Exhausted,
                                 Cancelled, TimedOut, Failed };
    static constexpr uint8_t MaxCost = 20;
    static constexpr uint64_t DeadlineMs = 30000;
    static constexpr uint32_t SliceUs = 1000;
    RustStampWork() = default;
    ~RustStampWork() { release(); }
    RustStampWork(const RustStampWork&) = delete;
    RustStampWork& operator=(const RustStampWork&) = delete;

    bool start(const uint8_t material[32], uint8_t kind, uint8_t cost, uint64_t now) {
        if (_job || !material || kind > 1 || cost > MaxCost) return false;
        uint8_t seed[32]; RustEntropy::fill(seed, sizeof(seed));
        rs_handheld_stamp_t* job = nullptr;
        const auto result = rs_handheld_stamp_create(material, kind, cost, seed,
                                                     uint64_t{8} << cost, &job);
        if (result != RS_HANDHELD_OK) return false;
        _job = job; _born = now; _progress = {}; _maxSliceUs = 0;
        _state = State::Preparing;
        advance(0, 0); // Cost zero is a stable immediate result.
        return _state != State::Failed;
    }
    void poll(uint64_t now) {
        if (!_job) return;
        if (now < _born || now - _born >= DeadlineMs) {
            _state = State::TimedOut; release(); return;
        }
        const uint32_t began = micros();
        unsigned rounds = 0;
        for (unsigned calls = 0; _job && calls < 64; ++calls) {
            const bool preparing = _state == State::Preparing;
            if (preparing && rounds++ == 4) break;
            advance(preparing ? 1 : 0, preparing ? 0 : 16);
            if (uint32_t(micros() - began) >= SliceUs) break;
        }
        const uint32_t elapsed = uint32_t(micros() - began);
        if (elapsed > _maxSliceUs) _maxSliceUs = elapsed;
    }
    void cancel() {
        if (!_job) return; // Completed and failed results remain stable.
        rs_handheld_stamp_cancel(_job);
        advance(0, 0);
    }
    void reset() { release(); _progress = {}; _state = State::Idle; _maxSliceUs = 0; }
    bool busy() const { return _job != nullptr; }
    State state() const { return _state; }
    const rs_handheld_stamp_progress_t& progress() const { return _progress; }
    uint32_t maxSliceUs() const { return _maxSliceUs; }
private:
    void release() { rs_handheld_stamp_destroy(_job); _job = nullptr; }
    void advance(uint16_t rounds, uint32_t attempts) {
        if (rs_handheld_stamp_step(_job, rounds, attempts, &_progress) != RS_HANDHELD_OK) {
            _state = State::Failed;
        } else {
            switch (_progress.state) {
                case 0: _state = State::Preparing; return;
                case 1: _state = State::Searching; return;
                case 2: _state = State::Complete; break;
                case 3: _state = State::Exhausted; break;
                case 4: _state = State::Cancelled; break;
                default: _state = State::Failed; break;
            }
        }
        release();
    }
    rs_handheld_stamp_t* _job = nullptr;
    rs_handheld_stamp_progress_t _progress{};
    uint64_t _born = 0;
    uint32_t _maxSliceUs = 0;
    State _state = State::Idle;
};
static_assert(sizeof(RustStampWork) <= handheld::ResourceBudget::StampOwner,
              "Stamp scheduler exceeds its retained budget");
