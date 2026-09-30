#pragma once

#include "RustLinkManager.h"
#include "RustIncomingDelivery.h"
#include "storage/PurgeJournal.h"
#include "config/PropagationSettings.h"

class RustLxmfEngine;

// One finite inbox transaction. It borrows the existing codec only inside the
// response callback and holds scalar storage/results across polls.
class RustPropagationInbox final : public RustLinkManager::RequestSink {
public:
    void begin(RustLxmfEngine&);
    void poll();
    void stop();
    void dropPeer(const uint8_t peer[16]);
    bool drained() const { return !_storage.valid() && !_incoming.valid() && !_owner; }
    bool requestSync();
    handheld::propagation::SyncView view() const;
    void onLinkResponse(const uint8_t*, size_t) override;
    void onLinkRequestFailed(RustLinkManager::RequestError) override;
    void onLinkSetupFailure(const uint8_t peer[16]);
private:
    enum class Stage : uint8_t { Idle, Select, Load, List, ListWait, Fetch, FetchWait,
        Commit, Write, Purge, PurgeWait, Clear, Drain };
    using Status = handheld::propagation::SyncStatus;
    void finish(Status, bool networkFailure = false);
    void discard(Status);
    bool allowed() const;
    bool ensureLink();
    void settleStorage();
    void skip(Status);
    void nextItem();
    RustLxmfEngine* _engine = nullptr;
    handheld::storage::purge::Journal _journal;
    handheld::storage::Ticket _storage;
    RustIncomingDelivery::IncomingRef _incoming;
    uint64_t _next = 0, _started = 0, _stepSince = 0, _requestAt = 0, _sourceSince = 0, _linkSince = 0;
    uint32_t _owner = 0;
    uint8_t _node[16]{};
    Stage _stage = Stage::Idle;
    Status _status = Status::Off, _afterClear = Status::Idle, _skipped = Status::Complete;
    uint8_t _index = 0, _attempts = 0, _received = 0, _interface = UINT8_MAX;
    bool _enabled = false, _requested = false, _stopping = false, _hasStarted = false;
};
