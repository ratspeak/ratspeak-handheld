#include "MessageView.h"
#include "StorageWindowAdapter.h"
#include "reticulum/MessageStatusDetail.h"
#include "Theme.h"
#include "reticulum/AnnounceManager.h"
#include "protocol/ProtocolBackend.h"
#include "runtime/ResourceBudget.h"
#include <algorithm>
#include <new>
#include <time.h>

namespace {
using History = handheld::history::HistoryWindow;
constexpr int CHAT_HEADER_H = Theme::SECTION_HEADER_H;
constexpr int CHAT_INPUT_H = Theme::CHAR_H + 4;

int visibleChatLines() {
    int chatH = Theme::CONTENT_H - CHAT_HEADER_H - CHAT_INPUT_H - 4;
    return chatH > 0 ? chatH / Theme::CHAR_H : 1;
}

void drawFittedHeader(M5Canvas& canvas, const std::string& text, int x, int y, int maxW) {
    if (canvas.textWidth(text.c_str()) <= maxW) {
        canvas.drawString(text.c_str(), x, y);
        return;
    }
    char buf[64];
    int len = std::min((int)text.length(), (int)sizeof(buf) - 3);
    while (len > 0) {
        memcpy(buf, text.c_str(), len);
        buf[len] = '.';
        buf[len + 1] = '.';
        buf[len + 2] = '\0';
        if (canvas.textWidth(buf) <= maxW) {
            canvas.drawString(buf, x, y);
            return;
        }
        len--;
    }
}

// Walk only the fixed published text arena. No wrapped-line vector or complete
// message string is retained. Sanitized UTF-8 scalars stay intact at every wrap.
template <typename Visitor>
int visitChatLines(const History& history, Visitor visit) {
    constexpr size_t width = (Theme::CONTENT_W - 4) / Theme::CHAR_W;
    int line = 0;
    auto emit = [&](size_t row, const char* text, uint16_t color) { visit(row, line++, text, color); };
    for (size_t index = 0; index < history.spanCount(); ++index) {
        const auto& row = *history.span(index);
        char heading[40];
        char timestamp[16] = "--:--";
        if (row.timestamp > 1700000000) {
            const time_t epoch = row.timestamp;
            if (const auto* tm = localtime(&epoch)) snprintf(timestamp, sizeof(timestamp), "%02d:%02d", tm->tm_hour, tm->tm_min);
        } else if (row.timestamp && millis() / 1000 > row.timestamp) {
            const auto ago = millis() / 1000 - row.timestamp;
            if (ago < 60) snprintf(timestamp, sizeof(timestamp), "%lus", ago);
            else if (ago < 3600) snprintf(timestamp, sizeof(timestamp), "%lum", ago / 60);
            else if (ago < 86400) snprintf(timestamp, sizeof(timestamp), "%luh", ago / 3600);
        }
        const char* phase = !row.incoming() && !row.unavailable() && history.statusReady() ?
            messageStatusLabel(static_cast<LXMFStatus>(row.status)) : nullptr;
        snprintf(heading, sizeof(heading), "%s %s%s%s%s%s", timestamp, row.incoming() ? "them" : "you",
                 phase ? " [" : "", phase ? phase : "", phase ? "]" : "",
                 history.focusedSpan() == index ? "  >" : "");
        emit(index, heading, Theme::TEXT_SECONDARY);
        const auto color = row.unavailable() || (history.statusReady() && row.status == uint8_t(LXMFStatus::FAILED)) ? Theme::ERROR :
            history.statusReady() && row.status == uint8_t(LXMFStatus::UNCONFIRMED) ? Theme::WARNING : Theme::PRIMARY;
        const char* source = history.text(index);
        size_t at = 0;
        do {
            char segment[width + 1]; size_t used = 0;
            while (source[at] && source[at] != '\n') {
                const auto byte = uint8_t(source[at]);
                const size_t scalar = byte < 0x80 ? 1 : byte < 0xe0 ? 2 : byte < 0xf0 ? 3 : 4;
                if (used + scalar > width) break;
                memcpy(segment + used, source + at, scalar); used += scalar; at += scalar;
            }
            const bool newline = source[at] == '\n';
            if (newline) ++at;
            segment[used] = 0; emit(index, segment, color);
            if (!source[at]) {
                if (newline) emit(index, "", color);
                break;
            }
        } while (true);
        if (!row.incoming() && !row.unavailable()) {
            const char* detail = !history.statusReady() || (row.flags & History::Span::StatusUnavailable) ? nullptr :
                messageStatusDetail(LXMFStatus(row.status), row.flags & History::Span::StatusPending,
                                    row.statusError, row.flags & History::Span::TxSuppressed);
            if (detail) emit(index, detail, Theme::WARNING);
        }
        if (history.mode() == History::Mode::Chat && row.more()) emit(index, "Read full: Tab, then Enter", Theme::ACCENT);
    }
    return line;
}

int totalChatLines(const History& history) {
    return visitChatLines(history, [](size_t, int, const char*, uint16_t) {});
}
}

