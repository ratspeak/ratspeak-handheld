#pragma once

#include "storage/StorageContract.h"
#include "StatusRefresh.h"
#include <cstddef>
#include <cstdint>

namespace handheld::history {

// One owner-thread presentation and reader policy for Canvas and ServiceClient.
// Adapters copy terminal bytes while the provider credit is held. result()
// consumes that stable owner copy; released() retires the opaque adapter token
// only after the actual provider credit has been released. result() may run
// before or after that provider release. No borrowed bytes survive result(),
// and no further request is possible before released().
class HistoryWindow {
public:
    static constexpr size_t PageSize = 48, VisibleSpans = 8;
    static constexpr size_t TextBytes = 1792, PreviewBytes = 216;
    static constexpr uint16_t ReadCapacity = 512;
    enum class Mode : uint8_t { Closed, Chat, Full };
    enum class State : uint8_t { Closed, Loading, Ready, Retrying, Exhausted };
    enum class Kind : uint8_t { None, Page, Record, Status };
    enum class Error : uint8_t { None, Busy, Storage, InvalidResponse, Exhausted };

    struct Query {
        uint32_t nonce = 0;
        // Status queries/results use the backend sample in Result.revision;
        // Record queries/results retain the persisted record's revision.
        uint32_t statusRevision = 0;
        uint8_t peer[16] = {};
        storage::HistoryEntry cursor;
        uint32_t offset = 0;
        uint16_t capacity = ReadCapacity;
        Kind kind = Kind::None;
        storage::HistoryDirection direction = storage::HistoryDirection::Before;
    };
    struct StatusProjection {
        enum Flag : uint8_t { Available = 1, Pending = 2, TxSuppressed = 4 };
        uint32_t counter = 0;
        uint8_t desired = 0, durable = 0;
        storage::Error error = storage::Error::None;
        uint8_t flags = 0;
    };
    static_assert(sizeof(StatusProjection) == 8, "Status response must stay bounded");
    struct Span {
        enum Flag : uint8_t { Incoming = 1, Read = 2, Escaped = 4, StatusPending = 8, TxSuppressed = 16, Unavailable = 32, StatusUnavailable = 64, StatusFresh = 128 };
        uint32_t timestamp = 0, counter = 0, sourceOffset = 0, recordRevision = 0;
        uint16_t textOffset = 0, textLength = 0, titleLength = 0, contentLength = 0, sourceLength = 0;
        uint8_t status = 0, durableStatus = 0;
        storage::Error statusError = storage::Error::None;
        uint8_t flags = 0;
        storage::Error readError = storage::Error::None;
        bool incoming() const { return flags & Incoming; }
        bool unavailable() const { return flags & Unavailable; }
        bool more() const { return !unavailable() && sourceOffset + sourceLength < uint32_t(titleLength) + contentLength; }
    };
    static_assert(sizeof(Span) == 32, "History span metadata budget changed");

    void open(const uint8_t peer[16], uint32_t identityGeneration);
    void close();
    bool older();
    bool newer();
    bool newest();
    bool openFull(size_t span);
    bool backToChat();
    void refresh();
    void notifyArrival();

    Query next(uint32_t now);
    bool admitted(uint32_t nonce, storage::Ticket owner);
    void rejected(uint32_t nonce, storage::Rejection reason, uint32_t now);
    bool result(uint32_t nonce, const storage::Result&, const void* bytes, size_t length, uint32_t now);
    bool released(uint32_t nonce);
    storage::Ticket ownerTicket() const { return control().owner; }
    uint32_t pendingNonce() const { return control().query; }
    bool responseCopied() const { return control().copied; }
    Kind pendingKind() const;
    uint32_t pendingStatusRevision() const { return control().sampledStatusRevision; }
    void observeStatusRevision(uint32_t revision);
    void observeHistoryRevision(uint32_t revision);
    bool freshnessAvailable() const { return control().observedHistoryRevision != UINT32_MAX; }
    // Ready includes explicit unavailable/error projections. This prevents a
    // changing backend or failed read from holding a body publication forever.
    bool statusReady() const { return control().statusReady; }
    bool statusRefreshDelayed() const { return visible() && livePage().info.statusRefresh.delayed(); }
    uint32_t statusRevision() const { return control().statusPublication; }

