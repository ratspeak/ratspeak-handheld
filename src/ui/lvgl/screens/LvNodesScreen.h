#pragma once

#include "UIManager.h"
#include "runtime/ServiceClient.h"
#include "ui/PeerList.h"
#include "Theme.h"
#include <array>
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
    static constexpr int RowHeight = 36;
    static constexpr int ViewportHeight = Theme::CONTENT_H - 19;
    // Partial first/last rows, one overscan each side, and one pinned press.
    static constexpr size_t RowPoolSize = (ViewportHeight + RowHeight - 1) / RowHeight + 4;
    handheld::PeerList _peers;
    struct Row {
        lv_obj_t* box = nullptr;
        lv_obj_t* name = nullptr;
        lv_obj_t* meta = nullptr;
        lv_obj_t* id = nullptr;
        std::string nameText;
        std::string hex;
        size_t index = 0;
    };
    std::array<Row, RowPoolSize> _rows;
    lv_obj_t* _caption = nullptr;
    lv_obj_t* _extent = nullptr;
    lv_obj_t* _pressedRow = nullptr;
    std::string _pressedHex;
    bool _bindingRows = false;
    bool interactionBusy() const;
    void bindRows();
    void focusSelection();
    void scrollToSelection();
    unsigned long _lastRebuild = 0;

    lv_obj_t* _list = nullptr;
    lv_obj_t* _emptyState = nullptr;
};
