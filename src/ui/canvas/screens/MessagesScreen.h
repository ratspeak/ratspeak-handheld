#pragma once

#include "Screen.h"
#include "widgets/ScrollList.h"
#include "reticulum/LXMFManager.h"
#include "history/ConversationWindow.h"

class AnnounceManager;
class ProtocolBackend;

class MessagesScreen : public Screen {
public:
    void render(M5Canvas& canvas) override;
    bool handleKey(const KeyEvent& event) override;
    const char* title() const override { return "Messages"; }
    void onEnter() override;
    void onExit() override { _visible = false; _conversations.close(); }

    void setLXMFManager(LXMFManager* lxmf) { _lxmf = lxmf; }
    void setAnnounceManager(AnnounceManager* am) { _am = am; }
    void setBackend(ProtocolBackend* backend) { _backend = backend; }

    // Callback to open a conversation
    using OpenConversationCb = std::function<void(const std::string& peerHex)>;
    void setOpenCallback(OpenConversationCb cb) { _openCb = cb; }

    // Callback to add contact
    using AddContactCb = std::function<void(const std::string& peerHex)>;
    void setAddContactCallback(AddContactCb cb) { _addContactCb = cb; }

    void notifyNewMessage() { _needsRefresh = true; }
    bool pollConversations(bool allowAdmission = true);
    // The app consumes accepted deletion results even while this screen is hidden.
    bool pollDeletion();

private:
    using Conversations = handheld::history::ConversationList;
    std::string peerHex(size_t index) const;
    std::string peerLabel(const std::string& peer) const;
    void renderList(M5Canvas&, int y, int height);
    void renderNavigation(M5Canvas&, int y);
    bool pageEnabled(int action) const;
    void activatePage(int action);
    void movePageFocus(int direction);
    void showContextMenu(int idx);
    void executeContextAction();
    void exitContextMenu();

    LXMFManager* _lxmf = nullptr;
    AnnounceManager* _am = nullptr;
    ProtocolBackend* _backend = nullptr;
    Conversations _conversations;
    OpenConversationCb _openCb;
    AddContactCb _addContactCb;
    bool _needsRefresh = false, _visible = false, _deleteSettled = false;
    int8_t _pageFocus = -1; // -1 = conversation list; 0..3 = first/previous/next/last.
    handheld::storage::Ticket _deleteTicket;
    uint8_t _deletePeer[16] = {};
    const char* _deleteNotice = nullptr;
    uint32_t _deleteNoticeSince = 0;

    // Context menu (triggered by Delete key)
    bool _showingContext = false;
    ScrollList _contextList;
    std::string _contextPeerHex;
    bool _contextIsContact = false;
};
