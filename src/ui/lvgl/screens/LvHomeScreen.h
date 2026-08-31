#pragma once

#include "UIManager.h"
#include "runtime/ServiceClient.h"
#include <Arduino.h>
#include <functional>
#include <vector>

class UserConfig;

class LvHomeScreen : public LvScreen {
public:
    void setService(handheld::ServiceClient* service) { _service = service; }
    void createUI(lv_obj_t* parent) override;
    void refreshUI() override;
    void onEnter() override;
    bool handleKey(const KeyEvent& event) override;

    void setBackend(handheld::ProtocolView* backend) { _backend = backend; }
    void setUserConfig(UserConfig* cfg) { _cfg = cfg; }
    void setLXMFManager(handheld::MessageViewModel* lxmf) { _lxmf = lxmf; }
    void setAnnounceManager(handheld::NodeView* am) { _am = am; }
    void setAnnounceCallback(std::function<void()> cb) { _announceCb = cb; }
    void setAudioToggleCallback(std::function<void()> cb) { _audioToggleCb = cb; }
    void setLoraToggleCallback(std::function<void()> cb) { _loraToggleCb = cb; }
    void setTCPToggleCallback(std::function<void()> cb) { _tcpToggleCb = cb; }
    void setWiFiToggleCallback(std::function<void()> cb) { _wifiToggleCb = cb; }
    void setGPSToggleCallback(std::function<void()> cb) { _gpsToggleCb = cb; }
    void setPeersCallback(std::function<void()> cb) { _peersCb = cb; }

    const char* title() const override { return "Home"; }

private:
    handheld::ServiceClient* _service = nullptr;
    handheld::ProtocolView* _backend = nullptr;
    UserConfig* _cfg = nullptr;
    handheld::MessageViewModel* _lxmf = nullptr;
    handheld::NodeView* _am = nullptr;
    std::function<void()> _announceCb;
    std::function<void()> _audioToggleCb;
    std::function<void()> _loraToggleCb;
    std::function<void()> _tcpToggleCb;
    std::function<void()> _wifiToggleCb;
    std::function<void()> _gpsToggleCb;
    std::function<void()> _peersCb;
    unsigned long _lastRefreshMs = 0;
    unsigned long _lastUptime = 0;
    uint32_t _lastHeap = 0;
    String _avatarSeed;
    std::vector<uint8_t> _avatarBuffer;

    lv_obj_t* _identityPanel = nullptr;
    lv_obj_t* _avatarBox = nullptr;
    lv_obj_t* _avatarCanvas = nullptr;
    lv_obj_t* _chipLora = nullptr;
    lv_obj_t* _chipTcp = nullptr;
    lv_obj_t* _chipWifi = nullptr;
    lv_obj_t* _statNodes = nullptr;
    lv_obj_t* _statPaths = nullptr;
    lv_obj_t* _statLinks = nullptr;

    lv_obj_t* _lblConsoleTitle = nullptr;
    lv_obj_t* _lblName = nullptr;
    lv_obj_t* _lblId = nullptr;
    lv_obj_t* _lblIdentity = nullptr;
    lv_obj_t* _lblStatus = nullptr;
    lv_obj_t* _lblLoraState = nullptr;
    lv_obj_t* _lblTcpState = nullptr;
    lv_obj_t* _lblWifiState = nullptr;
    lv_obj_t* _lblNodes = nullptr;
    lv_obj_t* _lblPaths = nullptr;
    lv_obj_t* _lblLinks = nullptr;
    lv_obj_t* _lblSummary = nullptr;
    lv_obj_t* _lblLastAnnounce = nullptr;
    lv_obj_t* _btnAnnounce = nullptr;
    lv_obj_t* _lblAnnounceAction = nullptr;

    void toggleAudio();
    void toggleLora();
    void toggleTcp();
    void toggleWiFi();
    void toggleGPS();
    void openPeers();
    void forceRefresh();
    void renderAvatar(const String& seed);
};
