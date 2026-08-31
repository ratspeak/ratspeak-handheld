#pragma once

#include "UIManager.h"
#include "runtime/ServiceClient.h"
#include "reticulum/LXMFMessage.h"
#include <functional>
#include <string>
#include <vector>


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

    void setPeerHex(const std::string& hex) { _peerHex = hex; }
    void setLXMFManager(handheld::MessageViewModel* lxmf) { _lxmf = lxmf; }
    void setBackend(handheld::ProtocolView* backend) { _backend = backend; }
    void setAnnounceManager(handheld::NodeView* am) { _am = am; }
    void setUIManager(class UIManager* ui) { _ui = ui; }
    void setBackCallback(BackCallback cb) { _onBack = cb; }

    const char* title() const override { return "Chat"; }

private:
    handheld::ServiceClient* _service = nullptr;
    void sendCurrentMessage(bool viaLink = false);
    void rebuildMessages();
    void appendMessage(const LXMFMessage& msg, bool deferLayout = false);
    std::string getPeerName();
    void updateHeader();
    void markVisibleConversationRead();
    void updateComposerState();
    void refreshComposerPlaceholder();
    void updateComposerText();
    void showSendModeMenu();
    void hideSendModeMenu();
    void updateSendModeMenu();
    void chooseSendMode(int idx);

    handheld::MessageViewModel* _lxmf = nullptr;
    handheld::ProtocolView* _backend = nullptr;
    handheld::NodeView* _am = nullptr;
    class UIManager* _ui = nullptr;
    BackCallback _onBack;
    std::string _peerHex;
    std::string _inputText;
    int _lastMsgCount = -1;
    int _knownTotalCount = -1;
    unsigned long _lastRefreshMs = 0;
    std::vector<LXMFMessage> _cachedMsgs;
    bool _markReadPending = false;
    bool _sendPending = false;
    std::string _retainedDraftPeer, _retainedDraft;
    uint32_t _lastHistoryRevision = 0;
    handheld::HistoryState _shownHistoryState = handheld::HistoryState::Closed;

    void updateMessageStatus(int msgIdx, LXMFStatus status);
    static void applyStatusGlyph(lv_obj_t* lbl, LXMFStatus status);

    // LVGL widgets
    lv_obj_t* _header = nullptr;
    lv_obj_t* _lblHeader = nullptr;
    lv_obj_t* _lblHeaderMeta = nullptr;
    lv_obj_t* _msgScroll = nullptr;
    lv_obj_t* _inputRow = nullptr;
    lv_obj_t* _textarea = nullptr;
    lv_obj_t* _btnSend = nullptr;
    lv_obj_t* _sendOverlay = nullptr;
    lv_obj_t* _sendRows[3] = {};
    lv_obj_t* _sendLabels[3] = {};
    int _sendMenuIdx = 0;
    bool _suppressNextSendClick = false;

    // Per-message status labels for partial updates (avoids full rebuild)
    std::vector<lv_obj_t*> _statusLabels;
    std::vector<lv_obj_t*> _textLabels;
    std::vector<lv_obj_t*> _bubbleBoxes;

    static constexpr unsigned long REFRESH_INTERVAL_MS = 2000;  // Check for new messages every 2s
    static constexpr size_t CHAT_VIEW_MAX_MESSAGES = 40;
    static constexpr size_t MAX_COMPOSER_CHARS = 120;
};