bool MessageView::prepareDraft(String& identity) {
    using Budget = handheld::ResourceBudget;
    static_assert(2 * (DraftLength + 1) + 2 * 33 + sizeof(TextInput) +
                  3 * sizeof(std::string) + 32 <= Budget::CanvasDraft,
                  "Review complete Canvas draft ownership");
    if (!_backend || _peerHex.size() != 32 || _input.getText().size() > DraftLength ||
        _retainedDraft.size() > DraftLength || _retainedPeer.size() > 32 ||
        _retainedIdentity.size() > 32) return false;
    const bool allocating = _input.textCapacity() < DraftLength ||
        _retainedDraft.capacity() < DraftLength || _retainedPeer.capacity() < 32 ||
        _retainedIdentity.capacity() < 32;
    // Preserve the established internal floor before reserving either owner.
    // SDK exceptions are enabled, but their emergency pool is zero: do not use
    // throwing allocation as the normal test for an already exhausted heap.
    if (ESP.getFreeHeap() < Budget::CardInternalFree + (allocating ? Budget::CanvasDraft : 0))
        return false;
    try {
        if (!_input.reserveTextCapacity(DraftLength)) return false;
        if (_retainedDraft.capacity() < DraftLength) _retainedDraft.reserve(DraftLength);
        if (_retainedPeer.capacity() < 32) _retainedPeer.reserve(32);
        if (_retainedIdentity.capacity() < 32) _retainedIdentity.reserve(32);
        identity = _backend->destinationHashHex();
        return identity.length() == 32;
    } catch (const std::bad_alloc&) { return false; }
}

void MessageView::onEnter() {
    _visible = true; _readRequested = true; _readRetryAt = millis();
    String identity;
    _draftReady = prepareDraft(identity);
    if (!_draftReady) {
        _input.setActive(false);
        _sendNotice = "Draft unavailable; Enter retries";
        refreshMessages();
        return;
    }
    _input.setActive(true);
    _sendNotice = _sendTicket.valid() && _retainedPeer == _peerHex &&
        identity.c_str() == _retainedIdentity ? "Saving message..." : nullptr;
    if (_retainedPeer == _peerHex && identity.c_str() == _retainedIdentity) {
        _input.setText(_retainedDraft);
        if (_sendTicket.valid() && !_submissionEdited) _submittedRevision = _input.revision();
    } else {
        _input.clear();
    }
    _input.setMaxLength(DraftLength);
    _input.setSubmitCallback([this](const std::string& text) {
        sendCurrentInput();
    });
    refreshMessages();
}

void MessageView::refreshMessages() {
    if (!_lxmf || _peerHex.empty()) return;
    rs::Bytes peer; peer.assignHex(_peerHex.c_str());
    if (peer.size() != 16) return;
    if (_history.mode() == History::Mode::Closed || memcmp(_history.peer(), peer.data(), 16)) {
        // Cardputer identity replacement performs an orderly restart. This
        // presentation belongs to the one identity established for this boot.
        _history.open(peer.data(), 1);
        _history.setScrollOffset(UINT32_MAX);
    } else _history.refresh();
    _needsRefresh = false;
}

