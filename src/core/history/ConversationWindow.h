#pragma once

#include "HistoryWindow.h"
#include "storage/ConversationView.h"
#include <cstddef>
#include <cstdint>

namespace handheld::history {

// A complete persisted-directory viewport with one query credit. The two banks
// are the only retained rows: selectors occupy candidate row slots until their
// copied detail arrives. Names and avatars remain in their existing owners.
template<size_t Capacity>
class ConversationWindow {
public:
    static_assert(Capacity == 2 || Capacity == 16 || Capacity == 64, "Use a bounded row budget");
    static constexpr size_t PageSize = Capacity;
    using Row = storage::ConversationView;
    using Selector = storage::ConversationSelector;
    using Cursor = storage::ConversationCursor;
    using Order = storage::ConversationOrder;
    using Direction = storage::ConversationDirection;
    using StatusProjection = HistoryWindow::StatusProjection;
    enum class Kind : uint8_t { None, Page, Detail, Status };
    enum class State : uint8_t { Closed, Loading, Ready, Retrying, Exhausted };
    struct Query {
        Selector selector;
        uint32_t nonce = 0, statusRevision = 0;
        uint16_t capacity = 0;
        Kind kind = Kind::None;
        Order order = Order::Recent;
        Direction direction = Direction::After;
        bool hasCursor = false;
    };

    void open(uint32_t identityGeneration, Order order = Order::Recent);
    void resume(uint32_t identityGeneration, Order order = Order::Recent);
    void close();
    bool first();
    bool previous();
    bool nextPage();
    bool last();
    void refresh();
    void observeRevision(uint32_t revision);
    void observeStatusRevision(uint32_t revision);
    bool followsFirst() const { return _control.value.followFirst; }
    bool updated() const { return _control.value.updated; }
    bool freshnessAvailable() const { return _control.value.observedRevision != UINT32_MAX; }

    Query next(uint32_t now);
    bool admitted(uint32_t nonce, storage::Ticket owner);
    void rejected(uint32_t nonce, storage::Rejection reason, uint32_t now);
    bool result(uint32_t nonce, const storage::Result&, const void* bytes, size_t length, uint32_t now);
    bool released(uint32_t nonce);
    storage::Ticket ownerTicket() const { return _control.value.owner; }
    uint32_t pendingNonce() const { return _control.value.nonce; }
    bool responseCopied() const { return _control.value.copied; }
    Kind pendingKind() const;
    uint32_t pendingStatusRevision() const { return _control.value.sampledStatusRevision; }

    // Rebind/detach static label pointers before acknowledging. As in history,
    // wait for statusReady on a same-view publication so known proof facts in
    // the old bank survive an unavailable first status query.
    void acknowledgePublication(uint32_t revision);
    uint32_t revision() const { return _control.value.publication; }
    uint32_t statusRevision() const { return _control.value.statusPublication; }
    bool statusReady() const { return _control.value.statusReady; }
    bool statusRefreshDelayed() const { return visible() && live().info.value.statusRefresh.delayed(); }
    uint32_t identityGeneration() const { return _control.value.identity; }
    Order order() const { return _control.value.order; }
    State state() const { return _control.value.state; }
    storage::Error error() const { return _control.value.error; }
    bool visible() const;
    bool loading() const;
    bool canPrevious() const;
    bool canNext() const;
    size_t count() const;
    uint32_t total() const;
    uint32_t pageNumber() const { return visible() ? live().info.value.page : 1; }
    const Row* row(size_t index) const;
    bool select(size_t index);
    const uint8_t* selectedPeer() const { return _control.value.selected; }
    size_t selectedIndex() const;
    void setScrollOffset(uint32_t offset) { _control.value.scrollOffset = offset; }
    uint32_t scrollOffset() const { return _control.value.scrollOffset; }

private:
    enum class Intent : uint8_t { None, First, Previous, Next, Last, Refresh };
    enum class Phase : uint8_t { Idle, Page, Detail, Status };
    struct Control {
        storage::Ticket owner;
        uint8_t selected[16] = {};
        uint32_t identity = 0, view = 0, nonce = 0, pendingView = 0, publication = 0;
        uint32_t retryAt = 0, scrollOffset = 0, observedRevision = 0;
        uint32_t observedStatusRevision = 0, sampledStatusRevision = 0, statusPublication = 0;
        State state = State::Closed;
        storage::Error error = storage::Error::None;
        Order order = Order::Recent;
        Intent intent = Intent::None;
        Phase phase = Phase::Idle;
        uint8_t active = 0, index = 0;
        bool awaiting = false, copied = false, publish = false, held = false;
        bool followFirst = true, updated = false, open = false, selectedValid = false;
        bool statusReady = false, statusDirty = false, statusFailed = false;
    };
    static_assert(sizeof(Control) <= 128, "Conversation controls exceed the adopted reserve");
    union ControlBank {
        Control value;
        uint8_t allocation[128];
        ControlBank() : value{} {}
    };
    struct PageInfo {
        Cursor bound;
        uint32_t identity = 0, total = 0, sourceRevision = 0, page = 1;
        uint8_t count = 0;
        Order order = Order::Recent;
        Direction direction = Direction::After;
        bool hasCursor = false, moreBefore = false, moreAfter = false;
        StatusRefresh statusRefresh;
    };
    static_assert(sizeof(PageInfo) <= 64, "Conversation page metadata exceeds its reserve");
    union InfoBank {
        PageInfo value;
        uint8_t allocation[64];
        InfoBank() : value{} {}
    };
    // Byte storage avoids an active-union lifetime ambiguity while a candidate
    // slot changes from selector to row. No consumer borrows candidate bytes.
    struct alignas(Row) Slot { uint8_t bytes[sizeof(Row)] = {}; };
    struct Bank { Slot rows[Capacity]; InfoBank info; };

    Bank& candidate() { return _banks[_control.value.active ^ 1]; }
    const Bank& live() const { return _banks[_control.value.active]; }
    bool changeView();
    bool navigate(Intent);
    void beginIntent();
    void publish();
    void fail(storage::Error, uint32_t now);
    bool pageResult(const storage::Result&, const uint8_t*, size_t);
    bool detailResult(const storage::Result&, const uint8_t*, size_t);
    void unavailableDetail(storage::Error);
    bool statusResult(const storage::Result&, const uint8_t*, size_t);
    void unavailableStatus(storage::Error);
    void finishStatuses(uint32_t now);
    Row& mutableRow(size_t index);
    Selector selector(size_t index) const;
    void putRow(size_t index, const Row&);

    ControlBank _control;
    Bank _banks[2];
};

static_assert(sizeof(ConversationWindow<16>) == 2560, "16-row window budget changed");
static_assert(sizeof(ConversationWindow<64>) == 9472, "64-row window budget changed");
static_assert(sizeof(ConversationWindow<2>) == 544, "Two-chat viewport budget changed");
// All handheld frontends share the same visible page size and refresh policy.
using ConversationList = ConversationWindow<2>;
extern template class ConversationWindow<2>;
extern template class ConversationWindow<16>;
extern template class ConversationWindow<64>;

} // namespace handheld::history
