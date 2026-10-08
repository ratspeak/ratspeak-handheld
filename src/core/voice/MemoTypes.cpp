#include "MemoTypes.h"
namespace handheld::memo {
const char* description(const Status& status) {
    // Stored messages and unsent previews both play a sealed clip. Failed
    // capture has no sealed length after the controller retires its work.
    const bool playback=status.fromMessage || status.length;
    switch(status.reason) {
    case Code::Busy: return "Audio busy";
    case Code::Stale: return "Reopen this voice message";
    case Code::Invalid: return "Voice message unavailable";
    case Code::MicrophoneUnavailable: return "Microphone unavailable";
    case Code::PlaybackUnavailable: return "Playback unavailable";
    case Code::AudioRemoved: return "Audio removed";
    case Code::UnsupportedAudio: return "Unsupported audio";
    case Code::AudioUnavailable: return "Audio unavailable";
    case Code::StorageUnavailable: return "Storage unavailable";
    case Code::Interrupted: return playback?"Playback interrupted":"Recording interrupted";
    case Code::NoMemory: return "Not enough memory";
    case Code::TooShort: return "No audio recorded";
    case Code::DeviceBusy: return "Audio processing too slow";
    case Code::InputLost: return playback?"Playback stopped":"Recording stopped";
    case Code::CaptureOverflow: return "Recording buffer full";
    case Code::Recovered: break;
    case Code::Ok: break;
    }
    switch(status.phase) {
    case Phase::Closed: return "Conversation closed";
    case Phase::Unavailable: return "Voice message unavailable";
    case Phase::Loading: return "Loading...";
    case Phase::Idle: return "Ready to record";
    case Phase::Starting: return "Starting...";
    case Phase::Recording: return "Recording";
    case Phase::Stopping: return "Stopping...";
    case Phase::Saving: return "Saving...";
    case Phase::Review: return status.fromMessage ? "Voice message" : "Voice clip";
    case Phase::Playing: return "Playing";
    case Phase::Pausing: return "Pausing...";
    case Phase::Paused: return "Paused";
    case Phase::Sending: return status.fromMessage?"Retrying message...":"Sending...";
    case Phase::Sent: return status.fromMessage?"Retry queued":"Queued";
    }
    return "Voice message";
}
}
