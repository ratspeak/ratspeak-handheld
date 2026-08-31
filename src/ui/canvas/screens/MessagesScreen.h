#pragma once

#include <map>
#include "Screen.h"
#include "widgets/ScrollList.h"
#include "reticulum/LXMFManager.h"

class AnnounceManager;
class ProtocolBackend;

class MessagesScreen : public Screen {
public:
    void render(M5Canvas& canvas) override;
    bool handleKey(const KeyEvent& event) override;
    const char* title() const override { return "Messages"; }
    void onEnter() override;

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

private:
    void refreshList();
    void showContextMenu(int idx);
    void executeContextAction();
    void exitContextMenu();

    LXMFManager* _lxmf = nullptr;
    AnnounceManager* _am = nullptr;
    ProtocolBackend* _backend = nullptr;
    ScrollList _list;
    std::vector<std::string> _peerHexes;
    unsigned long _lastRefresh = 0;
    OpenConversationCb _openCb;
    AddContactCb _addContactCb;
    bool _needsRefresh = false;

    // Context menu (triggered by Delete key)
    bool _showingContext = false;
    ScrollList _contextList;
    std::string _contextPeerHex;
    bool _contextIsContact = false;
};