void MessageView::render(M5Canvas& canvas) {
    int baseY = Theme::CONTENT_Y;

    // Header: peer name or hash.
    std::string header;
    if (_am) {
        const DiscoveredNode* node = _am->findNodeByHex(_peerHex);
        if (node && !node->name.empty()) header = node->name;
        if (header.empty()) header = _am->lookupName(_peerHex);
    }
    if (header.empty()) {
        if (_peerHex.size() >= 8) {
            header = _peerHex.substr(0, 4) + ":" + _peerHex.substr(4, 4);
        } else {
            header = _peerHex;
        }
    }
    canvas.fillRect(0, baseY, Theme::CONTENT_W, CHAT_HEADER_H, Theme::BG_SURFACE);
    canvas.fillRect(0, baseY + 2, 3, CHAT_HEADER_H - 4, Theme::PRIMARY);
    Theme::useUiFont(canvas);
    canvas.setTextColor(Theme::TEXT_PRIMARY);
    drawFittedHeader(canvas, _sendNotice ? _sendNotice : !_history.freshnessAvailable() ? "History needs manual refresh" :
                    _history.statusRefreshDelayed() ?
                        (_history.mode() == History::Mode::Full || _history.focusedSpan() < _history.spanCount() ?
                            "Status delayed; R retries" : "Status delayed; Tab, R") : header,
                    8, baseY + 2, Theme::CONTENT_W - 16);
    canvas.drawFastHLine(0, baseY + CHAT_HEADER_H, Theme::CONTENT_W, Theme::DIVIDER);

    Theme::useSmallFont(canvas);
    const int chatY = baseY + CHAT_HEADER_H + 2;
    const int inputY = baseY + Theme::CONTENT_H - CHAT_INPUT_H;
    const int visible = visibleChatLines();
    const int maximum = std::max(0, totalChatLines(_history) - visible);
    _history.setScrollOffset(_history.mode() == History::Mode::Chat && _history.followsNewest() ?
        uint32_t(maximum) : std::min(_history.scrollOffset(), uint32_t(maximum)));
    visitChatLines(_history, [&](size_t row, int line, const char* text, uint16_t color) {
        if (line < int(_history.scrollOffset()) || line >= int(_history.scrollOffset()) + visible) return;
        const int y = chatY + (line - _history.scrollOffset()) * Theme::CHAR_H;
        if (_history.focusedSpan() == row && _history.mode() == History::Mode::Chat)
            canvas.fillRect(0, y, Theme::CONTENT_W, Theme::CHAR_H, Theme::SELECTION_BG);
        canvas.setTextColor(color); canvas.drawString(text, 2, y);
    });
    if (!_history.spanCount()) {
        canvas.setTextColor(Theme::TEXT_SECONDARY);
        const char* text = _history.state() == History::State::Retrying ? "Read failed; retrying..." :
                           _history.loading() ? "Loading messages..." : "No messages yet";
        canvas.drawString(text, (Theme::CONTENT_W - canvas.textWidth(text)) / 2,
                          chatY + (inputY - chatY - Theme::CHAR_H) / 2);
    }
    if (_history.newBelow()) {
        canvas.setTextColor(Theme::WARNING);
        canvas.drawString("[new below]", Theme::CONTENT_W - 70, inputY - Theme::CHAR_H - 2);
    }
    canvas.drawFastHLine(0, inputY - 2, Theme::CONTENT_W, Theme::DIVIDER);
    if (_history.mode() == History::Mode::Full) {
        canvas.setTextColor(Theme::TEXT_SECONDARY);
        canvas.drawString("Arrows: read  R: refresh", 2, inputY + 2);
    } else if (_history.focusedSpan() < _history.spanCount()) {
        canvas.setTextColor(Theme::TEXT_SECONDARY);
        canvas.drawString("Enter: read  R: refresh", 2, inputY + 2);
    } else if (_draftReady) {
        _input.render(canvas, 0, inputY, Theme::CONTENT_W);
    } else {
        canvas.setTextColor(Theme::TEXT_SECONDARY);
        canvas.drawString("Draft unavailable; Enter retries", 2, inputY + 2);
    }
}

