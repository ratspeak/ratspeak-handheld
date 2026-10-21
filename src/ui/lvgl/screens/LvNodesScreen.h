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
#if defined(RSSENSECAPD1L)
class LvNomadBrowserScreen;
#endif

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
#if defined(RSSENSECAPD1L)
    void setNomadBrowserScreen(LvNomadBrowserScreen* screen) { _nomadScreen = screen; }
#endif
    bool handleLongPress() override;
    bool wantsVirtualKeyboard() const override { return _actionState == NodeAction::NICKNAME_INPUT; }

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
    // Touch-only boards have no physical Enter/Backspace, so delete confirmation
    // needs tappable buttons, not just the status-bar hint text.
    void showConfirmDelete(const std::string& nodeHex);
    void finishConfirmDelete(bool remove);
#if defined(RSSENSECAPD1L)
    // D1L-only (touch-only, square panel -- see ratspeak-handheld TODO.md's "Nomad
    // browser from Peers menu"): hand the tapped peer off to the dedicated fullscreen
    // LvNomadBrowserScreen and switch to it. The actual NomadNet fetch (and its
    // publicKey, derived from the peer's own saved identity, never a live
    // nomadnetwork.node announce) lives entirely in that screen now.
    void openNomadBrowserScreen();
#endif

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
    // D1L gets a 4th item ("Nomad Browser"); other boards keep the original 3.
#if defined(RSSENSECAPD1L)
    static constexpr int kMenuCount = 4;
#else
    static constexpr int kMenuCount = 3;
#endif
    lv_obj_t* _menuLabels[kMenuCount] = {};
    lv_obj_t* _menuBtns[kMenuCount] = {};
    lv_obj_t* _nicknameBox = nullptr;
    lv_obj_t* _nicknameLbl = nullptr;
    lv_obj_t* _nicknameHint = nullptr;
    lv_obj_t* _confirmBox = nullptr;
#if defined(RSSENSECAPD1L)
    LvNomadBrowserScreen* _nomadScreen = nullptr;
#endif
    // Square/tall panels (today: SenseCAP D1L) have room for the same
    // per-row identicon Contacts already shows; kRowH/kTextX match
    // LvContactsScreen's kContactRowH/kContactTextX exactly for visual
    // consistency between the two lists.
    static constexpr bool kShowAvatar = Theme::SCREEN_W >= 400 && Theme::SCREEN_H >= 400;
    static constexpr int RowHeight = kShowAvatar ? 38 : 36;
    static constexpr int TextX = kShowAvatar ? 48 : 8;
    static constexpr int ViewportHeight = Theme::CONTENT_H - 19;
    // Partial first/last rows, one overscan each side, and one pinned press.
    static constexpr size_t RowPoolSize = (ViewportHeight + RowHeight - 1) / RowHeight + 4;
    handheld::PeerList _peers;
    struct Row {
        lv_obj_t* box = nullptr;
        lv_obj_t* name = nullptr;
        lv_obj_t* meta = nullptr;
        lv_obj_t* id = nullptr;
        lv_obj_t* avatar = nullptr;  // Only created when kShowAvatar.
        std::string nameText;
        std::string hex;
        size_t index = 0;
    };
    std::array<Row, RowPoolSize> _rows;
    // One persistent render buffer per row slot (not per-peer, since rows
    // are a recycled/virtualized pool — see bindRows()).
    std::array<std::vector<uint8_t>, RowPoolSize> _avatarBuffers;
    lv_obj_t* _caption = nullptr;
    lv_obj_t* _extent = nullptr;
    lv_obj_t* _pressedRow = nullptr;
    std::string _pressedHex;
    // Set by a touch long-press on a row; checked by the row's CLICKED
    // branch so the inevitable click-on-release doesn't also open/select —
    // LVGL does not suppress CLICKED after LONG_PRESSED on its own.
    bool _longPressFired = false;
    bool _bindingRows = false;
    bool interactionBusy() const;
    void bindRows();
    void focusSelection();
    void scrollToSelection();
    // Shared by handleLongPress() (encoder/keyboard hold, sources the hex via
    // getFocusedNodeHex()) and the row's native LV_EVENT_LONG_PRESSED touch
    // handler (sources the hex via the press-time-pinned _pressedHex, since
    // these rows don't participate in LVGL click-focus — see bindRows()).
    bool confirmOrOpenAction(const std::string& nodeHex);
    unsigned long _lastRebuild = 0;

    lv_obj_t* _list = nullptr;
    lv_obj_t* _emptyState = nullptr;
};
