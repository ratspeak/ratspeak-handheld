#include "MessagesScreen.h"
#include "StorageWindowAdapter.h"
#include "Theme.h"
#include "PageNavigation.h"
#include "reticulum/AnnounceManager.h"
#include "protocol/ProtocolBackend.h"
#include "ui/RrcCommandId.h"

namespace {
constexpr int NavigationHeight = 18;
}

void MessagesScreen::switchFamily() {
    exitContextMenu(); _rrc.navigation.direct(!_rrc.navigation.direct());
    _rrc.refresh();
    if (_rrc.navigation.direct()) _conversations.resume(1);
    else _conversations.close();
}
void MessagesScreen::pollRrc() {
    if (!_backend) return;
    _rrc.observe(_backend->rrcStatus(),1);
    if (!_rrc.needsRows()) return;
    using Page = handheld::ui::RrcNavigation::Page;
    using namespace handheld::rrc;
    const auto view = _rrc.beginRows();
    if (!_rrc.completeRows(view,true)) return;
    switch (_rrc.navigation.page()) {
    case Page::Hubs: { HubView rows[HubViewCapacity]; const auto count = _backend->rrcHubs(rows,HubViewCapacity); _rrc.rows(rows,count); break; }
    case Page::Channels: { RoomView rows[RoomCapacity]; const auto count = _backend->rrcRooms(rows,RoomCapacity); _rrc.rows(rows,count); break; }
    case Page::People: { PersonView rows[PeopleCapacity]; const auto count = _backend->rrcPeople(_rrc.navigation.conversation().room,rows,PeopleCapacity); _rrc.rows(rows,count); break; }
    case Page::Directory: { DirectoryView rows[5]; const auto count = _backend->rrcDirectory(rows,5,_rrc.navigation.offset()); _rrc.rows(rows,count,false,true); break; }
    default: break;
    }
}
void MessagesScreen::prepareRrcForm() {
    if (!_rrc.form() || _rrcFormView == _rrc.navigation.revision()) return;
    _rrcInput.clearSensitive(); _rrcInput.setMaxLength(_rrc.formLimit());
    _rrcInput.setActive(true); _rrcFormView = _rrc.navigation.revision(); _rememberKey = false;
    _rrcInput.setSubmitCallback([this](const std::string& value) {
        if (_backend && _rrc.submit(value.data(),value.size(),_backend->rrcStatus(),_rememberKey)) {
            _rrcInput.clearSensitive(); _rrcFormView = 0;
        }
    });
}
void MessagesScreen::renderRrc(M5Canvas& canvas, int y) {
    if (!_backend) return;
    const auto status = _backend->rrcStatus();
    canvas.setTextColor(Theme::TEXT_SECONDARY);
    if (_deleteNotice || _rrc.navigation.page()!=handheld::ui::RrcNavigation::Page::Channels) {
        ScrollList::renderRow(canvas,_deleteNotice ? _deleteNotice : _rrc.heading(),0,y,Theme::CONTENT_W,false);
        y += Theme::LIST_ROW_H;
    }
    if (_rrc.form()) {
        prepareRrcForm();
        if (_rrc.navigation.page() == handheld::ui::RrcNavigation::Page::RoomKey) {
            canvas.drawString(_rememberKey ? "Remember: yes (Ctrl+R)" : "Remember: no (Ctrl+R)",4,y);
            y += Theme::CHAR_H + 2;
        }
        _rrcInput.render(canvas,0,y,Theme::CONTENT_W);
        return;
    }
    if (!_rrc.ready()) { canvas.drawString("Loading...",4,y); return; }
    const size_t visible = std::max(1,(Theme::CONTENT_Y + Theme::CONTENT_H-y)/Theme::LIST_ROW_H);
    const auto selected = _rrc.navigation.selected();
    const auto first = selected < _rrc.count() && selected >= visible ? selected-visible+1 : 0;
    for (size_t n = first; n < _rrc.count() && n < first+visible; ++n) {
        char text[112]; _rrc.label(n,status,text,sizeof text);
        ScrollList::renderRow(canvas,text,0,y,Theme::CONTENT_W,n==selected);
        y += Theme::LIST_ROW_H;
        if (n == 0 && _rrc.navigation.page() == handheld::ui::RrcNavigation::Page::Channels)
            canvas.drawFastHLine(2,y-1,Theme::CONTENT_W-4,Theme::DIVIDER);
    }
}
bool MessagesScreen::handleRrcKey(const KeyEvent& event) {
    if (!_backend) return true;
    if (_rrc.form()) {
        prepareRrcForm();
        if (event.escape && !event.repeat) { _rrcInput.clearSensitive(); _rrc.back(); return true; }
        if (event.ctrl && (event.character=='r' || event.character=='R')) { if (!event.repeat) _rememberKey=!_rememberKey; return true; }
        return _rrcInput.handleKey(event);
    }
    if (event.escape || event.backspace) { if (!event.repeat) _rrc.back(); return true; }
    auto selected = _rrc.navigation.selected();
    if (event.navUp()) { _rrc.navigation.select(selected >= _rrc.count() ? _rrc.count()-1 : selected ? selected-1 : 0); return true; }
    if (event.navDown()) { _rrc.navigation.select(selected >= _rrc.count() ? 0 : std::min(selected+1,_rrc.count()-1)); return true; }
    if (event.enter) { if (!event.repeat) _rrc.activate(selected,_backend->rrcStatus()); return true; }
    if (event.character=='r' || event.character=='R') { _rrc.refresh(); return true; }
    return false;
}

