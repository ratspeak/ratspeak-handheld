#pragma once

#include "UIManager.h"
#include "runtime/ServiceClient.h"
#include <functional>
#include <string>

class LvMessagesScreen : public LvScreen {
public:
    using OpenCallback = std::function<void(const std::string&)>;
    void setService(handheld::ServiceClient* service) { _service = service; }
    void setAnnounceManager(handheld::NodeView* nodes) { _am = nodes; }
    void setBackend(handheld::ProtocolView* backend) { _backend = backend; }
    void setOpenCallback(OpenCallback callback) { _onOpen = callback; }
    void setUIManager(UIManager* ui) { _ui = ui; }
    void createUI(lv_obj_t* parent) override;
    void destroyUI() override;
    void refreshUI() override;
    void onEnter() override;
    void onExit() override;
    bool handleKey(const KeyEvent&) override;
    bool handleLongPress() override;
    const char* title() const override { return "Messages"; }

private:
    using Window = handheld::history::ConversationList;
    using Row = Window::Row;
    struct RowWidgets { lv_obj_t *row = nullptr, *name = nullptr, *preview = nullptr, *status = nullptr; };
    static constexpr size_t RowCount = Window::PageSize;
    enum class Navigation : uint8_t { First, Previous, Next, Last };
    enum LongPressState : uint8_t { LP_NONE, LP_MENU, LP_CONFIRM_DELETE };
    bool bound() const;
    void detachRows();
    void bindRows();
    void updateCaptions();
    void updateStatuses();
    void updateNames();
    void pollName();
    void reportViewport();
    void navigate(Navigation);
    void applyUpdate();
    void openRow(size_t);
    int focusedIndex() const;
    void showActionMenu(size_t);
    void hideActionMenu();
    void rebuildActionOverlay();
    void performAction(handheld::Operation);
    void notice(const char*);

    handheld::ServiceClient* _service = nullptr;
    handheld::NodeView* _am = nullptr;
    handheld::ProtocolView* _backend = nullptr;
    UIManager* _ui = nullptr;
    OpenCallback _onOpen;
    RowWidgets _rows[RowCount];
    lv_obj_t *_list = nullptr, *_caption = nullptr, *_empty = nullptr, *_update = nullptr;
    lv_obj_t* _emptyState = nullptr;
    lv_obj_t* _navigation[4] = {};
    lv_obj_t* _actionOverlay = nullptr;
    lv_obj_t* _actionRows[3] = {};
    uint64_t _namesResolved = 0;
    uint32_t _boundRevision = 0, _boundIdentity = 0, _statusRevision = 0;
    uint32_t _boundPage = 1;
    uint32_t _loadStarted = 0;
    uint32_t _nodeRevision = 0;
    uint32_t _nameRequest = 0, _actionRequest = 0, _actionIdentity = 0;
    uint8_t _actionPeer[16] = {};
    uint8_t _rowCount = 0, _menuIdx = 0;
    LongPressState _lpState = LP_NONE;
    bool _active = false, _binding = false, _focusActive = false;
    bool _nameFailed = false;
    bool _loadPending = false;
};