bool MessageView::handleKey(const KeyEvent& event) {
    const bool full = _history.mode() == History::Mode::Full;
    if (!_draftReady && !full && event.enter && !event.repeat) {
        onEnter();
        return true;
    }
    if (event.escape || (event.backspace &&
                        (full || _history.focusedSpan() < _history.spanCount()))) {
        if (event.repeat) return true;
        if (full) { _history.backToChat(); _history.focusSpan(History::VisibleSpans); _input.setActive(true); }
        else if (_history.focusedSpan() < _history.spanCount()) {
            _history.focusSpan(History::VisibleSpans); _input.setActive(true);
        } else if (_backCb) _backCb();
        return true;
    }
    if (!full && event.tab && !event.repeat) {
        const auto focus = _history.focusedSpan();
        _history.focusSpan(event.shift ? (focus == 0 ? History::VisibleSpans :
            focus < _history.spanCount() ? focus - 1 : _history.spanCount() - 1) :
            focus < _history.spanCount() ? focus + 1 : 0);
        _input.setActive(_history.focusedSpan() == History::VisibleSpans);
        bool located = false;
        visitChatLines(_history, [&](size_t row, int line, const char*, uint16_t) {
            if (!located && row == _history.focusedSpan()) { _history.setScrollOffset(line); located = true; }
        });
        const uint32_t maximum = std::max(0, totalChatLines(_history) - visibleChatLines());
        _history.setViewportAtNewest(_history.scrollOffset() >= maximum);
        return true;
    }
    if (event.up || event.down || (full && (event.left || event.right))) {
        const uint32_t maximum = std::max(0, totalChatLines(_history) - visibleChatLines());
        const uint32_t offset = std::min(_history.scrollOffset(), maximum);
        if (event.up && offset) {
            _history.setScrollOffset(offset - 1); _history.setViewportAtNewest(false);
        } else if (event.down && offset < maximum) {
            _history.setScrollOffset(offset + 1); _history.setViewportAtNewest(offset + 1 == maximum);
        }
        else if (event.up || event.left) {
            if (_history.older()) _history.setScrollOffset(UINT32_MAX);
        } else if (_history.newer()) _history.setScrollOffset(0);
        return true;
    }
    if ((full || _history.focusedSpan() < _history.spanCount()) && !event.ctrl && !event.repeat &&
        (event.character == 'r' || event.character == 'R')) {
        _history.refresh(); return true;
    }
    if (full) return true; // The retained composer is not edited in the reader.
    if (_history.focusedSpan() < _history.spanCount()) {
        if (event.enter && !event.repeat) {
            const auto selected = _history.focusedSpan();
            if (_history.span(selected)->unavailable()) _history.refresh();
            else _history.openFull(selected);
        }
        return true;
    }
    // Backspace leaves an empty composer; Fn+Backspace remains forward Delete.
    if (event.backspace && _input.getText().empty()) {
        if (!event.repeat && _backCb) _backCb();
        return true;
    }
    if (!_draftReady) return true;
    const auto revision = _input.revision();
    if (_input.handleKey(event)) {
        if (_input.revision() != revision && _peerHex == _retainedPeer && _backend &&
            _backend->destinationHashHex().c_str() == _retainedIdentity) {
            _submissionEdited = true; _retainedDraft = _input.getText();
        }
        if (_input.revision() != revision && !_sendTicket.valid()) _sendNotice = nullptr;
        return true;
    }
    return false;
}

void MessageView::notifyStatusChange(const std::string& peerHex, uint32_t, LXMFStatus) {
    if (_peerHex == peerHex && _backend) _history.observeStatusRevision(_backend->lxmfStatusRevision());
}

void MessageView::notifyNewMessage(const handheld::storage::RecordKey& key) {
    rs::Bytes peer; peer.assignHex(_peerHex.c_str());
    if (peer.size() != sizeof(key.peer) || memcmp(peer.data(), key.peer, sizeof(key.peer)) != 0) return;
    _history.notifyArrival();
    if (_visible) _readRequested = true;
}

bool MessageView::pollHistory(bool allowAdmission) {
    if (!_lxmf) return false;
    using namespace handheld::storage;
    const auto publication = _history.revision(), statusPublication = _history.statusRevision();
    const auto state = _history.state();
    const auto error = _history.error();
    if (allowAdmission) {
        if (_visible && _needsRefresh) refreshMessages();
        _history.observeHistoryRevision(_lxmf->historyRevision());
        if (_backend) _history.observeStatusRevision(_backend->lxmfStatusRevision());
    }
    handheld::canvas::pollStorageWindow(_history, *_lxmf, _backend, _visible, allowAdmission,
        [](const History::Query& query) {
            RecordKey key; memcpy(key.peer, query.peer, 16);
            key.counter = query.cursor.counter; key.incoming = query.cursor.incoming;
            return key;
        },
        [&](const History::Query& query, const RecordKey& key) {
            return query.kind == History::Kind::Page ?
                _lxmf->requestHistoryPage(_peerHex, query.cursor, query.direction) :
                _lxmf->requestRecord(key, query.offset, query.capacity);
        });
    // The previous bank also retains known delivery facts until the first
    // status projection/fallback finishes. Abandoned views can release now;
    // same-view bodies must keep that fallback even without text pointers.
    if (!_history.visible() || _history.statusReady())
        _history.acknowledgePublication(_history.revision());
    return publication != _history.revision() || statusPublication != _history.statusRevision() ||
        state != _history.state() || error != _history.error();
}

