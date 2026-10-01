#pragma once
#include <cstddef>
#include <cstdint>

namespace handheld::voice {
enum class Route : uint8_t { Auto, IpOnly, LoRaOnly };
struct Settings {
    bool enabled = false;
    bool contactsOnly = true;
    uint8_t volume = 70;
    Route route = Route::Auto;
};
enum class Phase : uint8_t { Unavailable, Off, Idle, Finding, Calling, Incoming, Connecting, Ready, Talking, Receiving, Ending, Ended };
enum class Code : uint8_t { Ok, Local, Remote, Rejected, Busy, Timeout, ProfileUnsupported, AudioUnavailable, RouteLost,
                           SlowRoute, NoMemory, IdentityUnknown, NotContact, Invalid, Off, SlowCodec };
enum class Action : uint8_t { Start, Accept, Decline, End, TalkDown, TalkUp, Volume };
struct Command {
    uint32_t generation = 0;
    uint32_t view = 0;
    uint32_t stopEpoch = 0;
    Action action = Action::End;
    uint8_t volume = 70;
    uint8_t peer[16]{}; // Start: ordinary LXMF delivery destination with a known public identity.
};
struct Status {
    uint32_t generation = 0, elapsedMs = 0, rxDrops = 0, txDrops = 0;
    uint32_t stackFree = 0, encodeUs = 0, decodeUs = 0; // last worker observations, retained after teardown
    uint8_t peer[16]{};
    Phase phase = Phase::Unavailable;
    Code reason = Code::Ok;
    uint8_t profile = 0, iface = UINT8_MAX, capabilities = 0, volume = 70;
    bool incoming = false, verified = false;
};
inline bool active(Phase p) { return p >= Phase::Finding && p <= Phase::Ending; }
const char* description(const Status&);

struct Encoded {
    uint64_t bornMs = 0;
    uint32_t generation = 0;
    uint16_t length = 0;
    uint8_t bytes[81]{};
};
struct AudioStatus {
    uint32_t generation = 0, dropped = 0, peak = 0, stackFree = 0, encodeUs = 0, decodeUs = 0;
    Code error = Code::Ok;
    bool ready = false, receiving = false, capturing = false;
};
class AudioPort {
public:
    virtual ~AudioPort() = default;
    virtual uint32_t cancellationEpoch() const { return 0; }
    virtual uint8_t capabilities() const = 0; // capture=1, playback=2; board facts, not available memory
    virtual Code prepare(uint32_t generation, uint8_t profile, uint8_t volume) = 0;
    virtual void stop() = 0;
    virtual bool drained() = 0;
    virtual AudioStatus status() const = 0;
    virtual void capture(bool pressed, uint32_t stopEpoch = 0) = 0;
    virtual void flush() = 0;
    virtual void volume(uint8_t value) = 0;
    virtual bool receive(const Encoded&) = 0;
    virtual bool take(Encoded&) = 0;
};
}
