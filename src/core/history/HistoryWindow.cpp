#include "HistoryWindow.h"
#include "reticulum/LXMFMessage.h"
#include "util/DisplayText.h"
#include "reticulum/StatusFacts.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace handheld::history {
namespace {
bool less(storage::HistoryEntry a, storage::HistoryEntry b) {
    return a.counter < b.counter || (a.counter == b.counter && a.incoming < b.incoming);
}
bool equal(storage::HistoryEntry a, storage::HistoryEntry b) {
    return a.counter == b.counter && a.incoming == b.incoming;
}
constexpr size_t rawCapacity = HistoryWindow::ReadCapacity - sizeof(storage::StoredRecordHeader);

}

bool HistoryWindow::changeView() {
    auto& c = control();
    if (c.view == UINT32_MAX) { c.state = State::Exhausted; c.error = Error::Exhausted; return false; }
    ++c.view;
    c.error = Error::None; c.storageError = storage::Error::None;
    return true;
}
void HistoryWindow::open(const uint8_t peer[16], uint32_t identity) {
    if (!peer || !identity || !changeView()) return;
    auto& c = control();
    memcpy(c.peer, peer, 16); c.identity = identity;
    c.mode = Mode::Chat; c.state = State::Loading; c.intent = Intent::Newest;
    c.newBelow = false; c.followNewest = true; c.scrollOffset = 0;
    c.focus = VisibleSpans;
    if (!c.awaiting) c.phase = Phase::Idle;
}
void HistoryWindow::close() {
    changeView();
    auto& c = control();
    c.mode = Mode::Closed; c.state = State::Closed; c.intent = Intent::None;
    _pages[c.active].value.info.statusRefresh = {};
    if (!c.awaiting) c.phase = Phase::Idle;
}
bool HistoryWindow::visible() const {
    const auto& c = control(); const auto& info = livePage().info;
    return c.mode != Mode::Closed && c.publication && info.identity == c.identity &&
        !memcmp(info.peer, c.peer, 16) && info.full == (c.mode == Mode::Full);
}
bool HistoryWindow::loading() const {
    const auto& c = control();
    return c.mode != Mode::Closed && (c.awaiting || c.phase != Phase::Idle || c.intent != Intent::None);
}
size_t HistoryWindow::spanCount() const { return visible() ? livePage().info.spans : 0; }
const HistoryWindow::Span* HistoryWindow::span(size_t index) const {
    return index < spanCount() ? &liveFrame().spans[index] : nullptr;
}
const char* HistoryWindow::text(size_t index) const {
    const auto* row = span(index); return row ? liveFrame().text + row->textOffset : "";
}
uint32_t HistoryWindow::total() const { return visible() ? livePage().info.total : 0; }
bool HistoryWindow::canOlder() const {
    if (!visible()) return false;
    if (control().mode == Mode::Full) return spanCount() && span(0)->sourceOffset;
    const auto& info = livePage().info; return info.first || info.moreOlder;
}
bool HistoryWindow::canNewer() const {
    if (!visible()) return false;
    if (control().mode == Mode::Full) return spanCount() && span(0)->more();
    const auto& info = livePage().info; return info.last < info.count || info.moreNewer;
}
void HistoryWindow::setViewportAtNewest(bool atBottom) {
    auto& c = control();
    if (c.mode != Mode::Chat || !visible()) return;
    const bool follow = atBottom && !canNewer();
    if (c.followNewest == follow) return;
    c.followNewest = follow;
    if (follow) c.newBelow = false;
    // A refresh may already be reading a replacement page when the user
    // scrolls. Retire its exact credit, then rebuild using the new anchor
    // policy; that in-flight result must not move the user's visible tuple.
    if (c.phase == Phase::Page || c.phase == Phase::Preview || c.intent == Intent::Refresh) {
        if (!changeView()) return;
        c.intent = Intent::Refresh; c.state = State::Loading;
        if (!c.awaiting) c.phase = Phase::Idle;
    }
}
bool HistoryWindow::navigate(Intent intent) {
    auto& c = control();
    if (!visible() || !changeView()) return false;
    c.intent = intent; c.state = State::Loading; c.restoreKey = {}; c.scrollOffset = 0;
    if (!c.awaiting) c.phase = Phase::Idle;
    return true;
}
bool HistoryWindow::older() {
    if (!canOlder()) return false;
    const auto* current = span(0);
    const uint32_t end = current ? current->sourceOffset : 0;
    if (!navigate(control().mode == Mode::Full ? Intent::Full : Intent::Older)) return false;
    auto& c = control(); c.followNewest = false;
    if (c.mode == Mode::Full) {
        c.previousSlice = true; c.readEnd = end;
        c.fullOffset = end > rawCapacity - 3 ? end - (rawCapacity - 3) : 0;
    }
    return true;
}
bool HistoryWindow::newer() {
    if (!canNewer()) return false;
    const auto* current = span(0);
    const uint32_t offset = current ? current->sourceOffset + current->sourceLength : 0;
    if (!navigate(control().mode == Mode::Full ? Intent::Full : Intent::Newer)) return false;
    if (control().mode == Mode::Full) {
        control().fullOffset = offset; control().previousSlice = false; control().readEnd = 0;
    }
    return true;
}
bool HistoryWindow::newest() {
    if (control().mode == Mode::Closed || !changeView()) return false;
    auto& c = control(); c.mode = Mode::Chat; c.intent = Intent::Newest;
    c.state = State::Loading; c.newBelow = false; c.followNewest = true; c.scrollOffset = 0;
    if (!c.awaiting) c.phase = Phase::Idle;
    return true;
}
bool HistoryWindow::openFull(size_t index) {
    const auto* row = span(index);
    if (!row || row->unavailable() || control().mode != Mode::Chat) return false;
    const storage::HistoryEntry key{row->counter, row->incoming()};
    const uint32_t scroll = control().scrollOffset;
    if (!navigate(Intent::Full)) return false;
    auto& c = control(); c.mode = Mode::Full; c.fullKey = key;
    c.fullOffset = c.readEnd = 0; c.previousSlice = false; c.returnScroll = scroll;
    return true;
}
bool HistoryWindow::backToChat() {
    if (control().mode != Mode::Full || !changeView()) return false;
    auto& c = control(); c.mode = Mode::Chat; c.intent = Intent::Refresh; c.state = State::Loading;
    c.followNewest = false;
    c.scrollOffset = c.returnScroll;
    if (!c.awaiting) c.phase = Phase::Idle;
    return true;
}
void HistoryWindow::refresh() {
    auto& c = control();
    if (c.mode == Mode::Closed || c.state == State::Exhausted) return;
    // Coalesce refreshes behind a bounded in-progress publication. Unrelated
    // arrivals/status changes cannot repeatedly restart its body requests.
    if (!loading() && !changeView()) return;
    if (c.intent == Intent::None) c.intent = Intent::Refresh;
    if (c.state != State::Retrying) c.state = State::Loading;
}
void HistoryWindow::notifyArrival() {
    if (control().mode == Mode::Full || !control().followNewest) control().newBelow = true;
    else refresh();
}
void HistoryWindow::acknowledgePublication(uint32_t revision) {
    if (revision == control().publication) control().publicationHeld = false;
}
void HistoryWindow::observeStatusRevision(uint32_t revision) {
    auto& c = control();
    if (c.observedStatusRevision == revision) return;
    c.observedStatusRevision = revision; c.statusDirty = true;
}
void HistoryWindow::observeHistoryRevision(uint32_t revision) {
    auto& c = control();
    if (c.observedHistoryRevision == revision) return;
    c.observedHistoryRevision = revision;
    // This signal covers every peer. Refresh the anchored range; only an
    // actual increase in this peer's page total warrants a new-below indicator.
    // At saturation automatic freshness is unavailable; explicit refresh/open
    // remains bounded and usable instead of treating equal MAX as current.
    if (revision != UINT32_MAX) refresh();
}
HistoryWindow::Kind HistoryWindow::pendingKind() const {
    if (!control().awaiting) return Kind::None;
    return control().phase == Phase::Status ? Kind::Status :
        control().phase == Phase::Page ? Kind::Page : Kind::Record;
}

