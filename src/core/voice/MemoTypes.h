#pragma once
#include <cstdint>
#include <type_traits>

namespace handheld::memo {
enum class Phase : uint8_t { Unavailable, Loading, Idle, Starting, Recording, Stopping, Saving, Review, Playing, Sending, Sent, Closed, Pausing, Paused };
enum class Code : uint8_t { Ok, Busy, Stale, Invalid, MicrophoneUnavailable, PlaybackUnavailable,
    UnsupportedAudio, AudioUnavailable, StorageUnavailable, Interrupted, NoMemory, TooShort,
    Recovered, DeviceBusy, InputLost, CaptureOverflow, AudioRemoved };
enum class Action : uint8_t { Open, Record, Stop, Play, Send, Replace, Discard, Close, Volume, Retry, EndConversation, Pause };
struct Command {
    uint32_t view = 0, generation = 0, draftRevision = 0, stopEpoch = 0, counter = 0;
    uint8_t peer[16]{};
    Action action = Action::Close;
    uint8_t volume = 70;
    bool incoming = false;
    // Explicit foreground intent, carried with Open to avoid a second UI round
    // trip before playing a stored message. Epoch still cancels a queued start.
    bool playOnOpen = false;
};
static_assert(sizeof(Command) == 40, "Memo command IPC budget changed");
struct Status {
    uint32_t generation = 0, view = 0, revision = 0, draftRevision = 0, counter = 0;
    uint32_t frames = 0, stackFree = 0, encodeUs = 0, decodeUs = 0;
    uint8_t peer[16]{};
    uint16_t length = 0;
    Phase phase = Phase::Unavailable;
    Code reason = Code::Ok;
    uint8_t capabilities = 0, volume = 70;
    bool fromMessage = false, incoming = false, retryable = false;
    // Numeric diagnostics only, no identity/audio content. High bit distinguishes
    // admission rejection from a completed storage Error; step is Controller::Work.
    uint8_t storageStep = 0, storageError = 0, audioError = 0;
};
static_assert(sizeof(Status) <= 64 && std::is_trivially_copyable<Status>::value, "Memo status IPC budget changed");
constexpr bool busy(Phase phase) {
    return phase == Phase::Loading || phase == Phase::Starting || phase == Phase::Recording ||
        phase == Phase::Stopping || phase == Phase::Saving || phase == Phase::Playing || phase == Phase::Sending || phase == Phase::Pausing;
}
const char* description(const Status&);
} // namespace handheld::memo
