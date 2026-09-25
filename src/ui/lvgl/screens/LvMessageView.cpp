#include "LvMessageView.h"
#include "reticulum/MessageStatusDetail.h"
#include "Theme.h"
#include "LvTheme.h"
#include "LvTabBar.h"
#include "util/PerfTrace.h"
#include <Arduino.h>
#include <time.h>
#include <cmath>
#include "fonts/fonts.h"

namespace {

constexpr int kHeaderH = 36;
constexpr int kInputH = 31;
constexpr int kComposerButtonW = 44;
constexpr int kBubbleMaxW = Theme::CONTENT_W * 3 / 4;
#if !HAS_TOUCH
constexpr const char* kComposerPlaceholder = "Message... or Enter to read full";
#else
constexpr const char* kComposerPlaceholder = "Message...";
#endif

bool isPendingStatus(LXMFStatus status) {
    return status == LXMFStatus::QUEUED || status == LXMFStatus::SENDING;
}

uint32_t bubbleBorderColor(LXMFStatus status) {
    if (status == LXMFStatus::FAILED) return Theme::ERROR_CLR;
    if (status == LXMFStatus::UNCONFIRMED) return Theme::WARNING_CLR;
    if (isPendingStatus(status)) return Theme::WARNING_CLR;
    if (status == LXMFStatus::DELIVERED) return Theme::PRIMARY_MUTED;
    return Theme::BORDER;
}

bool formatClock(double ts, char* out, size_t outLen) {
    if (!out || outLen == 0 || ts <= 1700000000) return false;
    time_t t = (time_t)ts;
    struct tm converted;
    struct tm* tm = localtime_r(&t, &converted);
    if (!tm) return false;
    snprintf(out, outLen, "%02d:%02d", tm->tm_hour, tm->tm_min);
    return true;
}

int textWidthForBubble(const char* content, size_t length) {
    size_t longest = 0;
    size_t current = 0;
    for (size_t i = 0; i < length; ++i) {
        const char ch = content[i];
        if (ch == '\n' || ch == '\r') {
            if (current > longest) longest = current;
            current = 0;
        } else {
            current++;
        }
    }
    if (current > longest) longest = current;
    if (length > 34 || longest > 28) return kBubbleMaxW - 18;
    int width = (int)longest * 7 + 12;
    if (width < 54) width = 54;
    int maxW = kBubbleMaxW - 18;
    if (width > maxW) width = maxW;
    return width;
}

void makeTransparent(lv_obj_t* obj) {
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

}  // namespace

void LvMessageView::updateHeader() {
    if (!_lblHeader) return;
    if (_lblHeaderMeta && strcmp(lv_label_get_text(_lblHeaderMeta), _peerHex.c_str())) {
        lv_label_set_text(_lblHeaderMeta, _peerHex.c_str());
    }
    const auto appliedNodes = _am ? _am->revision() : (_service ? _service->status().nodeRevision : 0);
    if (_service && (_nameNodeRevision != appliedNodes ||
        _nameIdentity != _service->status().generation)) {
        _nameNodeRevision = appliedNodes;
        _nameIdentity = _service->status().generation; _nameResolved = false;
    }
    const auto liveName = _am ? _am->lookupName(_peerHex) : std::string();
    if (!liveName.empty()) {
        if (!_nameResolved) lv_label_set_text(_lblHeader, liveName.c_str());
        _nameResolved = true; return;
    }
    if (_nameResolved) return;
    const auto fallback = _peerHex.substr(0, 12);
    if (strcmp(lv_label_get_text(_lblHeader), fallback.c_str())) lv_label_set_text(_lblHeader, fallback.c_str());
    if (!_service || !_entered || _nameInFlight || !windowMatches() || !_service->available()) return;
    std::array<uint8_t, 16> peer;
    memcpy(peer.data(), _service->historyWindow().peer(), peer.size());
    const auto identity = _nameIdentity, nodes = _nameNodeRevision;
    const auto backendNodes = _service->status().nodeRevision;
    _nameInFlight = true;
    const auto id = _service->requestPeerName(_peerHex,
        [this, peer, identity, nodes, backendNodes](const handheld::Result& result, const char* name) {
            _nameInFlight = false;
            if (!_entered || !_screen || !_lblHeader || !_service || !windowMatches() ||
                _service->status().generation != identity || _service->status().nodeRevision != backendNodes ||
                (_am ? _am->revision() : _service->status().nodeRevision) != nodes ||
                memcmp(_service->historyWindow().peer(), peer.data(), peer.size())) return;
            _nameResolved = true; // Failure keeps the hash; explicit Refresh retries.
            if (result.outcome == handheld::Outcome::Ok && result.length) lv_label_set_text(_lblHeader, name);
        });
    if (!id) { _nameInFlight = false; _nameResolved = true; }
}

void LvMessageView::markVisibleConversationRead() {
    if (!_markReadPending || _readInFlight || !_service || !_entered || !_screen) return;
    const auto now = uint32_t(millis());
    if (_readRetry && uint32_t(now - _readRetryStarted) < 1000) return;
    if (!_service->available()) return;
    std::array<uint8_t, 16> peer;
    memcpy(peer.data(), _service->historyWindow().peer(), peer.size());
    const auto identity = _service->status().generation;
    _markReadPending = false; _readInFlight = true; _readRetry = false;
    const auto id = _service->action(handheld::Operation::MarkRead, _peerHex, "", 0,
        [this, peer, identity](const handheld::Result& result) {
            // ServiceClient owns this exact request until its terminal result.
            // Hidden/other-peer completions retire it without touching widgets
            // or clearing a newer conversation's pending read intent.
            _readInFlight = false; _readRequest = 0;
            const bool sameView = _entered && _service && windowMatches() &&
                _service->status().generation == identity &&
                !memcmp(_service->historyWindow().peer(), peer.data(), peer.size());
            if (sameView && result.outcome != handheld::Outcome::Ok &&
                result.outcome != handheld::Outcome::Cancelled &&
                result.outcome != handheld::Outcome::Stale &&
                result.outcome != handheld::Outcome::Invalid) {
                _markReadPending = true; _readRetry = true;
                _readRetryStarted = uint32_t(millis());
            }
        });
    if (!id) {
        _readInFlight = false; _markReadPending = true; _readRetry = true;
        _readRetryStarted = now;
    } else if (_readInFlight) _readRequest = id;
}

void LvMessageView::updateComposerState() {
    if (!_btnSend) return;
    bool hasText = !_inputText.empty();
    lv_obj_set_style_border_color(_btnSend, lv_color_hex(hasText ? Theme::PRIMARY : Theme::BORDER), 0);
    lv_obj_set_style_bg_color(_btnSend, lv_color_hex(hasText ? Theme::PRIMARY_SUBTLE : Theme::BG_ELEVATED), 0);
    if (_textarea) {
        lv_obj_set_style_border_color(_textarea, lv_color_hex(hasText ? Theme::PRIMARY_MUTED : Theme::BORDER), 0);
    }
    refreshComposerPlaceholder();
}

void LvMessageView::composerEdited() {
    if (_nextDraftRevision != UINT64_MAX) ++_nextDraftRevision;
    _draftRevision = _nextDraftRevision;
    if (_peerHex == _retainedDraftPeer && _service &&
        _retainedDraftIdentity == _service->status().generation) {
        _retainedDraft = _inputText;
        _retainedDraftRevision = _draftRevision;
    }
}

void LvMessageView::refreshComposerPlaceholder() {
    if (!_textarea) return;
    bool focused = lv_obj_has_state(_textarea, LV_STATE_FOCUSED);
    lv_textarea_set_placeholder_text(_textarea,
        (_inputText.empty() && !focused) ? kComposerPlaceholder : "");
    updateComposerText();
}

void LvMessageView::updateComposerText() {
    if (!_textarea) return;

    bool focused = lv_obj_has_state(_textarea, LV_STATE_FOCUSED);
    bool showCaret = focused || !_inputText.empty();
    if (!showCaret) {
        lv_textarea_set_text(_textarea, "");
        return;
    }

    std::string display = _inputText;
    display += "_";
    lv_textarea_set_text(_textarea, display.c_str());
    lv_textarea_set_cursor_pos(_textarea, LV_TEXTAREA_CURSOR_LAST);
}

void LvMessageView::createUI(lv_obj_t* parent) {
    _screen = parent;
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(parent, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_pad_all(parent, 0, 0);

    // Use flex column layout: header, messages (grows), input
    lv_obj_set_layout(parent, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 0, 0);

    const lv_font_t* font = &lv_font_rsdeck_12;

    // Header bar (top)
    _header = lv_obj_create(parent);
    lv_obj_set_size(_header, lv_pct(100), kHeaderH);
    lv_obj_set_style_bg_color(_header, lv_color_hex(Theme::BG_ELEVATED), 0);
    lv_obj_set_style_bg_opa(_header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(_header, lv_color_hex(Theme::BORDER), 0);
    lv_obj_set_style_border_width(_header, 1, 0);
    lv_obj_set_style_border_side(_header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_all(_header, 0, 0);
    lv_obj_set_style_radius(_header, 0, 0);
    lv_obj_clear_flag(_header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* backLbl = lv_label_create(_header);
    lv_obj_set_style_text_font(backLbl, &lv_font_rsdeck_14, 0);
    lv_obj_set_style_text_color(backLbl, lv_color_hex(Theme::PRIMARY), 0);
    lv_label_set_text(backLbl, "<");
    lv_obj_align(backLbl, LV_ALIGN_LEFT_MID, 6, 0);

    _lblHeader = lv_label_create(_header);
    lv_obj_set_style_text_font(_lblHeader, &lv_font_rsdeck_14, 0);
    lv_obj_set_style_text_color(_lblHeader, lv_color_hex(Theme::ACCENT), 0);
    lv_label_set_long_mode(_lblHeader, LV_LABEL_LONG_DOT);
    lv_obj_set_size(_lblHeader, Theme::CONTENT_W - 30, lv_font_rsdeck_14.line_height);
    lv_obj_set_pos(_lblHeader, 22, 2);

    _lblHeaderMeta = lv_label_create(_header);
    lv_obj_set_style_text_font(_lblHeaderMeta, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(_lblHeaderMeta, lv_color_hex(Theme::TEXT_SECONDARY), 0);
    lv_label_set_long_mode(_lblHeaderMeta, LV_LABEL_LONG_DOT);
    lv_obj_set_size(_lblHeaderMeta, Theme::CONTENT_W - 30, lv_font_rsdeck_10.line_height);
    lv_obj_set_pos(_lblHeaderMeta, 22, 21);

    // Make header tappable for back navigation
    lv_obj_add_flag(_header, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_header, [](lv_event_t* e) {
        auto* self = (LvMessageView*)lv_event_get_user_data(e);
        self->goBack();
    }, LV_EVENT_CLICKED, this);

    // Message scroll area (middle, grows to fill)
    _msgScroll = lv_obj_create(parent);
    lv_obj_clear_flag(_msgScroll, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_width(_msgScroll, lv_pct(100));
    lv_obj_set_flex_grow(_msgScroll, 1);
    lv_obj_set_style_bg_color(_msgScroll, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_bg_opa(_msgScroll, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_msgScroll, 0, 0);
    lv_obj_set_style_pad_all(_msgScroll, 6, 0);
    lv_obj_set_style_pad_row(_msgScroll, 7, 0);
    lv_obj_set_style_radius(_msgScroll, 0, 0);
    lv_obj_set_layout(_msgScroll, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(_msgScroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_style(_msgScroll, LvTheme::styleScrollbar(), LV_PART_SCROLLBAR);

    lv_obj_add_event_cb(_msgScroll, [](lv_event_t* e) {
        auto* self = static_cast<LvMessageView*>(lv_event_get_user_data(e));
        const auto code = lv_event_get_code(e);
        if (code == LV_EVENT_GESTURE && self->boundWindow() &&
            lv_obj_get_scroll_y(self->_msgScroll) <= 0 && lv_obj_get_scroll_bottom(self->_msgScroll) <= 1) {
            // A short final page has no scroll overflow, so LVGL reports a
            // gesture instead. It must still reach the adjacent history page.
            auto* input = lv_indev_get_act();
            if (input && lv_indev_get_type(input) == LV_INDEV_TYPE_POINTER) {
                const auto direction = lv_indev_get_gesture_dir(input);
                if (direction == LV_DIR_BOTTOM) self->historyAction(0);
                else if (direction == LV_DIR_TOP) self->historyAction(1);
            }
        } else if (code == LV_EVENT_SCROLL_BEGIN) {
            auto* input = lv_indev_get_act();
            // Elastic rebound emits nested animation begin/end events. Only
            // the input-owned scroll starts a gesture; keep its original peak.
            if (!self->_touchScrolling && !self->_binding && !lv_event_get_param(e) &&
                input && lv_indev_get_type(input) == LV_INDEV_TYPE_POINTER &&
                lv_indev_get_scroll_obj(input) == self->_msgScroll) {
                self->_touchScrolling = true;
                self->_touchScrollStart = lv_obj_get_scroll_y(self->_msgScroll);
                self->_touchScrollPeak = 0;
            }
        } else if (code == LV_EVENT_SCROLL) {
            if (self->_touchScrolling) {
                const auto delta = lv_obj_get_scroll_y(self->_msgScroll) - self->_touchScrollStart;
                // Keep the gesture direction through LVGL's elastic rebound.
                if (std::abs(delta) > std::abs(self->_touchScrollPeak)) self->_touchScrollPeak = delta;
            }
            self->saveScroll(self->_touchScrolling);
        } else if (code == LV_EVENT_SCROLL_END && self->_touchScrolling && lv_event_get_param(e)) {
            self->_touchScrolling = false;
            if (!self->boundWindow()) return;
            self->saveScroll(true);
            if (self->_touchScrollPeak < -8 && lv_obj_get_scroll_y(self->_msgScroll) <= 0)
                self->historyAction(0);
            else if (self->_touchScrollPeak > 8 && lv_obj_get_scroll_bottom(self->_msgScroll) <= 1)
                self->historyAction(1);
        }
    }, LV_EVENT_ALL, this);

    // A notice only occupies space when new arrivals or a failed read need an
    // action. Ordinary history navigation follows touch/wheel scrolling.
    _historyNotice = lv_btn_create(parent);
    lv_obj_set_size(_historyNotice, lv_pct(100), 28);
    lv_obj_add_style(_historyNotice, LvTheme::styleBtn(), 0);
    lv_obj_set_style_pad_all(_historyNotice, 0, 0);
    lv_obj_add_flag(_historyNotice, LV_OBJ_FLAG_HIDDEN);
    _historyNoticeLabel = lv_label_create(_historyNotice);
    lv_obj_set_style_text_font(_historyNoticeLabel, &lv_font_rsdeck_12, 0);
    lv_obj_center(_historyNoticeLabel);
    lv_obj_add_event_cb(_historyNotice, [](lv_event_t* e) {
        auto* self = static_cast<LvMessageView*>(lv_event_get_user_data(e));
        self->historyAction(self->_noticeAction);
    }, LV_EVENT_CLICKED, this);

    // Input row (bottom, just above tab bar)
    _inputRow = lv_obj_create(parent);
    lv_obj_set_size(_inputRow, lv_pct(100), kInputH);
    lv_obj_set_style_bg_color(_inputRow, lv_color_hex(Theme::BG_ELEVATED), 0);
    lv_obj_set_style_bg_opa(_inputRow, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(_inputRow, lv_color_hex(Theme::BORDER), 0);
    lv_obj_set_style_border_width(_inputRow, 1, 0);
    lv_obj_set_style_border_side(_inputRow, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_pad_all(_inputRow, 3, 0);
    lv_obj_set_style_radius(_inputRow, 0, 0);
    lv_obj_clear_flag(_inputRow, LV_OBJ_FLAG_SCROLLABLE);

    _textarea = lv_textarea_create(_inputRow);
    lv_obj_set_size(_textarea, Theme::CONTENT_W - kComposerButtonW - 12, 23);
    lv_obj_align(_textarea, LV_ALIGN_LEFT_MID, 0, 0);
    lv_textarea_set_one_line(_textarea, true);
    lv_textarea_set_max_length(_textarea, MAX_COMPOSER_CHARS + 1);
    lv_textarea_set_placeholder_text(_textarea, kComposerPlaceholder);
    lv_obj_add_style(_textarea, LvTheme::styleTextarea(), 0);
    lv_obj_add_style(_textarea, LvTheme::styleTextareaFocused(), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(_textarea, 1, 0);
    lv_obj_set_style_text_font(_textarea, font, 0);
    lv_obj_set_style_pad_all(_textarea, 2, 0);
    lv_obj_add_event_cb(_textarea, [](lv_event_t* e) {
        auto* self = (LvMessageView*)lv_event_get_user_data(e);
        lv_event_code_t code = lv_event_get_code(e);
        if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED || code == LV_EVENT_PRESSED) {
            self->refreshComposerPlaceholder();
        } else if (code == LV_EVENT_DEFOCUSED) {
            self->refreshComposerPlaceholder();
        }
    }, LV_EVENT_ALL, this);

    _btnSend = lv_btn_create(_inputRow);
    lv_obj_set_size(_btnSend, kComposerButtonW, 23);
    lv_obj_align(_btnSend, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_style(_btnSend, LvTheme::styleBtn(), 0);
    lv_obj_set_style_pad_all(_btnSend, 0, 0);
    lv_obj_t* sendLbl = lv_label_create(_btnSend);
    lv_obj_set_style_text_font(sendLbl, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(sendLbl, lv_color_hex(Theme::PRIMARY), 0);
    lv_label_set_text(sendLbl, "SEND");
    lv_obj_center(sendLbl);
    lv_obj_add_event_cb(_btnSend, [](lv_event_t* e) {
        auto* self = (LvMessageView*)lv_event_get_user_data(e);
        if (self->_suppressNextSendClick) {
            self->_suppressNextSendClick = false;
            return;
        }
        self->sendCurrentMessage(false);
    }, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(_btnSend, [](lv_event_t* e) {
        auto* self = (LvMessageView*)lv_event_get_user_data(e);
        self->_suppressNextSendClick = true;
        self->showSendModeMenu();
    }, LV_EVENT_LONG_PRESSED, this);

    updateHeader();
    updateComposerState();
}

void LvMessageView::setPeerHex(const std::string& hex) {
    if (_peerHex == hex) return;
    clearMessages();
    if (_service) _service->historyWindow().acknowledgePublication(_service->historyWindow().revision());
    _peerHex = hex;
    _nameResolved = false;
    _scrollToEnd = true;
    if (_entered && _service) {
        if (_retainedDraftPeer == _peerHex && _retainedDraftIdentity == _service->status().generation) {
            _inputText = _retainedDraft; _draftRevision = _retainedDraftRevision;
        } else { _inputText.clear(); composerEdited(); }
        updateComposerText(); updateComposerState();
        _service->watchHistory(_peerHex);
        _markReadPending = true;
        _readThrough = 0; _readRetry = false;
        refreshUI();
    }
}

bool LvMessageView::windowMatches() const {
    if (!_service || _peerHex.size() != 32) return false;
    const auto& window = _service->historyWindow();
    if (window.identityGeneration() != _service->status().generation) return false;
    static constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 16; ++i) {
        const auto byte = window.peer()[i];
        const auto lower = [](char value) { return value >= 'A' && value <= 'F' ? char(value + 'a' - 'A') : value; };
        if (lower(_peerHex[i * 2]) != hex[byte >> 4] || lower(_peerHex[i * 2 + 1]) != hex[byte & 15]) return false;
    }
    return true;
}

bool LvMessageView::boundWindow() const {
    if (!_entered || !windowMatches()) return false;
    const auto& window = _service->historyWindow();
    return window.visible() && window.statusReady() && _lastHistoryRevision == window.revision() &&
        _boundMode == window.mode() && _boundIdentity == window.identityGeneration();
}

void LvMessageView::clearMessages() {
    // Deleting the labels synchronously ends every static-text pointer lease.
    _binding = true;
    if (_msgScroll) lv_obj_clean(_msgScroll);
    _statusLabels.fill(nullptr); _textLabels.fill(nullptr);
    _bubbleBoxes.fill(nullptr); _readButtons.fill(nullptr);
    _emptyLabel = nullptr; _touchScrolling = false;
    _rowCount = 0; _lastHistoryRevision = 0; _lastStatusRevision = 0;
    _boundMode = HistoryWindow::Mode::Closed; _boundIdentity = 0;
    _binding = false;
}

void LvMessageView::saveScroll(bool userChange) {
    if (_binding || !_msgScroll || !boundWindow()) return;
    const auto y = lv_obj_get_scroll_y(_msgScroll);
    _service->historyWindow().setScrollOffset(y > 0 ? uint32_t(y) : 0);
    _atBottom = lv_obj_get_scroll_bottom(_msgScroll) <= 1;
    if (userChange) _service->historyWindow().setViewportAtNewest(_atBottom);
}

void LvMessageView::historyAction(unsigned action) {
    if (!_service || !windowMatches()) return;
    auto& window = _service->historyWindow();
    saveScroll();
    const bool full = window.mode() == HistoryWindow::Mode::Full;
    if (action < 2 && !boundWindow()) return;
    bool changed = false;
    if (action == 0) { changed = window.older(); if (changed) _scrollToEnd = !full; }
    else if (action == 1) { changed = window.newer(); if (changed) _scrollToEnd = false; }
    else if (action == 2) {
        changed = full ? window.backToChat() : window.newest();
        if (changed) _scrollToEnd = !full;
    } else { window.refresh(); _nameResolved = false; changed = true; }
    if (changed) refreshUI();
}

void LvMessageView::readFull(size_t index) {
    // Old widgets may still be visible while a replacement's status is pending.
    // Their indices cannot select rows from that unbound publication.
    if (!boundWindow()) return;
    auto& window = _service->historyWindow();
    const auto* row = window.span(index);
    if (!row || !row->more() || window.mode() != HistoryWindow::Mode::Chat) return;
    saveScroll(); window.focusSpan(index);
    if (window.openFull(index)) { _scrollToEnd = false; refreshUI(); }
}

void LvMessageView::goBack() {
    if (_service && windowMatches() && _service->historyWindow().mode() == HistoryWindow::Mode::Full) {
        historyAction(2); return;
    }
    if (_onBack) _onBack();
}

void LvMessageView::scrollHistory(int pixels) {
    if (!_msgScroll) return;
    saveScroll(true);
    if (boundWindow() && ((pixels < 0 && lv_obj_get_scroll_y(_msgScroll) <= 0) ||
                         (pixels > 0 && lv_obj_get_scroll_bottom(_msgScroll) <= 1))) {
        historyAction(pixels < 0 ? 0 : 1);
    } else {
        lv_obj_scroll_to_y(_msgScroll, lv_obj_get_scroll_y(_msgScroll) + pixels, LV_ANIM_OFF);
        saveScroll(true);
    }
}

bool LvMessageView::hasReadFocus() const {
    return boundWindow() && _service->historyWindow().mode() == HistoryWindow::Mode::Chat &&
        _service->historyWindow().focusedSpan() < _rowCount &&
        _readButtons[_service->historyWindow().focusedSpan()];
}

void LvMessageView::focusNextRead(int direction) {
    if (!boundWindow() || _service->historyWindow().mode() != HistoryWindow::Mode::Chat) return;
    auto& window = _service->historyWindow();
    const auto first = window.focusedSpan();
    size_t next = first;
    for (size_t tries = 0; tries <= HistoryWindow::VisibleSpans; ++tries) {
        if (direction < 0) next = next == 0 ? HistoryWindow::VisibleSpans : next - 1;
        else next = next == HistoryWindow::VisibleSpans ? 0 : next + 1;
        if (next == HistoryWindow::VisibleSpans || (next < _rowCount && _readButtons[next])) break;
    }
    window.focusSpan(next); updateHistoryFocus();
    if (next < _rowCount && _readButtons[next]) lv_obj_scroll_to_view(_readButtons[next], LV_ANIM_OFF);
}

void LvMessageView::updateHistoryFocus() {
    size_t focus = _service ? _service->historyWindow().focusedSpan() : HistoryWindow::VisibleSpans;
    if (boundWindow() && _service->historyWindow().mode() == HistoryWindow::Mode::Chat &&
        focus < HistoryWindow::VisibleSpans && !_readButtons[focus]) {
        _service->historyWindow().focusSpan(HistoryWindow::VisibleSpans);
        focus = HistoryWindow::VisibleSpans;
    }
    for (size_t i = 0; i < _rowCount; ++i) if (_readButtons[i]) {
        lv_obj_set_style_border_color(_readButtons[i], lv_color_hex(i == focus ? Theme::ACCENT : Theme::BORDER), 0);
        lv_obj_set_style_bg_color(_readButtons[i], lv_color_hex(i == focus ? Theme::PRIMARY_SUBTLE : Theme::BG_ELEVATED), 0);
    }
}

void LvMessageView::updateHistoryControls() {
    if (!_historyNotice || !_service) return;
    auto& window = _service->historyWindow();
    const bool matches = windowMatches();
    const bool ready = matches && window.visible() && window.statusReady();
    const char* notice = nullptr;
    _noticeAction = 3;
    if (matches && window.state() != HistoryWindow::State::Exhausted) {
        if (window.error() == HistoryWindow::Error::Busy) notice = "Storage busy; retry";
        else if (window.error() != HistoryWindow::Error::None) notice = "Read failed; retry";
        else if (!window.freshnessAvailable()) notice = "Updates unavailable; refresh";
        else if (ready && window.statusRefreshDelayed()) notice = "Status refresh delayed; retry";
        else if (ready && window.newBelow() && window.mode() == HistoryWindow::Mode::Chat) {
            notice = "New messages"; _noticeAction = 2;
        }
    }
    if (notice) {
        if (strcmp(lv_label_get_text(_historyNoticeLabel), notice)) lv_label_set_text(_historyNoticeLabel, notice);
        lv_obj_clear_flag(_historyNotice, LV_OBJ_FLAG_HIDDEN);
    } else lv_obj_add_flag(_historyNotice, LV_OBJ_FLAG_HIDDEN);

    if (!_rowCount) {
        if (!_emptyLabel) {
            _emptyLabel = lv_label_create(_msgScroll);
            lv_obj_add_flag(_emptyLabel, LV_OBJ_FLAG_FLOATING);
            lv_obj_set_style_text_font(_emptyLabel, &lv_font_rsdeck_12, 0);
            lv_obj_set_style_text_color(_emptyLabel, lv_color_hex(Theme::TEXT_MUTED), 0);
            lv_obj_set_style_text_align(_emptyLabel, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_center(_emptyLabel);
        }
        const char* text = matches && window.state() == HistoryWindow::State::Exhausted ? "Reopen conversation" :
            matches && window.error() != HistoryWindow::Error::None ? "Messages unavailable" :
            !ready || window.loading() ? "Loading messages..." : "No messages yet";
        if (strcmp(lv_label_get_text(_emptyLabel), text)) lv_label_set_text(_emptyLabel, text);
    }
}

void LvMessageView::destroyUI() {
    hideSendModeMenu();
    clearMessages();
    if (_service) _service->historyWindow().acknowledgePublication(_service->historyWindow().revision());
    _header = _lblHeader = _lblHeaderMeta = _msgScroll = nullptr;
    _historyNotice = _historyNoticeLabel = _emptyLabel = nullptr;
    _inputRow = _textarea = _btnSend = nullptr;
    for (size_t i = 0; i < 3; ++i) { _sendRows[i] = nullptr; _sendLabels[i] = nullptr; }
    LvScreen::destroyUI();
}

void LvMessageView::onEnter() {
    _entered = true;
    _nameResolved = false;
    _markReadPending = _service != nullptr;
    _readThrough = 0; _readRetry = false;
    if (_service) _service->watchHistory(_peerHex);
    if (_retainedDraftPeer == _peerHex && _service &&
        _retainedDraftIdentity == _service->status().generation) {
        _inputText = _retainedDraft; _draftRevision = _retainedDraftRevision;
    } else { _inputText.clear(); composerEdited(); }
    _scrollToEnd = true; _atBottom = true;
    hideSendModeMenu(); updateComposerText();
    updateHeader(); updateComposerState();
    clearMessages(); refreshUI();
}

void LvMessageView::onExit() {
    saveScroll(); _entered = false;
    _nameResolved = false;
    clearMessages();
    if (_service) {
        _service->closeHistory();
        _service->historyWindow().acknowledgePublication(_service->historyWindow().revision());
    }
    _markReadPending = false;
    hideSendModeMenu(); _inputText.clear();
}

void LvMessageView::refreshUI() {
    if (!_screen || !_entered || !_service) return;
    auto& window = _service->historyWindow();
    const bool matches = windowMatches();
    if (!matches || !window.visible()) {
        clearMessages();
        // This publication was abandoned by a peer/identity/mode change. No
        // label remains; release its bank even if its old status query is held.
        window.acknowledgePublication(window.revision());
    } else if (_lastHistoryRevision &&
        (_boundMode != window.mode() || _boundIdentity != window.identityGeneration())) clearMessages();
    if (matches && window.visible() && window.statusReady()) {
        if (_lastHistoryRevision != window.revision()) rebuildMessages();
        else if (_lastStatusRevision != window.statusRevision()) {
            _binding = true;
            for (size_t i = 0; i < _rowCount; ++i) updateMessageStatus(i, *window.span(i));
            _lastStatusRevision = window.statusRevision();
            lv_obj_update_layout(_msgScroll);
            lv_obj_scroll_to_y(_msgScroll, window.scrollOffset(), LV_ANIM_OFF);
            _binding = false;
        }
    }
    updateHeader(); updateHistoryControls(); updateHistoryFocus();
    if (matches && window.visible() && window.statusReady() && window.mode() == HistoryWindow::Mode::Chat) {
        // New incoming tuples can request one further write while the current
        // one is held. Status changes and stale unread snapshots cannot.
        for (size_t i = 0; i < window.spanCount(); ++i) {
            const auto& row = *window.span(i);
            if (row.incoming() && !row.unavailable() && row.counter > _readThrough) {
                _readThrough = row.counter;
                if (!(row.flags & Span::Read)) _markReadPending = true;
            }
        }
        if (window.total()) markVisibleConversationRead();
        else _markReadPending = false;
    }
}

void LvMessageView::appendMessage(size_t index, const Span& span, const char* text) {
    if (!_msgScroll || index >= HistoryWindow::VisibleSpans) return;
    const auto status = static_cast<LXMFStatus>(span.status);

    const lv_font_t* font = &lv_font_rsdeck_12;
    int textW = textWidthForBubble(text, span.textLength);
    // Leave a gap between the clock and the longest storage-status caption.
    if (!span.incoming() && textW < 152) textW = 152;
    if (_service && _service->historyWindow().mode() == HistoryWindow::Mode::Full) textW = Theme::CONTENT_W - 40;
    int boxW = textW + 16;

    lv_obj_set_layout(_msgScroll, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(_msgScroll, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* bubble = lv_obj_create(_msgScroll);
    lv_obj_set_width(bubble, Theme::CONTENT_W - 12);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    makeTransparent(bubble);

    lv_obj_t* box = lv_obj_create(bubble);
    lv_obj_set_width(box, boxW);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_layout(box, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left(box, 7, 0);
    lv_obj_set_style_pad_right(box, 7, 0);
    lv_obj_set_style_pad_top(box, 5, 0);
    lv_obj_set_style_pad_bottom(box, 5, 0);
    lv_obj_set_style_pad_row(box, 3, 0);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(bubbleBorderColor(status)), 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    if (span.incoming()) {
        lv_obj_set_style_bg_color(box, lv_color_hex(Theme::MSG_IN_BG), 0);
        lv_obj_align(box, LV_ALIGN_TOP_LEFT, 0, 0);
    } else {
        lv_obj_set_style_bg_color(box, lv_color_hex(Theme::MSG_OUT_BG), 0);
        lv_obj_align(box, LV_ALIGN_TOP_RIGHT, 0, 0);
    }
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);

    // Message text color - incoming is plain text, outgoing reflects delivery status
    uint32_t textColor = Theme::TEXT_PRIMARY; // incoming default
    if (!span.incoming()) {
        switch (status) {
            case LXMFStatus::QUEUED:
            case LXMFStatus::SENDING:
                textColor = Theme::TEXT_SECONDARY; break;
            case LXMFStatus::SENT:
            case LXMFStatus::DELIVERED:
                textColor = Theme::TEXT_PRIMARY; break;
            case LXMFStatus::FAILED:
                textColor = Theme::ERROR_CLR; break;
            default:
                textColor = Theme::TEXT_PRIMARY; break;
        }
    }
    lv_obj_t* lbl = lv_label_create(box);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(span.unavailable() ? Theme::TEXT_MUTED : textColor), 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, textW);
    lv_label_set_text_static(lbl, text);

    if (span.more() && _service && _service->historyWindow().mode() == HistoryWindow::Mode::Chat) {
        auto* button = _readButtons[index] = lv_btn_create(box);
        lv_obj_set_size(button, 78, 20); lv_obj_add_style(button, LvTheme::styleBtn(), 0);
        lv_obj_set_style_pad_all(button, 0, 0);
        lv_obj_set_user_data(button, (void*)(uintptr_t)index);
        lv_obj_add_event_cb(button, [](lv_event_t* e) {
            auto* self = static_cast<LvMessageView*>(lv_event_get_user_data(e));
            self->readFull(uintptr_t(lv_obj_get_user_data(lv_event_get_target(e))));
        }, LV_EVENT_CLICKED, this);
        auto* label = lv_label_create(button); lv_label_set_text(label, "Read full");
        lv_obj_set_style_text_font(label, &lv_font_rsdeck_10, 0); lv_obj_center(label);
    }

    char timeBuf[8] = {0};
    bool hasTime = formatClock(span.timestamp, timeBuf, sizeof(timeBuf));
    bool needsMeta = hasTime || (!span.incoming() && !span.unavailable());
    lv_obj_t* statusLbl = nullptr;
    if (needsMeta) {
        lv_obj_t* meta = lv_obj_create(box);
        lv_obj_set_size(meta, textW, lv_font_get_line_height(&lv_font_rsdeck_10));
        makeTransparent(meta);

        if (hasTime) {
            lv_obj_t* timeLbl = lv_label_create(meta);
            lv_obj_set_style_text_font(timeLbl, &lv_font_rsdeck_10, 0);
            lv_obj_set_style_text_color(timeLbl, lv_color_hex(Theme::TEXT_MUTED), 0);
            lv_label_set_text(timeLbl, timeBuf);
            lv_obj_align(timeLbl, LV_ALIGN_LEFT_MID, 0, 0);
        }

        if (!span.incoming()) {
            statusLbl = lv_label_create(meta);
            lv_obj_set_style_text_font(statusLbl, &lv_font_rsdeck_10, 0);
            applyStatusGlyph(statusLbl, span);
            lv_obj_align(statusLbl, LV_ALIGN_RIGHT_MID, 0, 0);
        }
    }

    _statusLabels[index] = statusLbl;
    _textLabels[index] = lbl;
    _bubbleBoxes[index] = box;
}

void LvMessageView::rebuildMessages() {
    if (!_msgScroll || !_service) return;
    auto& window = _service->historyWindow();
    if (!windowMatches() || !window.visible() || !window.statusReady()) return;
    const bool bottom = _scrollToEnd || (window.mode() == HistoryWindow::Mode::Chat &&
        window.followsNewest());
    const auto scroll = window.scrollOffset();
    clearMessages(); _binding = true;
    _rowCount = window.spanCount();
    for (size_t i = 0; i < _rowCount; ++i) appendMessage(i, *window.span(i), window.text(i));
    lv_obj_update_layout(_msgScroll);
    for (size_t i = 0; i < _rowCount; ++i)
        lv_obj_set_height(lv_obj_get_parent(_bubbleBoxes[i]), lv_obj_get_height(_bubbleBoxes[i]));
    lv_obj_update_layout(_msgScroll);
    lv_obj_scroll_to_y(_msgScroll, bottom ? LV_COORD_MAX : scroll, LV_ANIM_OFF);
    _lastHistoryRevision = window.revision(); _lastStatusRevision = window.statusRevision();
    _boundMode = window.mode(); _boundIdentity = window.identityGeneration();
    _binding = false; _scrollToEnd = false; saveScroll();
    // ALL old static labels were deleted before this bank can be reused.
    window.acknowledgePublication(_lastHistoryRevision);
}

void LvMessageView::updateMessageStatus(size_t index, const Span& span) {
    if (index >= _rowCount) return;
    lv_obj_t* statusLbl = _statusLabels[index];
    lv_obj_t* textLbl = _textLabels[index];
    lv_obj_t* bubbleBox = _bubbleBoxes[index];
    if (!statusLbl) return;  // Incoming message, no status label

    const auto status = static_cast<LXMFStatus>(span.status);
    const auto previousMetaHeight = lv_obj_get_height(lv_obj_get_parent(statusLbl));
    applyStatusGlyph(statusLbl, span);
    if (bubbleBox) {
        lv_obj_set_style_border_color(bubbleBox, lv_color_hex(bubbleBorderColor(status)), 0);
        // History rows pin their height after layout. A transient storage
        // caption can add or remove a line without rebuilding the history.
        if (lv_obj_get_style_height(lv_obj_get_parent(statusLbl), 0) != previousMetaHeight) {
            lv_obj_update_layout(bubbleBox);
            lv_obj_set_height(lv_obj_get_parent(bubbleBox), lv_obj_get_height(bubbleBox));
        }
    }

    // Update text color to match status
    if (textLbl) {
        uint32_t textColor = Theme::TEXT_PRIMARY;
        if (status == LXMFStatus::QUEUED || status == LXMFStatus::SENDING) {
            textColor = Theme::TEXT_SECONDARY;
        } else if (status == LXMFStatus::FAILED) {
            textColor = Theme::ERROR_CLR;
        }
        lv_obj_set_style_text_color(textLbl, lv_color_hex(textColor), 0);
    }
}

void LvMessageView::applyStatusGlyph(lv_obj_t* lbl, const Span& span) {
    if (!lbl) return;
    const char* glyph = messageStatusLabel(static_cast<LXMFStatus>(span.status));
    uint32_t color;
    switch (static_cast<LXMFStatus>(span.status)) {
        case LXMFStatus::DELIVERED:
            color = Theme::SUCCESS;
            break;
        case LXMFStatus::SENT:
            color = Theme::TEXT_MUTED;
            break;
        case LXMFStatus::FAILED:
            color = Theme::ERROR_CLR;
            break;
        case LXMFStatus::UNCONFIRMED:
            color = Theme::WARNING_CLR;
            break;
        case LXMFStatus::SENDING:
            color = Theme::WARNING_CLR;
            break;
        case LXMFStatus::QUEUED:
            color = Theme::WARNING_CLR;
            break;
        default:
            color = Theme::TEXT_MUTED;
            break;
    }
    // A failed refresh does not invalidate the last known delivery state or
    // constitute a failed status write. The window owns its delayed notice.
    const char* detail = span.flags & Span::StatusUnavailable ? nullptr :
        messageStatusDetail(static_cast<LXMFStatus>(span.status), span.flags & Span::StatusPending,
                            span.statusError, span.flags & Span::TxSuppressed);
    if (detail) {
        char text[48];
        snprintf(text, sizeof(text), "%s\n%s", glyph, detail);
        lv_label_set_text(lbl, text);
    } else {
        lv_label_set_text(lbl, glyph);
    }
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_point_t textSize;
    lv_txt_get_size(&textSize, lv_label_get_text(lbl), lv_obj_get_style_text_font(lbl, 0),
                    lv_obj_get_style_text_letter_space(lbl, 0),
                    lv_obj_get_style_text_line_space(lbl, 0), LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_obj_set_height(lv_obj_get_parent(lbl), textSize.y);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
}

void LvMessageView::sendCurrentMessage(bool viaLink) {
    if (!_service || _peerHex.empty() || _inputText.empty() || _sendPending) return;
    if (_inputText.size() > MAX_COMPOSER_CHARS) {
        if (_ui) _ui->lvStatusBar().showToast("Message too long", 1500);
        return;
    }
    if (_nextDraftRevision == UINT64_MAX) return;
    const auto revision = _draftRevision;
    const auto identity = _service->status().generation;
    _retainedDraftPeer = _peerHex;
    _retainedDraft = _inputText;
    _retainedDraftRevision = revision;
    _retainedDraftIdentity = identity;
    _sendPending = true;
    const auto id = _service->action(handheld::Operation::Send, _peerHex, _inputText, viaLink,
        [this, revision, identity](const handheld::Result& result) {
            _sendPending = false;
            const bool sameView = _peerHex == _retainedDraftPeer && _service &&
                                  _service->status().generation == identity;
            if (result.outcome == handheld::Outcome::Ok) {
                if (sameView && _draftRevision == revision) _inputText.clear();
                if (_retainedDraftRevision == revision && _retainedDraftIdentity == identity) {
                    _retainedDraft.clear(); _retainedDraftPeer.clear();
                }
            }
            // The screen object is app-owned; its widgets are not. Never touch
            // widgets after navigation destroyed this view.
            if (_screen && sameView) {
                updateComposerState(); updateComposerText();
            }
        });
    if (!id) _sendPending = false;
    if (id && _ui) _ui->lvStatusBar().showToast("Saving message...", 1000);
}

bool LvMessageView::handleKey(const KeyEvent& event) {
    if (_sendOverlay) {
        if (event.character == 0x1B ||
            ((event.del || event.character == 0x08) && !event.repeat)) {
            hideSendModeMenu();
            return true;
        }
        if (event.up || event.left) {
            _sendMenuIdx = (_sendMenuIdx + 2) % 3;
            updateSendModeMenu();
            return true;
        }
        if (event.down || event.right || event.tab) {
            _sendMenuIdx = (_sendMenuIdx + 1) % 3;
            updateSendModeMenu();
            return true;
        }
        if (event.enter || event.character == '\n' || event.character == '\r') {
            chooseSendMode(_sendMenuIdx);
            return true;
        }
        return true;
    }

    if (event.character == 0x1B) {
        goBack();
        return true;
    }

    if (event.del || event.character == 0x08) {
#if !HAS_TOUCH
        if (_inputText.empty() && hasReadFocus()) {
            if (!event.repeat) {
                _service->historyWindow().focusSpan(HistoryWindow::VisibleSpans);
                updateHistoryFocus();
            }
            return true;
        }
#endif
        if (!_inputText.empty()) {
            _inputText.pop_back();
            composerEdited();
            updateComposerState();
        } else if (!event.repeat) {
            // Hold-to-repeat stops at empty; only a fresh tap exits the chat.
            goBack();
        }
        return true;
    }

    if (event.enter || event.character == '\n' || event.character == '\r') {
        if (event.repeat) return true;
        if (hasReadFocus()) {
            readFull(_service->historyWindow().focusedSpan()); return true;
        }
#if !HAS_TOUCH
        // An empty composer lets keyboard-only boards enter Read full
        // selection with Enter; typing returns to composing.
        if (_inputText.empty()) {
            if (_historyNotice && !lv_obj_has_flag(_historyNotice, LV_OBJ_FLAG_HIDDEN)) {
                historyAction(_noticeAction); return true;
            }
            focusNextRead();
            if (hasReadFocus() && _ui)
                _ui->lvStatusBar().showToast("Up/Down: select  Enter: read  Back: cancel", 2000);
            return true;
        }
#endif
        sendCurrentMessage(false);
        return true;
    }

#if !HAS_TOUCH
    if (hasReadFocus() && (event.up || event.down)) {
        focusNextRead(event.up ? -1 : 1);
        return true;
    }
#endif
    if (event.up) { scrollHistory(-30); return true; }
    if (event.down) { scrollHistory(30); return true; }
    if (event.tab) { focusNextRead(); return true; }
    if (event.left || event.right) return true;

    if (event.character >= 0x20 && event.character < 0x7F) {
        if (_inputText.size() >= MAX_COMPOSER_CHARS) {
            if (_ui) _ui->lvStatusBar().showToast("Message too long", 900);
            return true;
        }
        unsigned long inputStartMs = millis();
        if (_service) { _service->historyWindow().focusSpan(HistoryWindow::VisibleSpans); updateHistoryFocus(); }
        _inputText += (char)event.character;
        composerEdited();
        updateComposerState();
        unsigned long elapsed = millis() - inputStartMs;
        if (PerfTrace::shouldLog(elapsed, RSDECK_PERF_UI_TRACE_MS)) {
            Serial.printf("[PERF] Chat input: peer=%s chars=%u total=%lums\n",
                          _peerHex.substr(0, 8).c_str(), (unsigned)_inputText.size(), elapsed);
        }
        return true;
    }

    return false;
}

bool LvMessageView::handleLongPress() {
    if (_inputText.empty()) return false;
    showSendModeMenu();
    return true;
}

void LvMessageView::showSendModeMenu() {
    if (_inputText.empty()) return;
    hideSendModeMenu();
    _sendMenuIdx = 0;

    _sendOverlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(_sendOverlay, 244, 118);
    lv_obj_center(_sendOverlay);
    lv_obj_add_style(_sendOverlay, LvTheme::styleModal(), 0);
    lv_obj_set_style_pad_all(_sendOverlay, 8, 0);
    lv_obj_clear_flag(_sendOverlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(_sendOverlay);
    lv_obj_set_style_text_font(title, &lv_font_rsdeck_12, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(Theme::ACCENT), 0);
    lv_label_set_text(title, "Send mode");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    static const char* labels[3] = {"Send normally", "Send as link", "Cancel"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t* row = lv_obj_create(_sendOverlay);
        lv_obj_set_size(row, 220, 24);
        lv_obj_set_pos(row, 12, 24 + i * 28);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, (void*)(intptr_t)i);
        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            auto* self = (LvMessageView*)lv_event_get_user_data(e);
            int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
            self->chooseSendMode(idx);
        }, LV_EVENT_CLICKED, this);

        _sendLabels[i] = lv_label_create(row);
        lv_obj_set_style_text_font(_sendLabels[i], &lv_font_rsdeck_12, 0);
        lv_label_set_text(_sendLabels[i], labels[i]);
        lv_obj_center(_sendLabels[i]);
        _sendRows[i] = row;
    }

    updateSendModeMenu();
}

void LvMessageView::hideSendModeMenu() {
    if (_sendOverlay) {
        lv_obj_del_async(_sendOverlay);
        _sendOverlay = nullptr;
    }
    for (int i = 0; i < 3; i++) {
        _sendRows[i] = nullptr;
        _sendLabels[i] = nullptr;
    }
}

void LvMessageView::updateSendModeMenu() {
    for (int i = 0; i < 3; i++) {
        if (!_sendRows[i] || !_sendLabels[i]) continue;
        bool selected = i == _sendMenuIdx;
        lv_obj_set_style_bg_color(_sendRows[i],
            lv_color_hex(selected ? Theme::PRIMARY_SUBTLE : Theme::BG_SURFACE), 0);
        lv_obj_set_style_border_color(_sendRows[i],
            lv_color_hex(selected ? Theme::BORDER_ACTIVE : Theme::BORDER), 0);
        lv_obj_set_style_text_color(_sendLabels[i],
            lv_color_hex(selected ? Theme::ACCENT : Theme::TEXT_SECONDARY), 0);
    }
}

void LvMessageView::chooseSendMode(int idx) {
    bool viaLink = idx == 1;
    if (idx == 2) {
        hideSendModeMenu();
        return;
    }
    hideSendModeMenu();
    sendCurrentMessage(viaLink);
}
