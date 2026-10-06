#pragma once

#include "Screen.h"
#include "widgets/TextInput.h"
#include "reticulum/LXMFManager.h"
#include "protocol/OutgoingContract.h"
#include "history/HistoryWindow.h"
#include "RrcClient.h"
#include "RrcHistoryAdapter.h"
#include "ui/RrcMessageMenu.h"
#include <string>

class AnnounceManager;
class ProtocolBackend;

class MessageView : public Screen {
public:
    void render(M5Canvas& canvas) override;
    bool handleKey(const KeyEvent& event) override;
    const char* title() const override { return "Chat"; }
    void onEnter() override;
    void onExit() override;

    void setLXMFManager(LXMFManager* lxmf) { _lxmf = lxmf; }
    void setVoiceCallback(std::function<void(const char*,uint32_t,bool)> callback) {_voice=std::move(callback);}
    void setVoiceCloseCallback(std::function<void()> callback) {_voiceClose=std::move(callback);}
    void setBackend(ProtocolBackend* backend) { _backend = backend; }
    void setMessageStore(MessageStore* store) { _store = store; }
    void setAnnounceManager(AnnounceManager* am) { _am = am; }
    bool setPeerHex(const std::string& peerHex);
    bool prepareMaintenance() { _rrcSendRequested=false;return leaveRrcDraft(); }
    bool setRrcConversation(const handheld::rrc::Conversation&, const char* insert = nullptr);
    bool pollRrc(bool allowAdmission = true);
    void notifyNewMessage(const handheld::storage::RecordKey&);
    // App-owned polling continues while this screen is hidden or powered off.
    bool pollSubmission();
    bool pollReadMarker();
    bool pollHistory(bool allowAdmission = true);

    // Status callback — update chat line color when send completes
    void notifyStatusChange(const std::string& peerHex, uint32_t counter, LXMFStatus status);

    // Callback to return to messages list
    using BackCallback = std::function<void()>;
    void setBackCallback(BackCallback cb) { _backCb = cb; }

    // Callback to update unread badge after markRead
    using UnreadUpdateCb = std::function<void()>;
    void setUnreadUpdateCallback(UnreadUpdateCb cb) { _unreadCb = cb; }

private:
    using RrcTools=handheld::ui::RrcMessageMenu;
    RrcTools _rrcTools;
    void openRrcTools(size_t index);
    void readRrcTool(RrcTools::Action action,bool metadata=false);
    void activateRrcTool();
    void closeRrcTools();
    void showRrcGuidance(RrcTools::State);
    void activateRrcGuidance();
    void renderRrcTools(M5Canvas&);
    bool leaveRrcDraft();
    bool sameRrcIdentity(const handheld::rrc::Conversation&, const uint8_t[16]) const;
    void loadRrcDraft();
    void saveRrcDraft(bool immediate = false, bool forSend = false);
    void clearConfirmedRrcDraft();
    void enterRrcHistory();
    void sendRrcMessage();
    void rrcNotice(const char*);
    void rrcEdited();
    handheld::rrc::Command rrcCommand(handheld::rrc::Action) const;
    bool rrcWritable() const { return _rrcBinding.room[0] || _rrcBinding.privateNotice(); }
    MessageStore* _store = nullptr;
    handheld::canvas::RrcClient _rrcClient;
    handheld::canvas::RrcHistoryAdapter _rrcHistory;
    handheld::rrc::Conversation _rrcBinding, _rrcSendingBinding;
    char _rrcInsert[36]{};
    uint8_t _rrcIdentity[16]{}, _rrcSendIdentity[16]{};
    uint64_t _rrcPreparedRevision = 0, _rrcSavedRevision = 0, _rrcRequestedRevision = 0, _rrcClearEditorRevision = 0;
    uint32_t _rrcView = 0, _rrcSentView = 0, _rrcSendId = 0, _rrcRetryAt = 0, _rrcEditAt = 0;
    uint32_t _rrcStorageRevision = 0, _rrcSentStorageRevision = 0, _rrcClearView = 0, _rrcReadThrough = 0, _rrcVisibleThrough = 0, _rrcObservedRevision = 0;
    bool _rrcMode = false, _rrcLoaded = false, _rrcLoading = false, _rrcSaving = false;
    bool _rrcEmote = false, _rrcUncertain = false, _rrcResendConfirmed = false;
    bool _rrcSendRequested = false, _rrcRequestedEmote = false, _rrcClearPending = false, _rrcClearing = false;
    void refreshMessages();
    void sendCurrentInput();
    bool prepareDraft(String& identity);
    static constexpr size_t DraftLength = 400;

    LXMFManager* _lxmf = nullptr;
    ProtocolBackend* _backend = nullptr;
    AnnounceManager* _am = nullptr;
    std::string _peerHex;
    handheld::history::HistoryWindow _history;
    TextInput _input;
    BackCallback _backCb;
    std::function<void(const char*,uint32_t,bool)> _voice;
    std::function<void()> _voiceClose;
    UnreadUpdateCb _unreadCb;
    bool _needsRefresh = false;

    handheld::outgoing::Ticket _sendTicket;
    uint64_t _submittedRevision = 0;
    bool _submissionEdited = false;
    bool _draftReady = false;
    std::string _retainedDraft, _retainedPeer, _retainedIdentity;
    const char* _sendNotice = nullptr;
    uint32_t _sendNoticeSince = 0;
    handheld::storage::Ticket _readTicket;
    char _readPeer[33] = {};
    uint32_t _readRetryAt = 0;
    bool _visible = false, _readRequested = false;
};
