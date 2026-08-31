#pragma once

#include "UIManager.h"
#include "runtime/ServiceClient.h"
#include <functional>
#include <string>
#include <vector>


class LvContactsScreen : public LvScreen {
public:
    void setService(handheld::ServiceClient* service) { _service = service; }
    using NodeSelectedCallback = std::function<void(const std::string& peerHex)>;

    void createUI(lv_obj_t* parent) override;
    void refreshUI() override;
    void onEnter() override;
    bool handleKey(const KeyEvent& event) override;

    void setAnnounceManager(handheld::NodeView* am) { _am = am; }
    void setNodeSelectedCallback(NodeSelectedCallback cb) { _onSelect = cb; }
    void setShowQrCallback(std::function<void()> cb) { _showQrCb = cb; }
    void setUIManager(class UIManager* ui) { _ui = ui; }
    bool handleLongPress() override;

    const char* title() const override { return "Contacts"; }

private:
    handheld::ServiceClient* _service = nullptr;
    void rebuildList();

    handheld::NodeView* _am = nullptr;
    class UIManager* _ui = nullptr;
    NodeSelectedCallback _onSelect;
    std::function<void()> _showQrCb;
    bool _confirmDelete = false;
    bool _focusActive = false;
    // Armed delete target + row snapshot are hash-addressed: node indices go
    // stale when the announce table compacts while the confirm toast is open.
    std::string _deleteHex;
    int _lastContactCount = -1;
    unsigned long _lastRebuild = 0;
    static constexpr unsigned long REBUILD_INTERVAL_MS = 30000;
    std::vector<std::string> _contactHexes;
    std::vector<std::vector<uint8_t>> _avatarBuffers;

    lv_obj_t* _list = nullptr;
    lv_obj_t* _emptyState = nullptr;
};
