#include "MessagesScreen.h"
#include "StorageWindowAdapter.h"
#include "Theme.h"
#include "reticulum/AnnounceManager.h"
#include "protocol/ProtocolBackend.h"

namespace {
constexpr int NavigationHeight = 18;
constexpr int NavigationGap = 8;
constexpr int NavigationWidth = 46;
}

void MessagesScreen::onEnter() {
    _showingContext = false; _visible = true; _pageFocus = -1;
    // Cardputer identity switches commit through an orderly restart.
    _conversations.resume(1);
}

std::string MessagesScreen::peerHex(size_t index) const {
    const auto* row = _conversations.row(index);
    if (!row) return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string peer(32, '0');
    for (size_t i = 0; i < 16; ++i) {
        peer[i * 2] = hex[row->peer[i] >> 4]; peer[i * 2 + 1] = hex[row->peer[i] & 15];
    }
    return peer;
}

std::string MessagesScreen::peerLabel(const std::string& peer) const {
    std::string label;
    if (_am) {
        const auto* node = _am->findNodeByHex(peer);
        if (node) label = node->name;
        if (label.empty()) label = _am->lookupName(peer);
    }
    if (label.empty()) label = peer.size() >= 8 ? peer.substr(0, 4) + ":" + peer.substr(4, 4) : peer;
    return label;
}

bool MessagesScreen::pollConversations(bool allowAdmission) {
    if (!_lxmf) return false;
    const auto publication = _conversations.revision(), statuses = _conversations.statusRevision();
    const auto state = _conversations.state(); const auto error = _conversations.error();
    const bool updated = _conversations.updated();
    const auto pageFocus = _pageFocus;
    if (allowAdmission) {
        _conversations.observeRevision(_lxmf->storeRevision());
        if (_backend) _conversations.observeStatusRevision(_backend->lxmfStatusRevision());
        _needsRefresh = false;
    }
    handheld::canvas::pollStorageWindow(_conversations, *_lxmf, _backend, _visible, allowAdmission,
        [](const Conversations::Query& query) {
            handheld::storage::RecordKey key; memcpy(key.peer, query.selector.cursor.peer, 16);
            key.counter = query.selector.counter; key.incoming = query.selector.incoming;
            return key;
        },
        [&](const Conversations::Query& query, const handheld::storage::RecordKey&) {
            return query.kind == Conversations::Kind::Page ?
                _lxmf->requestConversationPage(query.selector.cursor, query.hasCursor, query.order, query.direction, Conversations::PageSize) :
                _lxmf->requestConversation(query.selector);
        });
    if (!_conversations.visible() || _conversations.statusReady())
        _conversations.acknowledgePublication(_conversations.revision());
    if (_pageFocus >= 0 && !_conversations.loading() && !pageEnabled(_pageFocus))
        _pageFocus = pageEnabled(2) ? 2 : pageEnabled(1) ? 1 : -1;
    return publication != _conversations.revision() || statuses != _conversations.statusRevision() ||
        state != _conversations.state() || error != _conversations.error() || updated != _conversations.updated() ||
        pageFocus != _pageFocus;
}

void MessagesScreen::renderList(M5Canvas& canvas, int y, int height) {
    Theme::useUiFont(canvas);
    const int listHeight = height - NavigationHeight - 4;
    const size_t visible = std::max(1, listHeight / Theme::LIST_ROW_H);
    const auto count = _conversations.count(), selected = _conversations.selectedIndex();
    uint32_t offset = std::min(_conversations.scrollOffset(), uint32_t(count > visible ? count - visible : 0));
    if (selected < count) {
        if (selected < offset) offset = selected;
        else if (selected >= offset + visible) offset = selected - visible + 1;
    }
    _conversations.setScrollOffset(offset);
    for (size_t i = offset; i < count && i < offset + visible; ++i) {
        const auto& row = *_conversations.row(i);
        auto label = peerLabel(peerHex(i));
        if (row.flags & handheld::storage::ConversationView::Unavailable) label += " [unavailable]";
        else if (row.unreadCount) label += " [" + std::to_string(row.unreadCount) + "]";
        ScrollList::renderRow(canvas, label, 0, y + (i - offset) * Theme::LIST_ROW_H,
                              Theme::CONTENT_W, _pageFocus < 0 && i == selected);
    }
    if (!count) {
        canvas.setTextColor(Theme::TEXT_SECONDARY);
        canvas.drawString(_conversations.state() == Conversations::State::Retrying ? "Read failed; retrying..." :
            _conversations.state() == Conversations::State::Exhausted ? "Restart needed to read list" :
            _conversations.loading() ? "Loading conversations..." : "No conversations yet", 8, y + 1);
    }
    renderNavigation(canvas, y + height - NavigationHeight);
}

