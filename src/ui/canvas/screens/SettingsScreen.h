#pragma once

#include "Screen.h"
#include "widgets/ScrollList.h"
#include "widgets/TextInput.h"
#include "config/UserConfig.h"
#include "config/SettingsTransaction.h"
#include "storage/FlashStore.h"
#include "storage/SDStore.h"
#include "radio/RadioSettings.h"
#include <WiFi.h>
#include "audio/AudioNotify.h"
#include "power/PowerManager.h"
#include "runtime/ScanResult.h"
#include "transport/TCPClientInterface.h"
#include "protocol/ProtocolBackend.h"
#include "runtime/ServiceMessages.h"
#include <vector>

class SettingsScreen : public Screen {
public:
    void render(M5Canvas& canvas) override;
    bool handleKey(const KeyEvent& event) override;
    const char* title() const override { return "Settings"; }
    void onEnter() override;

    void setUserConfig(UserConfig* cfg) { _config = cfg; _candidateReady = cfg && _candidate.tryAssign(*cfg); _candidateDirty = false; }
    void setFlashStore(FlashStore* flash) { _flash = flash; }
    void setSDStore(SDStore* sd) { _sdStore = sd; }
    using RadioApplyCallback = RadioApply (*)(const UserSettings&, bool);
    void setRadioApply(RadioApplyCallback callback) { _radioApply = callback; }
    bool radioApplyPending() const { return _radioApplyPending; }
    bool pollRadioApply(bool accepting); // Global owner loop, including while hidden.
    void setAudio(AudioNotify* audio) { _audio = audio; }
    void setPower(PowerManager* power) { _power = power; }
    struct NetworkActions {
        bool (*startScan)() = nullptr;
        handheld::ScanResult (*finishScan)(String&) = nullptr;
        bool (*connect)() = nullptr;
        void (*disconnect)() = nullptr;
        bool (*connecting)() = nullptr;
    };
    void setNetworkActions(NetworkActions actions) { _network = actions; }
    bool pollNetworkResults(); // Refresh connection controls; retire scans even while hidden.
    void setBackend(ProtocolBackend* backend) { _backend = backend; }
    void setIdentityHash(const String& hash) { _identityHash = hash; }
    using SaveCallback = std::function<SettingsTransaction::Result(UserConfig&)>;
    void setSaveCallback(SaveCallback cb) { _saveCb = cb; }
    // Called once after an initially pending transaction settles on the board.
    void applyCommitted(bool refreshCandidate = true);
    void setMaintenanceCallback(std::function<bool(handheld::Operation)> cb) { _maintenanceCb = cb; }

    void setMaintenanceBlockReason(const char* reason) { _maintenanceReason=reason; }

    // Callback for back navigation
    using BackCallback = std::function<void()>;
    void setBackCallback(BackCallback cb) { _backCb = cb; }

private:
    enum SubMenu { MENU_MAIN, MENU_RADIO, MENU_WIFI, MENU_TCP, MENU_SDCARD,
                   MENU_DISPLAY, MENU_AUDIO, MENU_ABOUT, MENU_WIFI_SCAN,
                   MENU_PROPAGATION, MENU_PROPAGATION_CHOICE, MENU_PROPAGATION_NODES, MENU_VOICE };

    void buildPropagationMenu();
    void buildVoiceMenu();
    void activateVoiceRow(int);
    void showPropagationChoice();
    void showPropagationNodes();
    void activatePropagationRow(int row);
    void pollPropagationUI();
    bool savePropagationAddress(const uint8_t address[16]);
    handheld::propagation::NodeView _propNodes[handheld::propagation::NodeViewCapacity]{};
    size_t _propCount = 0;
    bool _propChoicePending = false;
    handheld::propagation::SyncStatus _propStatus = handheld::propagation::SyncStatus::Off;

    void buildMainMenu();
    void buildRadioMenu();
    void buildWiFiMenu();
    void buildTCPMenu();
    void buildSDCardMenu();
    void sdCardFormat();
    void buildDisplayMenu();
    void buildAudioMenu();
    void renderAbout(M5Canvas& canvas);

    // WiFi scanner
    void startWiFiScan();
    void buildScanResultsMenu();
    void selectNetwork(int index);
    void disconnectWiFi();
    void connectWiFi();
    enum class WiFiAction : uint8_t { None, Connect, Cancel, Disconnect };
    WiFiAction currentWiFiAction() const;
    bool pollWiFiStatus();
    void activateWiFiAction();

    void addTCPConnection(const std::string& host, uint16_t port);
    void toggleTCPConnection(int index);
    void removeTCPConnection(int index);

    void startEditing(int field, const std::string& currentValue);
    void commitEdit(const std::string& value);
    std::string getCurrentValue(SubMenu menu, int field);
    bool applyAndSave();
    void applyRadioPreset(int preset);
    void finishRadioApply(RadioApply result);
    void factoryReset();
    void requestMaintenance(handheld::Operation operation);
    void showToast(const char* msg, unsigned long durationMs = 1500);

    UserConfig* _config = nullptr;
    UserConfig _candidate;
    bool _candidateReady = false, _candidateDirty = false;
    FlashStore* _flash = nullptr;
    SDStore* _sdStore = nullptr;
    RadioApplyCallback _radioApply = nullptr;
    bool _radioApplyPending = false, _presetAnnouncePending = false;
    AudioNotify* _audio = nullptr;
    PowerManager* _power = nullptr;
    NetworkActions _network;
    WiFiAction _wifiAction = WiFiAction::None;
    WiFiAction _drawnWiFiAction = WiFiAction::None; // Gate activation until the label is drawn.
    bool _scanPending = false;
    handheld::ScanResult _scanOutcome = handheld::ScanResult::Ready;
    ProtocolBackend* _backend = nullptr;
    String _identityHash;

    SubMenu _subMenu = MENU_MAIN;
    ScrollList _list;
    bool _editing = false;
    TextInput _editInput;
    int _editField = -1;
    std::string _tcpPendingHost;
    std::string _editLabel;
    BackCallback _backCb;
    SaveCallback _saveCb;
    std::function<bool(handheld::Operation)> _maintenanceCb;
    const char* _maintenanceReason = "Maintenance unavailable";

    // WiFi scan state
    struct WiFiNetwork { String ssid; int32_t rssi; uint8_t encType; };
    std::vector<WiFiNetwork> _scanResults;

    // Toast overlay
    unsigned long _toastUntil = 0;
    const char* _toastMessage = nullptr;

    // Confirmation dialog state
    bool _confirmPending = false;
    int _confirmAction = 0;  // 0=factory reset, 1=SD wipe
};