void HistoryWindow::beginIntent() {
    auto& c = control(); const auto intent = c.intent; c.intent = Intent::None;
    auto& page = candidate(); page = livePage();
    auto& info = page.info;
    if (intent != Intent::Refresh) info.statusRefresh = {};
    c.spanCount = 0; c.usedText = 0; c.publish = false;
    info.spans = 0; info.full = c.mode == Mode::Full;
    c.phase = Phase::Preview;
    if (intent == Intent::Newest || info.identity != c.identity || memcmp(info.peer, c.peer, 16)) {
        info = {}; memcpy(info.peer, c.peer, 16); info.identity = c.identity;
        c.restoreKey = {}; c.phase = Phase::Page;
    } else if (c.mode == Mode::Full) {
        info.full = true; c.phase = Phase::Full;
    } else if (intent == Intent::Older) {
        if (info.first) { info.last = info.first; info.first = info.first > VisibleSpans ? info.first - VisibleSpans : 0; }
        else {
            info.bound = page.entries[0]; info.direction = storage::HistoryDirection::Before;
            c.phase = Phase::Page;
        }
    } else if (intent == Intent::Newer) {
        if (info.last < info.count) { info.first = info.last; info.last = std::min(size_t(info.count), size_t(info.first) + VisibleSpans); }
        else {
            info.bound = page.entries[info.count - 1]; info.direction = storage::HistoryDirection::After;
            c.phase = Phase::Page;
        }
    } else if (intent == Intent::Refresh) {
        c.restoreKey = !c.followNewest && info.first < info.count ? page.entries[info.first] : storage::HistoryEntry{};
        if (c.followNewest) { info.bound = {}; info.direction = storage::HistoryDirection::Before; }
        c.phase = Phase::Page;
    }
    c.buildIndex = info.first;
    if (c.phase == Phase::Preview && c.buildIndex == info.last) { c.publish = true; publish(); }
}
HistoryWindow::Query HistoryWindow::next(uint32_t now) {
    auto& c = control();
    if (c.mode == Mode::Closed || c.state == State::Exhausted || c.awaiting) return {};
    if (c.phase == Phase::Idle && visible() && c.statusDirty &&
        (c.publicationHeld || c.intent == Intent::None) &&
        (!c.statusFailed || !c.statusReady || int32_t(now - c.retryAt) >= 0)) {
        c.phase = Phase::Status; c.buildIndex = 0; c.statusFailed = false;
        c.sampledStatusRevision = c.observedStatusRevision;
    }
    if (c.phase == Phase::Status) {
        while (c.buildIndex < spanCount() && (span(c.buildIndex)->incoming() || span(c.buildIndex)->unavailable())) ++c.buildIndex;
        if (c.buildIndex == spanCount()) finishStatuses(now);
    }
    if (c.publicationHeld && c.phase != Phase::Status) return {};
    if (c.phase != Phase::Status && c.state == State::Retrying && int32_t(now - c.retryAt) < 0) return {};
    if (c.phase == Phase::Idle && c.intent != Intent::None) beginIntent();
    if (c.phase == Phase::Idle) return {};
    if (c.query == UINT32_MAX) { c.state = State::Exhausted; c.error = Error::Exhausted; return {}; }
    Query query; query.nonce = ++c.query; memcpy(query.peer, c.peer, 16);
    if (c.phase == Phase::Status) {
        query.kind = Kind::Status; query.cursor = {span(c.buildIndex)->counter, false};
        query.capacity = sizeof(StatusProjection); query.statusRevision = c.sampledStatusRevision;
    } else if (c.phase == Phase::Page) {
        query.kind = Kind::Page; query.cursor = candidate().info.bound;
        query.direction = candidate().info.direction;
        query.capacity = PageSize * sizeof(storage::HistoryEntry);
    } else {
        query.kind = Kind::Record;
        query.cursor = c.phase == Phase::Full ? c.fullKey : candidate().entries[c.buildIndex];
        query.offset = c.phase == Phase::Full ? c.fullOffset : 0;
    }
    c.awaiting = true; c.copied = false; c.pendingView = c.view; c.owner = {};
    return query;
}
bool HistoryWindow::admitted(uint32_t nonce, storage::Ticket owner) {
    auto& c = control();
    if (!c.awaiting || c.copied || nonce != c.query || c.owner.valid() || !owner.valid()) return false;
    c.owner = owner; return true;
}
void HistoryWindow::fail(Error error, storage::Error storageError, uint32_t now) {
    auto& c = control(); c.error = error; c.storageError = storageError;
    c.state = State::Retrying; c.retryAt = now + 1000;
}
void HistoryWindow::rejected(uint32_t nonce, storage::Rejection reason, uint32_t now) {
    auto& c = control();
    if (!c.awaiting || c.copied || c.owner.valid() || nonce != c.query) return;
    c.awaiting = false;
    if (c.pendingView != c.view) { c.phase = Phase::Idle; return; }
    if (reason == storage::Rejection::Busy) return;
    if (c.phase == Phase::Status) {
        statusUnavailable({span(c.buildIndex)->counter, false}, storage::Error::Unavailable);
        ++c.buildIndex; c.statusFailed = true;
        return;
    }
    if (reason == storage::Rejection::Exhausted) {
        c.state = State::Exhausted; c.error = Error::Exhausted; c.phase = Phase::Idle; c.intent = Intent::None;
        return;
    }
    fail(reason == storage::Rejection::Busy ? Error::Busy : Error::Storage, storage::Error::None, now);
}
bool HistoryWindow::result(uint32_t nonce, const storage::Result& result,
                           const void* bytes, size_t length, uint32_t now) {
    auto& c = control();
    if (!c.awaiting || c.copied || !c.owner.valid() || nonce != c.query) return false;
    c.copied = true;
    if (c.pendingView != c.view || c.mode == Mode::Closed) return true;
    if (c.phase == Phase::Status) {
        if (result.outcome != storage::Outcome::Committed || result.error != storage::Error::None ||
            (!bytes && length) || length != result.length || !statusResult(result, static_cast<const uint8_t*>(bytes), length)) {
            statusUnavailable({span(c.buildIndex)->counter, false},
                result.error == storage::Error::None ? storage::Error::InvalidRecord : result.error);
            c.statusFailed = true;
        }
        ++c.buildIndex;
        return true;
    }
    if (result.outcome != storage::Outcome::Committed || result.error != storage::Error::None) {
        if (c.phase != Phase::Page && (result.error == storage::Error::InvalidRecord || result.error == storage::Error::Stale)) {
            unavailableRecord(result.error);
            c.state = State::Loading; c.error = Error::None; c.storageError = storage::Error::None;
            return true;
        }
        fail(Error::Storage, result.error, now); return true;
    }
    if ((!bytes && length) || length != result.length ||
        !(c.phase == Phase::Page ? pageResult(result, static_cast<const uint8_t*>(bytes), length) :
                                  recordResult(result, static_cast<const uint8_t*>(bytes), length))) {
        fail(Error::InvalidResponse, storage::Error::InvalidRecord, now);
    } else { c.error = Error::None; c.storageError = storage::Error::None; c.state = State::Loading; }
    return true;
}
bool HistoryWindow::released(uint32_t nonce) {
    auto& c = control();
    if (!c.awaiting || !c.copied || nonce != c.query) return false;
    c.awaiting = false; c.copied = false; c.owner = {};
    if (c.pendingView != c.view || c.mode == Mode::Closed) { c.phase = Phase::Idle; c.publish = false; return true; }
    if (c.publish) publish();
    return true;
}
void HistoryWindow::publish() {
    auto& c = control();
    if (c.publication == UINT32_MAX) { c.state = State::Exhausted; c.error = Error::Exhausted; return; }
    candidate().info.spans = c.spanCount;
    const auto& previous = livePage().info;
    const auto* oldFocus = previous.identity == c.identity && !memcmp(previous.peer, c.peer, 16) &&
        c.focus < previous.spans ? &liveFrame().spans[c.focus] : nullptr;
    const storage::HistoryEntry focus = oldFocus ? storage::HistoryEntry{oldFocus->counter, oldFocus->incoming()} : storage::HistoryEntry{};
    c.focus = VisibleSpans;
    if (focus.counter) for (size_t i = 0; i < c.spanCount; ++i) {
        const auto& row = candidateFrame().spans[i];
        if (equal(focus, {row.counter, row.incoming()})) { c.focus = i; break; }
    }
    c.active ^= 1; ++c.publication; c.publicationHeld = true;
    c.phase = Phase::Idle; c.publish = false; c.state = State::Ready;
    c.statusDirty = true; c.statusReady = false; c.retryAt = 0;
    if (c.mode == Mode::Chat) {
        const auto& info = livePage().info;
        c.followNewest = c.followNewest && !info.moreNewer && info.last == info.count;
        if (c.followNewest) c.newBelow = false;
    }
}
bool HistoryWindow::statusResult(const storage::Result& result, const uint8_t* bytes, size_t length) {
    const auto& c = control();
    if (length != sizeof(StatusProjection) || memcmp(result.key.peer, c.peer, 16) ||
        result.key.incoming || result.key.counter != span(c.buildIndex)->counter ||
        result.revision != c.sampledStatusRevision || result.revision != c.observedStatusRevision) return false;
    StatusProjection projection; memcpy(&projection, bytes, sizeof(projection));
    if (projection.counter != result.key.counter || projection.desired > uint8_t(LXMFStatus::UNCONFIRMED) ||
        projection.durable > uint8_t(LXMFStatus::UNCONFIRMED) || projection.error > storage::Error::Internal ||
        (projection.flags & ~(StatusProjection::Available | StatusProjection::Pending | StatusProjection::TxSuppressed))) return false;
    if (!(projection.flags & StatusProjection::Available)) {
        statusUnavailable({projection.counter, false}, projection.error == storage::Error::None ? storage::Error::Unavailable : projection.error);
        control().statusFailed = true;
    } else status({projection.counter, false}, projection.desired, projection.durable,
                  projection.flags & StatusProjection::Pending, projection.error, projection.flags & StatusProjection::TxSuppressed);
    return true;
}
void HistoryWindow::finishStatuses(uint32_t now) {
    auto& c = control();
    if (c.sampledStatusRevision != c.observedStatusRevision) {
        for (size_t i = 0; i < spanCount(); ++i) if (!span(i)->incoming())
            statusUnavailable({span(i)->counter, false}, storage::Error::Stale);
        c.statusFailed = true;
    }
    c.phase = Phase::Idle; c.statusReady = true; c.statusDirty = c.statusFailed;
    _pages[c.active].value.info.statusRefresh.finish(c.statusFailed, now);
    c.retryAt = c.statusFailed ? now + 1000 : 0;
    if (c.statusPublication == UINT32_MAX) { c.state = State::Exhausted; c.error = Error::Exhausted; }
    else ++c.statusPublication;
}
bool HistoryWindow::pageResult(const storage::Result& result, const uint8_t* bytes, size_t length) {
    auto& page = candidate(); auto& info = page.info; auto& c = control();
    const size_t count = length / sizeof(storage::HistoryEntry);
    if (length % sizeof(storage::HistoryEntry) || count > PageSize || result.total < count ||
        memcmp(result.key.peer, c.peer, 16) || (!count && (result.key.counter || result.more))) return false;
    if (!count && result.total) {
        // Trim/deletion can empty an exclusive range while other records remain.
        // Re-anchor once at newest; never index a zero-count selector array or
        // skip the old boundary by manufacturing an opposite exclusive cursor.
        if (!info.bound.counter) return false;
        info.bound = {}; info.direction = storage::HistoryDirection::Before;
        c.restoreKey = {}; c.phase = Phase::Page;
        return true;
    }
    storage::HistoryEntry previous;
    for (size_t i = 0; i < count; ++i) {
        const auto* raw = bytes + i * sizeof(storage::HistoryEntry);
        if (raw[offsetof(storage::HistoryEntry, incoming)] > 1) return false;
        storage::HistoryEntry entry; memcpy(&entry, raw, sizeof(entry));
        if (!entry.counter || (i && !less(previous, entry)) || (info.bound.counter &&
            (info.direction == storage::HistoryDirection::Before ? !less(entry, info.bound) : !less(info.bound, entry)))) return false;
        previous = entry;
    }
    if (count) {
        storage::HistoryEntry boundary;
        memcpy(&boundary, bytes + (info.direction == storage::HistoryDirection::Before ? 0 : count - 1) * sizeof(boundary), sizeof(boundary));
        if (!equal(boundary, {result.key.counter, result.key.incoming})) return false;
        memcpy(page.entries, bytes, length);
    }
    if (!c.followNewest && result.total > livePage().info.total) c.newBelow = true;
    info.count = count; info.total = result.total;
    info.moreOlder = count && (info.direction == storage::HistoryDirection::Before ? result.more : info.bound.counter != 0);
    info.moreNewer = count && (info.direction == storage::HistoryDirection::After ? result.more : info.bound.counter != 0);
    info.first = info.direction == storage::HistoryDirection::After ? 0 : count > VisibleSpans ? count - VisibleSpans : 0;
    if (c.restoreKey.counter && count) {
        const auto* at = std::lower_bound(page.entries, page.entries + count, c.restoreKey, less);
        info.first = at == page.entries + count ? count - 1 : size_t(at - page.entries);
    }
    info.last = std::min(count, size_t(info.first) + VisibleSpans);
    c.buildIndex = info.first; c.phase = Phase::Preview;
    if (!count) c.publish = true;
    return true;
}
bool HistoryWindow::recordResult(const storage::Result& result, const uint8_t* bytes, size_t length) {
    auto& c = control();
    if (length < sizeof(storage::StoredRecordHeader) || length > ReadCapacity || c.spanCount >= VisibleSpans) return false;
    for (const auto offset : {offsetof(storage::StoredRecordHeader, incoming), offsetof(storage::StoredRecordHeader, read),
                              offsetof(storage::StoredRecordHeader, hasMessageId)}) if (bytes[offset] > 1) return false;
    storage::StoredRecordHeader header; memcpy(&header, bytes, sizeof(header));
    const auto key = c.phase == Phase::Full ? c.fullKey : candidate().entries[c.buildIndex];
    const auto offset = c.phase == Phase::Full ? c.fullOffset : 0;
    const uint64_t total = uint64_t(header.titleLength) + header.contentLength;
    const size_t payload = length - sizeof(header);
    if (total > storage::Budget::MaxStoredFile || header.status > uint8_t(LXMFStatus::UNCONFIRMED) ||
        !std::isfinite(header.timestamp) || header.counter != key.counter || header.incoming != key.incoming ||
        memcmp(header.incoming ? header.source : header.destination, c.peer, 16) ||
        memcmp(result.key.peer, c.peer, 16) || !equal({result.key.counter, result.key.incoming}, key) ||
        result.revision != header.revision || offset > total || payload > total - offset ||
        result.total != total || result.nextOffset != offset + payload || result.more != (result.nextOffset < total) ||
        (result.more && !payload)) return false;
    auto& frame = candidateFrame();
    Span row; row.counter = key.counter; row.recordRevision = header.revision;
    row.timestamp = header.timestamp <= 0 ? 0 : header.timestamp >= UINT32_MAX ? UINT32_MAX : uint32_t(header.timestamp);
    row.titleLength = header.titleLength; row.contentLength = header.contentLength;
    row.status = row.durableStatus = header.status;
    row.flags = (header.incoming ? Span::Incoming : 0) | (header.read ? Span::Read : 0);
    row.textOffset = c.usedText;
    const auto* raw = bytes + sizeof(header);
    size_t start = 0, end = payload;
    if (c.phase == Phase::Full && c.previousSlice) {
        if (c.readEnd < offset || c.readEnd > offset + payload) return false;
        end = c.readEnd - offset;
        // Backward navigation overlaps up to three bytes to recover a codepoint
        // boundary; it never needs an unbounded stack of previous slice offsets.
        while (offset && start < end && start < 3 && handheld::display::continuation(raw[start])) ++start;
    }
    row.sourceOffset = offset + start;
    const size_t capacity = c.phase == Phase::Full ? TextBytes - c.usedText - 1 : PreviewBytes;
    if (c.usedText + capacity >= TextBytes) return false;
    size_t at = start, written = 0;
    static constexpr char hex[] = "0123456789ABCDEF";
    while (at < end) {
        bool escape = false;
        const size_t fieldEnd = offset + at < header.titleLength ?
            std::min(end, size_t(header.titleLength - offset)) : end;
        const size_t count = handheld::display::codepoint(raw + at, fieldEnd - at,
            offset + fieldEnd == header.titleLength || offset + fieldEnd == total || c.previousSlice, escape);
        if (!count) break;
        const bool boundary = header.titleLength && offset + at == header.titleLength;
        const size_t needed = escape || raw[at] == '\t' ? 4 : count;
        if (written + needed + size_t(boundary) > capacity) break;
        if (boundary) frame.text[c.usedText + written++] = '\n';
        auto* target = frame.text + c.usedText + written;
        if (escape) {
            target[0] = '\\'; target[1] = 'x'; target[2] = hex[raw[at] >> 4]; target[3] = hex[raw[at] & 15];
            row.flags |= Span::Escaped;
        } else if (raw[at] == '\t') memset(target, ' ', 4);
        else memcpy(target, raw + at, count);
        written += needed; at += count;
    }
    if (at == start && (offset + start < total || c.previousSlice)) return false;
    row.sourceLength = at - start; row.textLength = written;
    frame.text[c.usedText + written] = 0;
    frame.spans[c.spanCount++] = row; c.usedText += written + 1;
    if (c.phase == Phase::Full || ++c.buildIndex == candidate().info.last) c.publish = true;
    return true;
}
void HistoryWindow::unavailableRecord(storage::Error error) {
    auto& c = control();
    const auto key = c.phase == Phase::Full ? c.fullKey : candidate().entries[c.buildIndex];
    Span row; row.counter = key.counter; row.flags = Span::Unavailable | (key.incoming ? Span::Incoming : 0);
    row.readError = error; row.textOffset = c.usedText;
    const char* text = error == storage::Error::Stale ? "Message no longer available" : "Message could not be read";
    row.textLength = strlen(text);
    memcpy(candidateFrame().text + c.usedText, text, row.textLength + 1);
    candidateFrame().spans[c.spanCount++] = row; c.usedText += row.textLength + 1;
    if (c.phase == Phase::Full || ++c.buildIndex == candidate().info.last) c.publish = true;
}
bool HistoryWindow::status(storage::HistoryEntry key, uint8_t desired, uint8_t durable,
                           bool pending, storage::Error error, bool suppressed) {
    if (desired > uint8_t(LXMFStatus::UNCONFIRMED) || durable > uint8_t(LXMFStatus::UNCONFIRMED)) return false;
    bool changed = false;
    for (size_t i = 0; i < spanCount(); ++i) {
        auto& row = _frames[control().active].spans[i];
        if (key.incoming || row.incoming() || row.counter != key.counter) continue;
        const uint8_t flags = (row.flags & ~(Span::StatusPending | Span::TxSuppressed | Span::StatusUnavailable)) | Span::StatusFresh |
            (pending ? Span::StatusPending : 0) | (suppressed ? Span::TxSuppressed : 0);
        changed |= row.status != desired || row.durableStatus != durable || row.statusError != error || row.flags != flags;
        row.status = desired; row.durableStatus = durable; row.statusError = error; row.flags = flags;
    }
    return changed;
}
bool HistoryWindow::statusUnavailable(storage::HistoryEntry key, storage::Error error) {
    if (key.incoming) return false;
    bool changed = false;
    for (size_t i = 0; i < spanCount(); ++i) {
        auto& row = _frames[control().active].spans[i];
        if (row.incoming() || row.counter != key.counter) continue;
        const auto oldStatus = row.status, oldDurable = row.durableStatus, oldFlags = row.flags;
        // Before the new publication is acknowledged, the previous bank still
        // holds its exact status observations. Preserve those if a fresh query
        // fails instead of regressing a known pending/delivered observation.
        const auto& previous = _pages[control().active ^ 1].value.info;
        if (!(row.flags & Span::StatusFresh) && control().publicationHeld && previous.identity == control().identity &&
            !memcmp(previous.peer, control().peer, 16)) {
            for (size_t j = 0; j < previous.spans; ++j) {
                const auto& old = _frames[control().active ^ 1].spans[j];
                if (old.incoming() || old.counter != key.counter) continue;
                const auto facts = retainKnownStatus(
                    {row.recordRevision, row.status, row.durableStatus,
                     bool(row.flags & Span::StatusPending), bool(row.flags & Span::TxSuppressed)},
                    {old.recordRevision, old.status, old.durableStatus,
                     bool(old.flags & Span::StatusPending), bool(old.flags & Span::TxSuppressed)});
                row.status = facts.desired; row.durableStatus = facts.durable;
                row.flags = (row.flags & ~(Span::StatusPending | Span::TxSuppressed)) |
                    (facts.pending ? Span::StatusPending : 0) | (facts.suppressed ? Span::TxSuppressed : 0);
                break;
            }
        }
        changed |= !(row.flags & Span::StatusUnavailable) || row.statusError != error;
        row.flags |= Span::StatusUnavailable; row.statusError = error;
        changed |= row.status != oldStatus || row.durableStatus != oldDurable || row.flags != oldFlags;
    }
    return changed;
}

} // namespace handheld::history
