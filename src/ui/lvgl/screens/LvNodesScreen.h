#pragma once

#include "UIManager.h"
#include "runtime/ServiceClient.h"
#include <functional>
#include <string>
#include <vector>

class UserConfig;

class LvNodesScreen : public LvScreen {
public:
    void setService(handheld::ServiceClient* service) { _service = service; }
    using NodeSelectedCallback = std::function<void(const std::string& peerHex)>;

    void createUI(lv_obj_t* parent) override;
    void destroyUI() override;
    void refreshUI() override;
    void onEnter() override;
    bool handleKey(const KeyEvent& event) override;

    void setAnnounceManager(handheld::NodeView* am) { _am = am; }
    void setNodeSelectedCallback(NodeSelectedCallback cb) { _onSelect = cb; }
    void setUIManager(class UIManager* ui) { _ui = ui; }
    void setUserConfig(UserConfig* cfg) { _cfg = cfg; }
    bool handleLongPress() override;

    const char* title() const override { return "Peers"; }

private:
    handheld::ServiceClient* _service = nullptr;
    void rebuildList();
    std::string getFocusedNodeHex() const;

    // Action modal helpers
    enum class NodeAction { BROWSE, ACTION_MENU, NICKNAME_INPUT };
    void showActionMenu(const std::string& nodeHex);
    void hideOverlay();
    void showNicknameInput();
    void updateMenuSelection();
    void updateNicknameDisplay();
    void updateOverlayDetails(const char* title);

    handheld::NodeView* _am = nullptr;
    class UIManager* _ui = nullptr;
    UserConfig* _cfg = nullptr;
    NodeSelectedCallback _onSelect;
    bool _confirmDelete = false;
    bool _focusActive = false;

    // Action modal state
    NodeAction _actionState = NodeAction::BROWSE;
    int _menuIdx = 0;
    // Armed target is hash-addressed: node indices go stale when the announce
    // table compacts (evictStale/at-cap swap-and-pop) while a modal is open.
    std::string _actionNodeHex;
    String _nicknameText;

    // Overlay widgets
    lv_obj_t* _overlay = nullptr;
    lv_obj_t* _overlayTitle = nullptr;
    lv_obj_t* _overlayMeta = nullptr;
    lv_obj_t* _overlayReach = nullptr;
    lv_obj_t* _menuLabels[3] = {};
    lv_obj_t* _menuBtns[3] = {};
    lv_obj_t* _nicknameBox = nullptr;
    lv_obj_t* _nicknameLbl = nullptr;
    lv_obj_t* _nicknameHint = nullptr;
    int _lastNodeCount = -1;
    int _lastContactCount = -1;

    // Sorted index vectors (into _am->nodes())
    std::vector<int> _sortedContactIndices;
    std::vector<int> _sortedOnlineIndices;
    // Row → dest-hash snapshot (widget user_data indexes this, never _nodes)
    std::vector<std::string> _rowHexes;

    unsigned long _lastRebuild = 0;
    static constexpr unsigned long REBUILD_INTERVAL_MS = 5000;
    static constexpr unsigned long AGE_REBUILD_INTERVAL_MS = 30000;

    lv_obj_t* _list = nullptr;
    lv_obj_t* _emptyState = nullptr;
};
