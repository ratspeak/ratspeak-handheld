#include "voice/VoiceTypes.h"
namespace handheld::voice {
const char* description(const Status& s) {
    if (s.phase == Phase::Ended) {
        switch (s.reason) {
            case Code::Local: return "Voice ended";
            case Code::Remote: return "Peer ended voice";
            case Code::Rejected: return "Voice declined";
            case Code::Busy: return "Device busy";
            case Code::Timeout: return "No answer";
            case Code::ProfileUnsupported: return "Voice profile unsupported";
            case Code::AudioUnavailable: return "Audio unavailable";
            case Code::RouteLost: return "Connection lost";
            case Code::SlowRoute: return "Connection too slow for live voice";
            case Code::Off: return "Voice is off";
            case Code::RfUnsupported: return "RF setting cannot carry live voice";
            case Code::ChannelBusy: return "Voice channel busy";
            case Code::RoutePolicyUnavailable: return "Selected voice connection unavailable";
            case Code::SlowCodec: return "Voice processing too slow";
            case Code::NoMemory: return "Not enough memory for voice";
            case Code::IdentityUnknown: return "Peer identity unavailable";
            case Code::NotContact: return "Incoming voice: contacts only";
            default: return "Voice unavailable";
        }
    }
    switch (s.phase) {
        case Phase::Unavailable: return "Voice audio not available on this device";
        case Phase::Off: return "Voice is off";
        case Phase::Idle: return "Voice ready";
        case Phase::Finding: return "Finding voice route...";
        case Phase::Calling: return "Requesting voice...";
        case Phase::Incoming: return "Voice request";
        case Phase::Connecting: return "Connecting...";
        case Phase::Ready: return "Connected";
        case Phase::Talking: return "Talking";
        case Phase::Receiving: return "Incoming audio";
        case Phase::Ending: return "Ending voice...";
        default: return "Voice unavailable";
    }
}
}
