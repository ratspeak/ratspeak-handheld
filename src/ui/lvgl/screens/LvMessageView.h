#pragma once

#include "UIManager.h"
#include "runtime/ServiceClient.h"
#include "reticulum/LXMFMessage.h"
#include <functional>
#include <string>
#include <array>
#include "history/HistoryWindow.h"


class LvMessageView : public LvScreen {
public:
    void setService(handheld::ServiceClient* service) { _service = service; }
    using BackCallback = std::function<void()>;

    void createUI(lv_obj_t* parent) override;
    void destroyUI() override;
    void refreshUI() override;
    void onEnter() override;
    void onExit() override;
    bool handleKey(const KeyEvent& event) override;
    bool handleLongPress() override;
    TextInputRequest textInput() const override;

    void setPeerHex(const std::string& hex);
    void setBackend(handheld::ProtocolView* backend) { _backend = backend; }
    void setAnnounceManager(handheld::NodeView* am) { _am = am; }
    void setUIManager(class UIManager* ui) { _ui = ui; }
    void setBackCallback(BackCallback cb) { _onBack = cb; }

    const char* title() const override { return "Chat"; }

private:
    handheld::ServiceClient* _service = nullptr;
    void sendCurrentMessage(bool viaLink = false);
    void rebuildMessages();
    using HistoryWindow = handheld::history::HistoryWindow;
    using Span = HistoryWindow::Span;
    void appendMessage(size_t index, const Span& span, const char* text);
    void clearMessages();
    bool windowMatches() const;
    bool boundWindow() const;
    void historyAction(unsigned action);
    void readFull(size_t index);
    void goBack();
    void scrollHistory(int pixels);
    bool hasReadFocus() const;
    void focusNextRead(int direction = 1);
    void updateHistoryControls();
    void updateHistoryFocus();
    void saveScroll(bool userChange = false);
    void updateHeader();
    void markVisibleConversationRead();
    void updateComposerState();
    void refreshComposerPlaceholder();
    void updateComposerText();
    void composerEdited();
    void showSendModeMenu();
    void hideSendModeMenu();
    void updateSendModeMenu();
    void chooseSendMode(int idx);

    handheld::ProtocolView* _backend = nullptr;
    handheld::NodeView* _am = nullptr;
    class UIManager* _ui = nullptr;
    BackCallback _onBack;
    std::string _peerHex;
    std::string _inputText;
    bool _markReadPending = false;
    bool _readInFlight = false, _readRetry = false;
    uint32_t _readRequest = 0, _readRetryStarted = 0, _readThrough = 0;
    bool _nameInFlight = false, _nameResolved = false;
    uint32_t _nameNodeRevision = 0, _nameIdentity = 0;
    bool _sendPending = false;
    std::string _retainedDraftPeer, _retainedDraft;
    uint64_t _nextDraftRevision = 1, _draftRevision = 1, _retainedDraftRevision = 0;
    uint32_t _retainedDraftIdentity = 0;
    uint32_t _lastHistoryRevision = 0;
    uint32_t _lastStatusRevision = 0, _boundIdentity = 0;
    HistoryWindow::Mode _boundMode = HistoryWindow::Mode::Closed;
    bool _entered = false, _binding = false, _atBottom = true, _scrollToEnd = true;
    uint8_t _rowCount = 0;

    void updateMessageStatus(size_t index, const Span& span);
    static void applyStatusGlyph(lv_obj_t* label, const Span& span);

    // LVGL widgets
    lv_obj_t* _header = nullptr;
    lv_obj_t* _lblHeader = nullptr;
    lv_obj_t* _lblHeaderMeta = nullptr;
    lv_obj_t* _msgScroll = nullptr;
    lv_obj_t* _historyNotice = nullptr;
    lv_obj_t* _historyNoticeLabel = nullptr;
    lv_obj_t* _emptyLabel = nullptr;
    bool _touchScrolling = false;
    int32_t _touchScrollStart = 0, _touchScrollPeak = 0;
    unsigned _noticeAction = 3;
    lv_obj_t* _inputRow = nullptr;
    lv_obj_t* _textarea = nullptr;
    lv_obj_t* _btnSend = nullptr;
    lv_obj_t* _sendOverlay = nullptr;
    lv_obj_t* _sendRows[3] = {};
    lv_obj_t* _sendLabels[3] = {};
    int _sendMenuIdx = 0;
    bool _suppressNextSendClick = false;
    bool _composing = false;  // The composer was tapped; keep its keyboard up.

    // Every label points into the client's leased publication, never a body copy.
    std::array<lv_obj_t*, HistoryWindow::VisibleSpans> _statusLabels{}, _textLabels{}, _bubbleBoxes{}, _readButtons{};

    static constexpr size_t MAX_COMPOSER_CHARS = 120;
};