void MessagesScreen::onEnter() {
    _showingContext = false; _visible = true; _pageFocus = -1;
    _rrc.visible(true);
    _rrc.notice = [this](const char* text) { _deleteNotice = text; _deleteNoticeSince = millis(); };
    _rrc.send = [this](const handheld::rrc::Command& value, const uint8_t* body, size_t length) {
        if (!_backend) return handheld::rrc::Code::Offline;
        auto command = value;
        command.revision = handheld::ui::nextCardRrcCommand();
        if (!command.revision) return handheld::rrc::Code::Full;
        return _backend->rrcCommand(command,body,length);
    };
    // Cardputer identity switches commit through an orderly restart.
    if (_rrc.navigation.direct()) _conversations.resume(1);
}
void MessagesScreen::onExit() {
    _visible = false; _conversations.close(); _rrc.visible(false);
    // Secret forms never survive leaving the browser.
    _rrcInput.clearSensitive(); _rrcFormView = 0;
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
    const auto rrcRevision = _rrc.revision();
    if (allowAdmission) pollRrc();
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
        pageFocus != _pageFocus || rrcRevision != _rrc.revision();
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
    handheld::canvas::drawPageNavigation(canvas, y, _pageFocus, [this](int action) { return pageEnabled(action); });
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
    if (!_rrc.navigation.direct()) {
        canvas.drawString((std::string("Direct  |  [") + _rrc.navigation.secondLabel() + "]   Tab").c_str(),8,y+2);
        canvas.drawFastHLine(0,y+headerH,Theme::CONTENT_W,Theme::DIVIDER);
        renderRrc(canvas,y+headerH+2); return;
    }
    bool unavailable = false;
    for (size_t i = 0; i < _conversations.count(); ++i)
        unavailable |= bool(_conversations.row(i)->flags & handheld::storage::ConversationView::Unavailable);
    const char* notice = _deleteNotice ? _deleteNotice : !_conversations.freshnessAvailable() ? "Refresh needed (R)" :
        unavailable || _conversations.state() == Conversations::State::Retrying ? "Read failed; R to retry" :
        _conversations.statusRefreshDelayed() ? "Status refresh delayed (R)" :
        nullptr;
    const auto heading = notice ? std::string(notice) : std::string("[Direct]  |  ") + _rrc.navigation.secondLabel() + "   Tab";
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
    if (event.tab && !event.repeat && !_rrc.form() && !_showingContext) { switchFamily(); return true; }
    if (!_rrc.navigation.direct()) return handleRrcKey(event);
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