    // LVGL may point labels at published text. A publication holds the old bank
    // until those labels have been rebound/detached. Both adapters also wait
    // for statusReady() before acknowledging a same-view publication: the old
    // bank preserves known delivery facts if the first fresh projection fails.
    // Abandoned/hidden views may acknowledge after detaching their labels, and
    // still retire their exact owned query ticket.
    void acknowledgePublication(uint32_t revision);
    uint32_t revision() const { return control().publication; }
    const uint8_t* peer() const { return control().peer; }
    uint32_t identityGeneration() const { return control().identity; }
    size_t focusedSpan() const { return control().focus; }
    // VisibleSpans means the composer. UI focus is part of the control budget.
    void focusSpan(size_t index) { control().focus = index < spanCount() ? index : VisibleSpans; }
    Mode mode() const { return control().mode; }
    State state() const { return control().state; }
    Error error() const { return control().error; }
    storage::Error storageError() const { return control().storageError; }
    bool visible() const;
    bool newBelow() const { return control().newBelow; }
    // Renderers report the actual viewport, not just the selector-page edge.
    // Reading above the bottom preserves the anchored tuple on new arrivals.
    void setViewportAtNewest(bool atBottom);
    bool followsNewest() const { return control().followNewest; }
    void setScrollOffset(uint32_t offset) { control().scrollOffset = offset; }
    uint32_t scrollOffset() const { return control().scrollOffset; }
    bool loading() const;
    bool canOlder() const;
    bool canNewer() const;
    size_t spanCount() const;
    const Span* span(size_t index) const;
    const char* text(size_t index) const;
    uint32_t total() const;
    // Status overlays never restart or invalidate body reading.
    bool status(storage::HistoryEntry key, uint8_t desired, uint8_t durable,
                bool pending, storage::Error error, bool suppressed);
    bool statusUnavailable(storage::HistoryEntry key, storage::Error error);

private:
    enum class Intent : uint8_t { None, Newest, Older, Newer, Refresh, Full };
    enum class Phase : uint8_t { Idle, Page, Preview, Full, Status };
    struct Control {
        uint8_t peer[16] = {};
        uint32_t identity = 0, view = 0, query = 0, pendingView = 0, publication = 0, retryAt = 0;
        storage::Ticket owner;
        storage::HistoryEntry fullKey, restoreKey;
        uint32_t fullOffset = 0, readEnd = 0, scrollOffset = 0, returnScroll = 0;
        Mode mode = Mode::Closed;
        State state = State::Closed;
        Error error = Error::None;
        storage::Error storageError = storage::Error::None;
        Intent intent = Intent::None;
        Phase phase = Phase::Idle;
        uint8_t active = 0, buildIndex = 0, spanCount = 0;
        uint8_t focus = VisibleSpans;
        uint16_t usedText = 0;
        bool awaiting = false, copied = false, publish = false, publicationHeld = false;
        bool newBelow = false, followNewest = true, previousSlice = false;
        uint32_t observedStatusRevision = 0, sampledStatusRevision = 0, statusPublication = 0;
        bool statusDirty = false, statusFailed = false, statusReady = false;
        uint32_t observedHistoryRevision = 0;
    };
    static_assert(sizeof(Control) <= 128, "History control exceeds its row-arena allocation");
    union ControlBank {
        Control value;
        uint8_t allocation[128];
        ControlBank() : value{} {}
    };
    struct PageInfo {
        uint8_t peer[16] = {};
        uint32_t identity = 0;
        storage::HistoryEntry bound;
        uint32_t total = 0;
        uint8_t count = 0, first = 0, last = 0, spans = 0;
        storage::HistoryDirection direction = storage::HistoryDirection::Before;
        bool moreOlder = false, moreNewer = false, full = false;
        StatusRefresh statusRefresh;
    };
    static_assert(sizeof(PageInfo) <= 64, "History page control exceeds its row-arena allocation");
    struct Page { storage::HistoryEntry entries[PageSize] = {}; PageInfo info; };
    union PageBank {
        Page value;
        uint8_t allocation[448];
        PageBank() : value{} {}
    };
    struct Frame { Span spans[VisibleSpans] = {}; char text[TextBytes] = {}; };
    static_assert(sizeof(Frame) == 2048, "History text arena includes every span descriptor");

    Control& control() { return _control.value; }
    const Control& control() const { return _control.value; }
    Page& candidate() { return _pages[control().active ^ 1].value; }
    const Page& livePage() const { return _pages[control().active].value; }
    Frame& candidateFrame() { return _frames[control().active ^ 1]; }
    const Frame& liveFrame() const { return _frames[control().active]; }
    bool changeView();
    bool navigate(Intent);
    void beginIntent();
    void publish();
    void fail(Error, storage::Error, uint32_t now);
    bool pageResult(const storage::Result&, const uint8_t*, size_t);
    bool recordResult(const storage::Result&, const uint8_t*, size_t);
    bool statusResult(const storage::Result&, const uint8_t*, size_t);
    void finishStatuses(uint32_t now);
    void unavailableRecord(storage::Error);

    ControlBank _control;
    PageBank _pages[2];
    Frame _frames[2];
};

static_assert(sizeof(HistoryWindow) == 5120, "History owns exactly its adopted 4096 text +1024 row bytes");

} // namespace handheld::history
