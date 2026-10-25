#pragma once

#include <vector>
#include <string>
#include <functional>
#include "Screen.h"
#include "ui/PeerList.h"
#include "widgets/ScrollList.h"
#include "reticulum/AnnounceManager.h"

class NodesScreen : public Screen {
public:
    void setAnnounceManager(AnnounceManager* mgr) { _announces = mgr; }

    // Callback when user selects a node (for starting conversation)
    using NodeSelectedCb = std::function<void(const std::string& peerHex)>;
    void setNodeSelectedCallback(NodeSelectedCb cb) { _selectCb = cb; }

    // Callback for save/unsave contact
    using NodeSaveCb = std::function<void(const std::string& peerHex, bool save)>;
    void setNodeSaveCallback(NodeSaveCb cb) { _saveCb = cb; }

    void onEnter() override;
    void onExit() override;
    void render(M5Canvas& canvas) override;
    bool handleKey(const KeyEvent& event) override;
    const char* title() const override { return "Peers"; }

private:
    void refreshList();
    void showActionMenu(int nodeIdx);
    void executeAction(int actionIdx);
    void exitActionMenu();

    AnnounceManager* _announces = nullptr;
    handheld::PeerList _peers;
    unsigned long _lastRefresh = 0;
    NodeSelectedCb _selectCb;
    NodeSaveCb _saveCb;

    // Action menu state
    bool _showingActions = false;
    ScrollList _actionList;
    std::string _selectedNodeName;
    std::string _selectedNodeHash;
    bool _selectedNodeSaved = false;

};