bool MessageView::pollReadMarker() {
    if (!_lxmf) return false;
    bool changed = false;
    if (_readTicket.valid()) {
        handheld::storage::Result result;
        if (!_lxmf->pollStorageResult(_readTicket, result)) return false;
        _lxmf->releaseStorageResult(_readTicket);
        _readTicket = {};
        if (result.outcome == handheld::storage::Outcome::Committed) {
            if (_unreadCb) _unreadCb();
            // Per-row read flags are snapshots; the committed marker updates the badge.
        } else if (_visible && _peerHex == _readPeer) {
            _sendNotice = "Couldn't mark read"; _sendNoticeSince = millis();
        }
        changed = true;
    }
    // One ordered marker at a time. A new arrival during an outstanding marker
    // requests another pass; leaving the conversation stops new admission.
    if (_visible && _readRequested && !_peerHex.empty() &&
        int32_t(uint32_t(millis()) - _readRetryAt) >= 0) {
        const auto submitted = _lxmf->requestMarkRead(_peerHex);
        if (submitted.accepted()) {
            _readTicket = submitted.ticket;
            strlcpy(_readPeer, _peerHex.c_str(), sizeof(_readPeer));
            _readRequested = false;
        } else if (submitted.rejection == handheld::storage::Rejection::Busy) {
            _readRetryAt = millis() + 250;
        } else {
            _readRequested = false;
            _sendNotice = "Couldn't mark read"; _sendNoticeSince = millis();
            changed = true;
        }
    }
    return changed;
}

bool MessageView::pollSubmission() {
    if (!_backend) return false;
    bool changed = false;
    if (_sendNotice && _draftReady && !_sendTicket.valid() && uint32_t(millis() - _sendNoticeSince) >= 4000) {
        _sendNotice = nullptr;
        changed = true;
    }
    if (!_sendTicket.valid()) return changed;
    handheld::outgoing::InitialResult result;
    const auto state = _backend->lxmfPoll(_sendTicket, result);
    if (state == handheld::outgoing::Poll::Pending) return changed;
    const auto ticket = _sendTicket;
    _sendTicket = {};
    const bool saved = state == handheld::outgoing::Poll::Ready &&
                       result.outcome == handheld::storage::Outcome::Committed;
    const bool sameConversation = _peerHex == _retainedPeer &&
        _backend->destinationHashHex().c_str() == _retainedIdentity;
    const bool sameDraft = sameConversation &&
        !_submissionEdited && _input.revision() == _submittedRevision &&
        _input.revision() != UINT64_MAX;
    if (state == handheld::outgoing::Poll::Ready) _backend->lxmfAcknowledge(ticket);
    if (saved) {
        if (sameDraft) _input.clear();
        if (!_submissionEdited) { _retainedDraft.clear(); _retainedPeer.clear(); }
        _sendNotice = sameConversation ? (result.txSuppressed ? "Saved; send cancelled" : "Message saved") : nullptr;
    } else {
        _sendNotice = sameConversation ? "Not saved; draft kept" : nullptr;
    }
    _sendNoticeSince = millis();
    _needsRefresh = true;
    return true;
}

void MessageView::sendCurrentInput() {
    if (!_backend || !_lxmf || _peerHex.empty() || _sendTicket.valid() ||
        _input.revision() == UINT64_MAX) return;
    const auto& text = _input.getText();
    if (text.empty()) return;
    String identity;
    if (!prepareDraft(identity)) {
        _sendNotice = "Not saved; draft kept";
        _sendNoticeSince = millis();
        return;
    }
    rs::Bytes destination;
    try { destination.assignHex(_peerHex.c_str()); }
    catch (const std::bad_alloc&) {
        _sendNotice = "Not saved; draft kept";
        _sendNoticeSince = millis();
        return;
    }
    if (destination.size() != 16) return;
    const auto submitted = _backend->lxmfSubmit(destination.data(), nullptr, 0,
        reinterpret_cast<const uint8_t*>(text.data()), text.size(), false);
    if (!submitted.accepted()) {
        _sendNotice = "Not saved; try again";
        _sendNoticeSince = millis();
        return;
    }
    _sendTicket = submitted.ticket;
    _submittedRevision = _input.revision();
    _submissionEdited = false;
    _retainedDraft = text;
    _retainedPeer = _peerHex;
    _retainedIdentity = identity.c_str();
    _sendNotice = "Saving message...";
    // History is populated only from committed records; no synthetic timestamp,
    // outgoing body cache or acknowledgement of mere queue admission.
}
