#pragma once

#include "Screen.h"
#include "widgets/TextInput.h"
#include "reticulum/LXMFManager.h"
#include <vector>
#include <string>
#include <map>

class AnnounceManager;
class ProtocolBackend;

class MessageView : public Screen {
public:
    void render(M5Canvas& canvas) override;
    bool handleKey(const KeyEvent& event) override;
    const char* title() const override { return "Chat"; }
    void onEnter() override;

    void setLXMFManager(LXMFManager* lxmf) { _lxmf = lxmf; }
    void setBackend(ProtocolBackend* backend) { _backend = backend; }
    void setAnnounceManager(AnnounceManager* am) { _am = am; }
    void setPeerHex(const std::string& peerHex) { _peerHex = peerHex; }
    void notifyNewMessage() { _needsRefresh = true; }
    void notifyNewMessage(const LXMFMessage& msg);

    // Status callback — update chat line color when send completes
    void notifyStatusChange(const std::string& peerHex, double timestamp, LXMFStatus status);

    // Callback to return to messages list
    using BackCallback = std::function<void()>;
    void setBackCallback(BackCallback cb) { _backCb = cb; }

    // Callback to update unread badge after markRead
    using UnreadUpdateCb = std::function<void()>;
    void setUnreadUpdateCallback(UnreadUpdateCb cb) { _unreadCb = cb; }

private:
    void refreshMessages();
    void sendCurrentInput();

    // Cached display lines
    struct ChatLine {
        std::string text;
        uint16_t color;
    };

    LXMFManager* _lxmf = nullptr;
    ProtocolBackend* _backend = nullptr;
    AnnounceManager* _am = nullptr;
    std::string _peerHex;
    std::vector<LXMFMessage> _messages;
    std::vector<ChatLine> _chatLines;
    int _scrollOffset = 0;
    TextInput _input;
    unsigned long _lastRefresh = 0;
    BackCallback _backCb;
    UnreadUpdateCb _unreadCb;
    bool _needsRefresh = false;
    bool _hasNewBelow = false;  // True if new messages arrived while scrolled up

    // Pending messages not yet flushed to disk — merged on refreshMessages()
    std::map<std::string, std::vector<LXMFMessage>> _pendingMessages;
};
