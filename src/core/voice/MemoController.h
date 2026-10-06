#pragma once
#include "MemoTypes.h"
#include "VoiceTypes.h"
#include "storage/MemoDraft.h"

class MessageStore;
namespace handheld::memo {
// Protocol/Service owner only. Storage and audio workers retain their own
// bounded credits. No filesystem, codec call or PCM buffer lives in this owner.
class Controller {
public:
    struct Deps {
        MessageStore* store = nullptr;
        voice::AudioPort* audio = nullptr;
        void* context = nullptr;
        void (*sent)(void*, const storage::RecordKey&) = nullptr;
        storage::Submission (*retry)(void*,const storage::RecordKey&,uint32_t) = nullptr;
    };
    void begin(const Deps&, const uint8_t local[16], uint8_t volume);
    Code command(const Command&, storage::memo::Command sendContext = {});
    void poll(uint64_t now);
    void stop();
    void configureVolume(uint8_t);
    bool drained() const;
    const Status& status() const { return _status; }
private:
    enum class Work : uint8_t { None, Inspect, Begin, Append, Seal, Cancel, Promote, Clear, Message, Clip, Retry, ClearReplace, Fresh, Expire };
    void phase(Phase, Code = Code::Ok);
    void review(Code = Code::Ok);
    void submit();
    void settle();
    void audio();
    void fail(Code);
    storage::RecordKey key() const;
    storage::memo::Command binding() const;
    Deps _d;
    Status _status;
    storage::memo::Snapshot _draft;
    storage::AudioMetadata _media;
    storage::Ticket _ticket;
    voice::Encoded _packet;
    uint8_t _local[16]{};
    double _sendTime = 0;
    uint64_t _now = 0, _started = 0, _expiryRetry = 0;
    uint32_t _viewFloor = 0, _epoch = 0, _written = 0, _offset = 0, _recordRevision = 0;
    Work _work = Work::None;
    messaging::DeliveryPolicy _policy = messaging::DeliveryPolicy::DirectOnly;
    Code _failure = Code::Ok;
    uint16_t _resumeFrame = 0;
    uint8_t _configuredVolume = 70;
    bool _accepting = false, _audioOwned = false, _recording = false, _stopping = false, _closed = false;
    bool _conversation = false, _ending = false, _pausing = false;
};
static_assert(sizeof(Controller) <= 384, "Memo controller fixed retention budget changed");
}
