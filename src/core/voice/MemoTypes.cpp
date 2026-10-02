#include "MemoTypes.h"
namespace handheld::memo {
const char* description(const Status& status) {
    switch(status.reason) {
    case Code::Busy: return "Audio busy";
    case Code::Stale: return "Reopen this voice message";
    case Code::Invalid: return "Voice message unavailable";
    case Code::MicrophoneUnavailable: return "Microphone unavailable";
    case Code::PlaybackUnavailable: return "Playback unavailable";
    case Code::UnsupportedAudio: return "Unsupported audio";
    case Code::AudioUnavailable: return "Audio unavailable";
    case Code::StorageUnavailable: return "Storage unavailable";
    case Code::Interrupted: return "Recording interrupted";
    case Code::NoMemory: return "Audio unavailable";
    case Code::TooShort: return "No audio recorded";
    case Code::Ok: break;
    }
    switch(status.phase) {
    case Phase::Unavailable: return "Voice message unavailable";
    case Phase::Loading: return "Loading...";
    case Phase::Idle: return "Ready to record";
    case Phase::Starting: return "Starting...";
    case Phase::Recording: return "Recording";
    case Phase::Stopping: return "Stopping...";
    case Phase::Saving: return "Saving...";
    case Phase::Review: return status.fromMessage ? "Voice message" : "Saved draft";
    case Phase::Playing: return "Playing";
    case Phase::Sending: return status.fromMessage?"Retrying message...":"Adding to messages...";
    case Phase::Sent: return status.fromMessage?"Retry queued":"Added to messages";
    }
    return "Voice message";
}
}