void MessagesScreen::renderNavigation(M5Canvas& canvas, int y) {
    constexpr int groupWidth = 4 * NavigationWidth + 3 * NavigationGap;
    for (int action = 0; action < 4; ++action) {
        const int x = (Theme::CONTENT_W - groupWidth) / 2 + action * (NavigationWidth + NavigationGap);
        const bool enabled = pageEnabled(action), focused = enabled && _pageFocus == action;
        canvas.fillRoundRect(x, y, NavigationWidth, NavigationHeight, 3,
                             focused ? Theme::SELECTION_BG : Theme::BG_ELEVATED);
        canvas.drawRoundRect(x, y, NavigationWidth, NavigationHeight, 3,
                             focused ? Theme::ACCENT : Theme::BORDER);
        const uint16_t color = !enabled ? Theme::TEXT_MUTED : focused ? Theme::ACCENT : Theme::TEXT_PRIMARY;
        const bool doubled = action == 0 || action == 3, right = action >= 2;
        const int width = doubled ? 14 : 6;
        const int left = x + (NavigationWidth - width) / 2;
        // Two-pixel strokes, centered by their actual ink bounds.
        for (int chevron = 0; chevron < (doubled ? 2 : 1); ++chevron) {
            for (int row = 0; row < 10; ++row) {
                const int slope = row < 5 ? row : 9 - row;
                canvas.drawFastHLine(left + chevron * 8 + (right ? slope : 4 - slope),
                                    y + (NavigationHeight - 10) / 2 + row, 2, color);
            }
        }
    }
}

void MessagesScreen::showContextMenu(int idx) {
    if (idx < 0 || size_t(idx) >= _conversations.count()) return;
    _contextPeerHex = peerHex(idx);

    _contextIsContact = false;
    if (_am) {
        const DiscoveredNode* node = _am->findNodeByHex(_contextPeerHex);
        if (node && node->saved) _contextIsContact = true;
    }

    _contextList.clear();
    _contextList.addItem("Message");
    if (!_contextIsContact) {
        _contextList.addItem("Add Contact");
    }
    _contextList.addItem("Delete History");
    _contextList.addItem("Back");
    _contextList.setSelected(0);
    _showingContext = true;
}

void MessagesScreen::executeContextAction() {
    const std::string& action = _contextList.getSelectedItem();

    if (action == "Message") {
        const std::string peer = _contextPeerHex;
        exitContextMenu();
        if (_openCb) _openCb(peer);
    } else if (action == "Add Contact") {
        const std::string peer = _contextPeerHex;
        exitContextMenu();
        if (_addContactCb) _addContactCb(peer);
    } else if (action == "Delete History") {
        if (_lxmf && _backend && !_deleteTicket.valid()) {
            rs::Bytes peer; peer.assignHex(_contextPeerHex.c_str());
            if (peer.size() == sizeof(_deletePeer) && _backend->lxmfBeginPeerDelete(peer.data())) {
                const auto submitted = _lxmf->requestDelete(_contextPeerHex);
                if (submitted.accepted()) {
                    _deleteTicket = submitted.ticket; _deleteSettled = false;
                    memcpy(_deletePeer, peer.data(), sizeof(_deletePeer));
                    _deleteNotice = "Deleting history...";
                } else {
                    handheld::storage::Result failed;
                    failed.error = handheld::storage::Error::Unavailable;
                    _backend->lxmfFinishPeerDelete(peer.data(), failed);
                    _deleteNotice = "Not deleted; try again";
                }
            } else _deleteNotice = "Deletion busy; try again";
            _deleteNoticeSince = millis();
        }
        exitContextMenu();
    } else {
        exitContextMenu();
    }
}

bool MessagesScreen::pollDeletion() {
    if (!_deleteTicket.valid()) {
        if (_deleteNotice && uint32_t(millis() - _deleteNoticeSince) >= 4000) {
            _deleteNotice = nullptr;
            return true;
        }
        return false;
    }
    if (!_lxmf || !_backend) return false;
    if (!_deleteSettled) {
        handheld::storage::Result result;
        if (!_lxmf->pollStorageResult(_deleteTicket, result)) return false;
        // Settle the protocol fence exactly once, then retain the terminal credit
        // until storage confirms release, even while this screen is hidden.
        _backend->lxmfFinishPeerDelete(_deletePeer, result);
        _deleteSettled = true;
        _deleteNotice = result.outcome == handheld::storage::Outcome::Committed ?
            "History deleted" : "History not deleted";
        _deleteNoticeSince = millis();
        if (result.outcome == handheld::storage::Outcome::Committed) _conversations.refresh();
    }
    if (_lxmf->releaseStorageResult(_deleteTicket)) {
        _deleteTicket = {}; _deleteSettled = false;
    }
    return true;
}

void MessagesScreen::exitContextMenu() {
    _showingContext = false;
    _contextPeerHex.clear();
}

