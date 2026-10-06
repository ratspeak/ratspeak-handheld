#pragma once

#include "UIManager.h"
#include "runtime/ServiceClient.h"
#include "reticulum/LXMFMessage.h"
#include <functional>
#include <string>
#include <array>
#include "history/HistoryWindow.h"
#include "ui/RrcMessageMenu.h"
#include "ui/MessageMenu.h"


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

    bool setPeerHex(const std::string& hex);
    bool prepareMaintenance() { _rrcSendRequested=false;return leaveRrcDraft(); }
    bool setRrcConversation(const handheld::rrc::Conversation&, const char* insert = nullptr);
    void pollRrc(); // App-owned, including hidden drafts and send completions.
    void setBackend(handheld::ProtocolView* backend) { _backend = backend; }
    void setAnnounceManager(handheld::NodeView* am) { _am = am; }
    void setUIManager(class UIManager* ui) { _ui = ui; }
    void setBackCallback(BackCallback cb) { _onBack = cb; }

    const char* title() const override { return "Chat"; }

private:
    using RrcTools=handheld::ui::RrcMessageMenu;
    RrcTools _rrcTools;
    handheld::ui::MessageMenu _messageTools;
    bool _messageActionPending=false,_selectAfterPage=false,_dismissedDelete=false;
    uint32_t _audioPressPublication=0,_audioPressOwner=0;
    size_t _audioPressIndex=handheld::history::HistoryWindow::VisibleSpans;
    handheld::memo::Phase _audioPressPhase=handheld::memo::Phase::Unavailable;
    uint32_t _messagePressedSerial=0;
    void openMessageTools(size_t);
    void activateMessageTool();
    void openRrcTools(size_t index);
    void readRrcTool(RrcTools::Action action,bool metadata=false);
    void activateRrcTool();
    void closeRrcTools();
    void showRrcGuidance(RrcTools::State);
    void activateRrcGuidance();
    bool leaveRrcDraft();
    void loadRrcDraft();
    void saveRrcDraft(bool immediate = false, bool forSend = false);
    void clearConfirmedRrcDraft();
    void enterRrcHistory();
    void sendRrcMessage();
    void rrcNotice(const char*);
    handheld::rrc::Command rrcCommand(handheld::rrc::Action) const;
    bool rrcWritable() const { return _rrcBinding.room[0] || _rrcBinding.privateNotice(); }
    handheld::rrc::Conversation _rrcBinding;
    handheld::rrc::Conversation _rrcSendingBinding;
    char _rrcInsert[36]{}; // One pending nickname insertion, never message text.
    uint64_t _rrcPreparedRevision = 0, _rrcSavedRevision = 0, _rrcSentDraftRevision = 0;
    uint64_t _rrcRequestedRevision = 0, _rrcClearEditorRevision = 0;
    uint32_t _rrcIdentity = 0, _rrcView = 0, _rrcSentView = 0, _rrcSendId = 0, _rrcRetryAt = 0, _rrcEditAt = 0;
    bool _rrcMode = false, _rrcLoaded = false, _rrcLoading = false, _rrcSaving = false;
    bool _rrcEmote = false, _rrcUncertain = false, _rrcResendConfirmed = false;
    uint32_t _rrcStorageRevision = 0, _rrcSentStorageRevision = 0, _rrcSendIdentity = 0, _rrcClearView = 0;
    bool _rrcSendRequested = false, _rrcRequestedEmote = false, _rrcClearPending = false, _rrcClearing = false;
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
    bool dismissSelection();
    void selectVisibleMessage();
    void revealSelection();
    void focusNextRead(int direction = 1);
    void updateHistoryControls();
    void updateHistoryFocus();
    void updateAudioControls();
    void saveScroll(bool userChange = false);
    void updateHeader();
    void markVisibleConversationRead();
    void updateComposerState();
    void refreshComposerPlaceholder();
    void updateComposerText();
    void composerEdited();
    int sendMenuCount() const {if(_messageTools.visible()) return int(_messageTools.count());return _rrcMode || !_service || !(_service->status().memo.capabilities&1)?3:4;}
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
    void applyStatusGlyph(lv_obj_t* label, const Span& span);

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
    lv_obj_t* _sendRows[8] = {};
    lv_obj_t* _sendLabels[8] = {};
    int _sendMenuIdx = 0;
    bool _suppressNextSendClick = false;

    // Every label points into the client's leased publication, never a body copy.
    std::array<lv_obj_t*, HistoryWindow::VisibleSpans> _statusLabels{}, _textLabels{}, _bubbleBoxes{}, _readButtons{}, _moreButtons{}, _audioErrors{};

    static constexpr size_t MAX_COMPOSER_CHARS = 120;
};
