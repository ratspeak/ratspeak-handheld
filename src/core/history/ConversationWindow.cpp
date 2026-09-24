#include "ConversationWindow.h"
#include "reticulum/StatusFacts.h"
#include <cstring>
#include <new>

namespace handheld::history {
namespace {
bool samePeer(const uint8_t* a, const uint8_t* b) { return !std::memcmp(a, b, 16); }
}

template<size_t N> bool ConversationWindow<N>::changeView() {
    auto& c = _control.value;
    if (c.view == UINT32_MAX) { c.state = State::Exhausted; c.error = storage::Error::RevisionExhausted; return false; }
    ++c.view; c.error = storage::Error::None; return true;
}
template<size_t N> void ConversationWindow<N>::open(uint32_t identity, Order order) {
    if (!identity || !storage::validConversationOrder(order) || !changeView()) return;
    auto& c = _control.value;
    c.identity = identity; c.order = order; c.open = true; c.state = State::Loading;
    c.intent = Intent::First; c.followFirst = true; c.updated = false; c.scrollOffset = 0;
    std::memset(c.selected, 0, 16); c.selectedValid = false;
    if (!c.awaiting) c.phase = Phase::Idle;
}
template<size_t N> void ConversationWindow<N>::resume(uint32_t identity, Order order) {
    const auto& info = live().info.value;
    if (!identity || !_control.value.publication || info.identity != identity || info.order != order) {
        open(identity, order); return;
    }
    if (!changeView()) return;
    auto& c = _control.value;
    c.identity = identity; c.order = order; c.open = true;
    // The live bank already belongs to this identity/order. Re-entering an
    // unchanged list needs no filesystem read; hidden mutations are revisioned.
    bool stale = info.sourceRevision != c.observedRevision || c.updated || !freshnessAvailable();
    for (size_t i = 0; i < count(); ++i) stale |= bool(row(i)->flags & Row::Unavailable);
    c.state = stale ? State::Loading : State::Ready;
    c.intent = stale ? Intent::Refresh : Intent::None;
    if (!c.statusReady) c.statusDirty = true;
    if (!c.awaiting) c.phase = Phase::Idle;
}
template<size_t N> void ConversationWindow<N>::close() {
    changeView();
    auto& c = _control.value;
    c.open = false; c.state = State::Closed; c.intent = Intent::None;
    if (!c.awaiting) c.phase = Phase::Idle;
}
template<size_t N> bool ConversationWindow<N>::visible() const {
    const auto& c = _control.value; const auto& info = live().info.value;
    return c.open && c.publication && info.identity == c.identity && info.order == c.order;
}
template<size_t N> bool ConversationWindow<N>::loading() const {
    const auto& c = _control.value;
    return c.open && (c.awaiting || c.phase != Phase::Idle || c.intent != Intent::None);
}
template<size_t N> size_t ConversationWindow<N>::count() const { return visible() ? live().info.value.count : 0; }
template<size_t N> uint32_t ConversationWindow<N>::total() const { return visible() ? live().info.value.total : 0; }
template<size_t N> const typename ConversationWindow<N>::Row* ConversationWindow<N>::row(size_t index) const {
    return index < count() ? std::launder(reinterpret_cast<const Row*>(live().rows[index].bytes)) : nullptr;
}
template<size_t N> typename ConversationWindow<N>::Row& ConversationWindow<N>::mutableRow(size_t index) {
    return *std::launder(reinterpret_cast<Row*>(_banks[_control.value.active].rows[index].bytes));
}
template<size_t N> typename ConversationWindow<N>::Selector ConversationWindow<N>::selector(size_t index) const {
    Selector result;
    std::memcpy(&result, _banks[_control.value.active ^ 1].rows[index].bytes, sizeof(result));
    return result;
}
template<size_t N> void ConversationWindow<N>::putRow(size_t index, const Row& row) {
    new (candidate().rows[index].bytes) Row(row);
}
template<size_t N> bool ConversationWindow<N>::select(size_t index) {
    const auto* selected = row(index);
    if (!selected) return false;
    std::memcpy(_control.value.selected, selected->peer, 16); _control.value.selectedValid = true; return true;
}
template<size_t N> size_t ConversationWindow<N>::selectedIndex() const {
    if (_control.value.selectedValid)
        for (size_t i = 0; i < count(); ++i) if (samePeer(row(i)->peer, _control.value.selected)) return i;
    return PageSize;
}
template<size_t N> bool ConversationWindow<N>::canPrevious() const { return visible() && live().info.value.moreBefore; }
template<size_t N> bool ConversationWindow<N>::canNext() const { return visible() && live().info.value.moreAfter; }
template<size_t N> bool ConversationWindow<N>::navigate(Intent intent) {
    auto& c = _control.value;
    if (!c.open || (intent != Intent::First && !visible()) || !changeView()) return false;
    c.intent = intent; c.state = State::Loading; c.scrollOffset = 0;
    c.followFirst = intent == Intent::First;
    if (c.followFirst) c.updated = false;
    std::memset(c.selected, 0, 16); c.selectedValid = false;
    if (!c.awaiting) c.phase = Phase::Idle;
    return true;
}
template<size_t N> bool ConversationWindow<N>::first() { return navigate(Intent::First); }
template<size_t N> bool ConversationWindow<N>::previous() { return canPrevious() && navigate(Intent::Previous); }
template<size_t N> bool ConversationWindow<N>::nextPage() { return canNext() && navigate(Intent::Next); }
template<size_t N> bool ConversationWindow<N>::last() { return canNext() && navigate(Intent::Last); }
template<size_t N> void ConversationWindow<N>::refresh() {
    auto& c = _control.value;
    if (!c.open || c.state == State::Exhausted) return;
    // A stream of unrelated mutations must not restart a partially read page.
    if (c.intent != Intent::None) return;
    if (!loading() && !changeView()) return;
    c.intent = Intent::Refresh;
    if (c.state != State::Retrying) c.state = State::Loading;
}
template<size_t N> void ConversationWindow<N>::observeRevision(uint32_t revision) {
    auto& c = _control.value;
    if (c.observedRevision == revision) return;
    c.observedRevision = revision;
    if (revision == UINT32_MAX) return;
    // A queued page samples this revision when it starts. Only a scan already
    // underway needs a follow-up; never restart its accepted storage credit.
    if ((c.phase == Phase::Page || c.phase == Phase::Detail) &&
        candidate().info.value.sourceRevision == revision) return;
    refresh();
}
template<size_t N> void ConversationWindow<N>::observeStatusRevision(uint32_t revision) {
    auto& c = _control.value;
    if (c.observedStatusRevision == revision) return;
    c.observedStatusRevision = revision; c.statusDirty = true;
}
template<size_t N> void ConversationWindow<N>::acknowledgePublication(uint32_t revision) {
    if (revision == _control.value.publication) _control.value.held = false;
}
template<size_t N> typename ConversationWindow<N>::Kind ConversationWindow<N>::pendingKind() const {
    const auto& c = _control.value;
    if (!c.awaiting) return Kind::None;
    return c.phase == Phase::Page ? Kind::Page : c.phase == Phase::Detail ? Kind::Detail : Kind::Status;
}
template<size_t N> void ConversationWindow<N>::beginIntent() {
    auto& c = _control.value;
    const auto intent = c.intent; c.intent = Intent::None;
    auto& info = candidate().info.value;
    info = live().info.value;
    if (intent == Intent::First || !visible() || (intent == Intent::Refresh && c.followFirst)) {
        info = {}; info.identity = c.identity; info.order = c.order;
    } else if (intent == Intent::Last) {
        info = {}; info.identity = c.identity; info.order = c.order; info.direction = Direction::Before;
    } else if ((intent == Intent::Previous || intent == Intent::Next) && count()) {
        const auto* boundary = row(intent == Intent::Previous ? 0 : count() - 1);
        info.bound.timestamp = boundary->timestamp; std::memcpy(info.bound.peer, boundary->peer, 16);
        info.direction = intent == Intent::Previous ? Direction::Before : Direction::After;
        info.hasCursor = true;
        info.page = intent == Intent::Previous ? (info.page > 1 ? info.page - 1 : 1) : info.page + 1;
    }
    info.count = 0; info.sourceRevision = c.observedRevision;
    c.index = 0; c.publish = false; c.phase = Phase::Page;
}
template<size_t N> typename ConversationWindow<N>::Query ConversationWindow<N>::next(uint32_t now) {
    auto& c = _control.value;
    if (!c.open || c.state == State::Exhausted || c.awaiting) return {};
    if (c.phase == Phase::Idle && visible() && c.statusDirty &&
        (c.held || c.intent == Intent::None) &&
        (!c.statusFailed || !c.statusReady || int32_t(now - c.retryAt) >= 0)) {
        c.phase = Phase::Status; c.index = 0; c.statusFailed = false;
        c.sampledStatusRevision = c.observedStatusRevision;
    }
    if (c.phase == Phase::Status) {
        while (c.index < count() && (!(row(c.index)->flags & Row::HasOutgoing) || (row(c.index)->flags & Row::Unavailable))) ++c.index;
        if (c.index == count()) finishStatuses(now);
    }
    if (c.held && c.phase != Phase::Status) return {};
    if (c.phase != Phase::Status && c.state == State::Retrying && int32_t(now - c.retryAt) < 0) return {};
    if (c.phase == Phase::Idle && c.intent != Intent::None) beginIntent();
    if (c.phase == Phase::Detail) {
        while (c.index < candidate().info.value.count && selector(c.index).error != storage::Error::None)
            unavailableDetail(selector(c.index).error);
        if (c.publish) { publish(); return next(now); }
    }
    if (c.phase == Phase::Idle) return {};
    if (c.nonce == UINT32_MAX) { c.state = State::Exhausted; c.error = storage::Error::RevisionExhausted; return {}; }
    Query query; query.nonce = ++c.nonce; query.order = c.order;
    if (c.phase == Phase::Page) {
        const auto& info = candidate().info.value;
        query.kind = Kind::Page; query.selector.cursor = info.bound;
        query.direction = info.direction; query.hasCursor = info.hasCursor;
        query.capacity = N * sizeof(Selector);
    } else if (c.phase == Phase::Detail) {
        query.kind = Kind::Detail; query.selector = selector(c.index); query.capacity = sizeof(Row);
    } else {
        const auto& current = *row(c.index);
        query.kind = Kind::Status; std::memcpy(query.selector.cursor.peer, current.peer, 16);
        query.selector.counter = current.lastOutgoingCounter;
        query.statusRevision = c.sampledStatusRevision; query.capacity = sizeof(StatusProjection);
    }
    c.awaiting = true; c.copied = false; c.pendingView = c.view; c.owner = {};
    return query;
}
template<size_t N> bool ConversationWindow<N>::admitted(uint32_t nonce, storage::Ticket owner) {
    auto& c = _control.value;
    if (!c.awaiting || c.copied || nonce != c.nonce || c.owner.valid() || !owner.valid()) return false;
    c.owner = owner; return true;
}
template<size_t N> void ConversationWindow<N>::fail(storage::Error error, uint32_t now) {
    auto& c = _control.value; c.error = error; c.state = State::Retrying; c.retryAt = now + 1000;
}
template<size_t N> void ConversationWindow<N>::rejected(uint32_t nonce, storage::Rejection reason, uint32_t now) {
    auto& c = _control.value;
    if (!c.awaiting || c.copied || c.owner.valid() || nonce != c.nonce) return;
    c.awaiting = false;
    if (c.pendingView != c.view) { c.phase = Phase::Idle; return; }
    // No ticket was accepted. Keep this exact page/detail/status position and
    // try on the next owner tick rather than imposing an I/O failure backoff.
    if (reason == storage::Rejection::Busy) return;
    if (c.phase == Phase::Status) {
        unavailableStatus(storage::Error::Unavailable); ++c.index; c.statusFailed = true; return;
    }
    if (reason == storage::Rejection::Exhausted) {
        c.state = State::Exhausted; c.error = storage::Error::CounterExhausted;
        c.phase = Phase::Idle; c.intent = Intent::None; return;
    }
    fail(reason == storage::Rejection::NoMemory ? storage::Error::Allocation : storage::Error::Unavailable, now);
}
template<size_t N> bool ConversationWindow<N>::result(uint32_t nonce, const storage::Result& result,
                                                     const void* bytes, size_t length, uint32_t now) {
    auto& c = _control.value;
    if (!c.awaiting || c.copied || !c.owner.valid() || nonce != c.nonce) return false;
    c.copied = true;
    if (c.pendingView != c.view || !c.open) return true;
    if (c.phase == Phase::Status) {
        if (result.outcome != storage::Outcome::Committed || result.error != storage::Error::None ||
            (!bytes && length) || length != result.length || !statusResult(result, static_cast<const uint8_t*>(bytes), length)) {
            unavailableStatus(result.error == storage::Error::None ? storage::Error::InvalidRecord : result.error);
            c.statusFailed = true;
        }
        ++c.index; return true;
    }
    if (result.outcome != storage::Outcome::Committed || result.error != storage::Error::None) {
        if (c.phase == Phase::Detail && (result.error == storage::Error::InvalidRecord || result.error == storage::Error::Stale)) {
            unavailableDetail(result.error); c.state = State::Loading; c.error = storage::Error::None;
        } else fail(result.error == storage::Error::None ? storage::Error::Internal : result.error, now);
        return true;
    }
    if ((!bytes && length) || length != result.length ||
        !(c.phase == Phase::Page ? pageResult(result, static_cast<const uint8_t*>(bytes), length) :
                                   detailResult(result, static_cast<const uint8_t*>(bytes), length))) {
        fail(storage::Error::InvalidRecord, now);
    } else { c.error = storage::Error::None; c.state = State::Loading; }
    return true;
}
template<size_t N> bool ConversationWindow<N>::released(uint32_t nonce) {
    auto& c = _control.value;
    if (!c.awaiting || !c.copied || nonce != c.nonce) return false;
    c.awaiting = false; c.copied = false; c.owner = {};
    if (c.pendingView != c.view || !c.open) { c.phase = Phase::Idle; c.publish = false; return true; }
    if (c.publish) publish();
    return true;
}
template<size_t N> bool ConversationWindow<N>::pageResult(const storage::Result& result, const uint8_t* bytes, size_t length) {
    auto& c = _control.value; auto& info = candidate().info.value;
    const size_t entries = length / sizeof(Selector);
    if (length % sizeof(Selector) || entries > N || result.total < entries || (!entries && result.more)) return false;
    if (!entries && result.total) {
        if (!info.hasCursor) return false;
        info.bound = {}; info.hasCursor = false; info.direction = Direction::After; info.page = 1;
        return true;
    }
    Selector previous;
    for (size_t i = 0; i < entries; ++i) {
        Selector current; std::memcpy(&current, bytes + i * sizeof(Selector), sizeof(current));
        if (!storage::validConversationSelector(current) ||
            (current.error != storage::Error::None && current.error != storage::Error::InvalidRecord) ||
            (i && !storage::conversationLess(previous.cursor, current.cursor, info.order)) ||
            (info.hasCursor && (info.direction == Direction::Before ?
                !storage::conversationLess(current.cursor, info.bound, info.order) :
                !storage::conversationLess(info.bound, current.cursor, info.order)))) return false;
        // In Recent order the same peer could otherwise appear twice with
        // different timestamps. Directory rows always represent distinct peers.
        for (size_t j = 0; j < i; ++j) if (samePeer(selector(j).cursor.peer, current.cursor.peer)) return false;
        std::memcpy(candidate().rows[i].bytes, &current, sizeof(current));
        previous = current;
    }
    info.count = entries; info.total = result.total;
    info.moreBefore = entries && (info.direction == Direction::Before ? result.more : info.hasCursor);
    info.moreAfter = entries && (info.direction == Direction::After ? result.more : info.hasCursor);
    if (entries && info.direction == Direction::Before && !info.hasCursor) {
        // Storage returns the last N selectors in one bounded scan. Keep the
        // final partial page, matching repeated Next on an unchanged directory.
        const size_t tail = (result.total - 1) % N + 1;
        if (tail > entries) return false;
        for (size_t i = 0; tail < entries && i < tail; ++i)
            std::memcpy(candidate().rows[i].bytes, candidate().rows[entries - tail + i].bytes, sizeof(Selector));
        info.count = tail; info.moreBefore = result.total > tail;
        info.page = (result.total - 1) / N + 1;
    }
    if (!info.moreBefore) info.page = 1;
    c.index = 0; c.phase = Phase::Detail;
    if (!entries) c.publish = true;
    return true;
}
template<size_t N> bool ConversationWindow<N>::detailResult(const storage::Result& result, const uint8_t* bytes, size_t length) {
    auto& c = _control.value;
    if (length != sizeof(Row) || c.index >= candidate().info.value.count) return false;
    const auto selected = selector(c.index);
    Row value; std::memcpy(&value, bytes, sizeof(value));
    if (!samePeer(result.key.peer, selected.cursor.peer) || result.key.counter != selected.counter ||
        result.key.incoming != bool(selected.incoming) || !samePeer(value.peer, selected.cursor.peer) ||
        value.timestamp != selected.cursor.timestamp || value.previewLength >= sizeof(value.preview) ||
        value.preview[value.previewLength] || std::memchr(value.preview, 0, value.previewLength) ||
        value.error != storage::Error::None || value.status > uint8_t(LXMFStatus::UNCONFIRMED) ||
        value.durableStatus != value.status || value.unreadCount > value.totalCount ||
        (value.flags & ~(Row::LastIncoming | Row::HasOutgoing | Row::PreviewTruncated)) ||
        bool(value.flags & Row::LastIncoming) != bool(selected.incoming) ||
        bool(value.flags & Row::HasOutgoing) != bool(value.lastOutgoingCounter)) return false;
    putRow(c.index, value);
    if (++c.index == candidate().info.value.count) c.publish = true;
    return true;
}
template<size_t N> void ConversationWindow<N>::unavailableDetail(storage::Error error) {
    auto& c = _control.value; const auto selected = selector(c.index);
    Row value; value.timestamp = selected.cursor.timestamp; std::memcpy(value.peer, selected.cursor.peer, 16);
    value.error = error; value.flags = Row::Unavailable | (selected.incoming ? Row::LastIncoming : 0);
    putRow(c.index, value);
    if (++c.index == candidate().info.value.count) c.publish = true;
}
template<size_t N> void ConversationWindow<N>::publish() {
    auto& c = _control.value;
    if (c.publication == UINT32_MAX) { c.state = State::Exhausted; c.error = storage::Error::RevisionExhausted; return; }
    c.active ^= 1; ++c.publication; c.held = true; c.phase = Phase::Idle; c.publish = false;
    c.state = State::Ready; c.statusDirty = true; c.statusReady = false; c.retryAt = 0;
    c.followFirst = !live().info.value.moreBefore;
    c.updated = live().info.value.sourceRevision != c.observedRevision;
    if (!c.selectedValid && count()) select(0);
}
template<size_t N> bool ConversationWindow<N>::statusResult(const storage::Result& result, const uint8_t* bytes, size_t length) {
    auto& c = _control.value; auto& current = mutableRow(c.index);
    if (length != sizeof(StatusProjection) || !samePeer(result.key.peer, current.peer) || result.key.incoming ||
        result.key.counter != current.lastOutgoingCounter || result.revision != c.sampledStatusRevision ||
        result.revision != c.observedStatusRevision) return false;
    StatusProjection projection; std::memcpy(&projection, bytes, sizeof(projection));
    if (projection.counter != result.key.counter || projection.desired > uint8_t(LXMFStatus::UNCONFIRMED) ||
        projection.durable > uint8_t(LXMFStatus::UNCONFIRMED) || projection.error > storage::Error::Internal ||
        (projection.flags & ~(StatusProjection::Available | StatusProjection::Pending | StatusProjection::TxSuppressed))) return false;
    if (!(projection.flags & StatusProjection::Available)) {
        unavailableStatus(projection.error == storage::Error::None ? storage::Error::Unavailable : projection.error);
        c.statusFailed = true;
    } else {
        current.status = projection.desired; current.durableStatus = projection.durable; current.error = projection.error;
        current.flags = (current.flags & ~(Row::StatusPending | Row::TxSuppressed | Row::StatusUnavailable)) | Row::StatusFresh |
            (projection.flags & StatusProjection::Pending ? Row::StatusPending : 0) |
            (projection.flags & StatusProjection::TxSuppressed ? Row::TxSuppressed : 0);
    }
    return true;
}
template<size_t N> void ConversationWindow<N>::unavailableStatus(storage::Error error) {
    auto& c = _control.value; auto& current = mutableRow(c.index);
    const auto& previous = candidate();
    if (!(current.flags & Row::StatusFresh) && c.held && previous.info.value.identity == c.identity) {
        for (size_t i = 0; i < previous.info.value.count; ++i) {
            const auto& known = *std::launder(reinterpret_cast<const Row*>(previous.rows[i].bytes));
            if (!(known.flags & Row::HasOutgoing) || !samePeer(known.peer, current.peer) ||
                known.lastOutgoingCounter != current.lastOutgoingCounter) continue;
            const auto facts = retainKnownStatus(
                {current.outgoingRevision, current.status, current.durableStatus,
                 bool(current.flags & Row::StatusPending), bool(current.flags & Row::TxSuppressed)},
                {known.outgoingRevision, known.status, known.durableStatus,
                 bool(known.flags & Row::StatusPending), bool(known.flags & Row::TxSuppressed)});
            current.status = facts.desired; current.durableStatus = facts.durable;
            current.flags = (current.flags & ~(Row::StatusPending | Row::TxSuppressed)) |
                (facts.pending ? Row::StatusPending : 0) | (facts.suppressed ? Row::TxSuppressed : 0);
            break;
        }
    }
    current.flags |= Row::StatusUnavailable;
    current.error = error;
}
template<size_t N> void ConversationWindow<N>::finishStatuses(uint32_t now) {
    auto& c = _control.value;
    if (c.sampledStatusRevision != c.observedStatusRevision) {
        for (c.index = 0; c.index < count(); ++c.index) if ((row(c.index)->flags & Row::HasOutgoing) && !(row(c.index)->flags & Row::Unavailable))
            unavailableStatus(storage::Error::Stale);
        c.statusFailed = true;
    }
    c.phase = Phase::Idle; c.statusReady = true; c.statusDirty = c.statusFailed;
    c.retryAt = c.statusFailed ? now + 1000 : 0;
    if (c.statusPublication == UINT32_MAX) { c.state = State::Exhausted; c.error = storage::Error::RevisionExhausted; }
    else ++c.statusPublication;
}

template class ConversationWindow<2>;
template class ConversationWindow<16>;
template class ConversationWindow<64>;

} // namespace handheld::history