void MessagesScreen::render(M5Canvas& canvas) {
    int y = Theme::CONTENT_Y;

    const int headerH = Theme::SECTION_HEADER_H;
    canvas.fillRect(0, y, Theme::CONTENT_W, headerH, Theme::BG_SURFACE);
    canvas.fillRect(0, y + 2, 3, headerH - 4, Theme::ACCENT);
    canvas.setTextColor(Theme::ACCENT);
    Theme::useUiFont(canvas);
    bool unavailable = false;
    for (size_t i = 0; i < _conversations.count(); ++i)
        unavailable |= bool(_conversations.row(i)->flags & (handheld::storage::ConversationView::Unavailable |
                                                          handheld::storage::ConversationView::StatusUnavailable));
    const char* notice = _deleteNotice ? _deleteNotice : !_conversations.freshnessAvailable() ? "Refresh needed (R)" :
        unavailable || _conversations.state() == Conversations::State::Retrying ? "Read failed; R to retry" :
        nullptr;
    const auto heading = notice ? std::string(notice) : "Page " + std::to_string(_conversations.pageNumber());
    canvas.drawString(heading.c_str(), 8, y + 2);
    canvas.drawFastHLine(0, y + headerH, Theme::CONTENT_W, Theme::DIVIDER);
    y += headerH + 2;

    if (_showingContext) {
        Theme::useUiFont(canvas);
        canvas.setTextColor(Theme::PRIMARY);
        std::string label;
        if (_am) {
            const DiscoveredNode* node = _am->findNodeByHex(_contextPeerHex);
            if (node && !node->name.empty()) label = node->name;
            if (label.empty()) label = _am->lookupName(_contextPeerHex);
        }
        if (label.empty()) label = _contextPeerHex.substr(0, 8);
        canvas.drawString(label.c_str(), 8, y);
        y += Theme::LIST_ROW_H + 2;
        canvas.drawFastHLine(0, y, Theme::SCREEN_W, Theme::DIVIDER);
        y += 2;

        _contextList.render(canvas, 0, y, Theme::CONTENT_W, Theme::CONTENT_H - (y - Theme::CONTENT_Y));
    } else {
        renderList(canvas, y, Theme::CONTENT_H - (y - Theme::CONTENT_Y));
    }
    Theme::useSmallFont(canvas);
}

bool MessagesScreen::pageEnabled(int action) const {
    return action >= 0 && action < 4 && !_conversations.loading() &&
        (action < 2 ? _conversations.canPrevious() : _conversations.canNext());
}

void MessagesScreen::activatePage(int action) {
    if (!pageEnabled(action)) return;
    switch (action) {
        case 0: _conversations.first(); break;
        case 1: _conversations.previous(); break;
        case 2: _conversations.nextPage(); break;
        case 3: _conversations.last(); break;
    }
}

void MessagesScreen::movePageFocus(int direction) {
    for (int action = _pageFocus + direction; action >= 0 && action < 4; action += direction) {
        if (pageEnabled(action)) { _pageFocus = action; return; }
    }
}

bool MessagesScreen::handleKey(const KeyEvent& event) {
    if (event.repeat && (event.backspace || event.forwardDelete)) return true;
    if (_showingContext) {
        if (event.escape || event.backspace) {
            exitContextMenu();
            return true;
        }
        if (event.navUp()) { _contextList.scrollUp(); return true; }
        if (event.navDown()) { _contextList.scrollDown(); return true; }
        if (event.enter) {
            executeContextAction();
            return true;
        }
        return true;
    }

    const size_t selected = _conversations.selectedIndex();
    const size_t count = _conversations.count();
    if (_pageFocus >= 0) {
        if (event.escape || event.backspace || event.navUp()) {
            if (!event.repeat) _pageFocus = -1;
            return true;
        }
        if (event.navLeft() || event.navRight()) {
            movePageFocus(event.navLeft() ? -1 : 1);
            return true;
        }
        if (event.enter) {
            if (!event.repeat) activatePage(_pageFocus);
            return true;
        }
        if (event.navDown() || event.forwardDelete) return true;
    }
    if (event.left || event.right) {
        if (!event.repeat) {
            activatePage(event.left ? (event.shift ? 0 : 1) : (event.shift ? 3 : 2));
        }
        return true;
    }
    if (event.navUp() || event.navDown()) {
        if (count) {
            // A removed/off-page peer stays selected until explicit navigation.
            // The first tap restores a visible selection without skipping a page.
            if (selected >= count) {
                _conversations.select(event.navUp() ? count - 1 : 0);
            } else if (event.navUp()) {
                if (selected > 0 && selected < count) _conversations.select(selected - 1);
                else _conversations.select(0);
            } else if (selected + 1 < count) _conversations.select(selected + 1);
            else if (!event.repeat) _pageFocus = pageEnabled(2) ? 2 : pageEnabled(1) ? 1 : -1;
        }
        return true;
    }
    if (!event.ctrl && !event.repeat && (event.character == 'r' || event.character == 'R')) {
        _conversations.refresh(); return true;
    }
    if (!event.ctrl && !event.repeat && (event.character == 'n' || event.character == 'N')) {
        _pageFocus = -1; _conversations.first(); return true;
    }
    if (event.enter) {
        if (!event.repeat && selected < count && _openCb) {
            const auto peer = peerHex(selected); // Stable through reentrant navigation.
            _openCb(peer);
        }
        return true;
    }
    if (event.forwardDelete) {
        if (selected < count) showContextMenu(selected);
        return true;
    }

    return false;
}
