#pragma once

#include "UIManager.h"
#include "runtime/ServiceClient.h"
#include "transport/WiFiInterface.h"
#include "config/UserConfig.h"
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
class AudioNotify;
class Power;

enum class SettingType : uint8_t {
    READONLY,
    INTEGER,
    TOGGLE,
    ENUM_CHOICE,
    ACTION,
    TEXT_INPUT
};

enum class SettingsView : uint8_t {
    CATEGORY_LIST,
    ITEM_LIST,
    WIFI_PICKER
};

struct SettingItem {
    const char* label;
    SettingType type;
    std::function<int()> getter;
    std::function<void(int)> setter;
    std::function<String(int)> formatter;
    int minVal = 0;
    int maxVal = 1;
    int step = 1;
    std::vector<const char*> enumLabels;
    std::function<void()> action;
    std::function<String()> textGetter;
    std::function<void(const String&)> textSetter;
    int maxTextLen = 16;
};

struct SettingsCategory {
    const char* name;
    int startIdx;
    int count;
    std::function<String()> summary;
};

class LvSettingsScreen : public LvScreen {
public:
    void setService(handheld::ServiceClient* service) { _service = service; }
    void createUI(lv_obj_t* parent) override;
    void onEnter() override;
    void refreshUI() override;
    bool handleKey(const KeyEvent& event) override;
    bool handleLongPress() override;

    void setUserConfig(UserConfig* cfg) { _cfg = cfg; }
    void setAudio(AudioNotify* audio) { _audio = audio; }
    void setPower(Power* power) { _power = power; }
    void setBackend(handheld::ProtocolView* backend) { _backend = backend; }
    void setUIManager(UIManager* ui) { _ui = ui; }
    void setIdentityHash(const String& hash) { _identityHash = hash; }
    void setDestinationHash(const String& hash) { _destinationHash = hash; }
    void setShowQrCallback(std::function<void()> cb) { _showQrCb = cb; }

    const char* title() const override { return "Settings"; }
    bool firmwareCheckRunning() const { return _fwCheckState.load() == FirmwareCheckState::RUNNING; }

private:
    handheld::ServiceClient* _service = nullptr;
    void buildItems();
    void applyAndSave();
    void applyPreset(int presetIdx);
    int detectPreset() const;
    bool isEditable(int idx) const;
    void skipToNextEditable(int dir);

    void showCategoryList();
    void showItemList(int catIdx);
    void showWifiPicker();
    void rebuildCategoryList();
    void rebuildItemList();
    void rebuildWifiList();
    void selectWifiResult(int resultIdx);

    void enterCategory(int catIdx);
    void exitToCategories();
    void updateCategorySelection(int oldIdx, int newIdx);
    void updateItemSelection(int oldIdx, int newIdx);
    void updateWifiSelection(int oldIdx, int newIdx);
    bool settingNeedsReboot(const SettingItem& item) const;
    bool categoryNeedsReboot(int catIdx) const;
    bool confirmableAction(const SettingItem& item) const;
    bool armedAction(const SettingItem& item) const;
    bool destructiveAction(const SettingItem& item) const;
    const char* confirmationTitle() const;
    const char* confirmationDetail() const;
    bool hasPendingConfirmation() const;
    void clearConfirmations();
    void runFormatSD();
    void runWipeSD();
    void runFactoryReset();
    void runEnableDevMode();
    void startFirmwareCheck();
    static void firmwareCheckTask(void* arg);

    UserConfig* _cfg = nullptr;
    AudioNotify* _audio = nullptr;
    Power* _power = nullptr;
    handheld::ProtocolView* _backend = nullptr;
    UIManager* _ui = nullptr;
    String _identityHash;
    String _destinationHash;
    std::function<void()> _showQrCb;
    bool _gpsSnapEnabled = true;

    SettingsView _view = SettingsView::CATEGORY_LIST;
    std::vector<SettingsCategory> _categories;
    std::vector<SettingItem> _items;
    int _categoryIdx = 0;
    int _selectedIdx = 0;
    int _catRangeStart = 0;
    int _catRangeEnd = 0;

    // Edit state
    bool _editing = false;
    int _editValue = 0;
    bool _numericTyping = false;   // true once user types digits in INTEGER edit
    bool _textEditing = false;
    String _editText;
    bool _confirmingInitSD = false;
    bool _confirmingWipeSD = false;
    bool _confirmingReset = false;
    bool _confirmingDevMode = false;

    enum class FirmwareCheckState : uint8_t {
        IDLE,
        RUNNING,
        CURRENT,
        AVAILABLE,
        FAILED
    };
    std::atomic<FirmwareCheckState> _fwCheckState{FirmwareCheckState::IDLE};
    TaskHandle_t _fwCheckTask = nullptr;
    char _fwCheckVersion[24] = {};

    // Frequency digit-cursor editor (radio-style)
    bool _freqEditing = false;
    int _freqCursor = 0;         // 0-8, active digit position
    int _freqDigits[9] = {};     // Individual digits of Hz frequency
    int _freqOriginal = 0;       // Original value for Esc cancel
    void freqDecompose(int value);
    int freqRecompose() const;
    String freqFormatWithCursor() const;

    // WiFi picker
    std::vector<WiFiInterface::ScanResult> _wifiResults;
    int _wifiPickerIdx = 0;
    size_t _wifiTargetSlot = 0;
    bool _wifiScanActive = false;

    // Reboot-required tracking
    bool _rebootNeeded = false;
    struct RebootSnapshot {
        RatWiFiMode wifiMode;
        std::vector<WiFiNetwork> wifiSTANetworks;
        uint8_t wifiSTASelected;
        bool autoIfaceEnabled;
        bool sdStorageEnabled;
        bool loraEnabled;
    };
    RebootSnapshot _rebootSnap;
    void snapshotRebootSettings();
    bool loraSettingsChanged() const;
    bool interfaceSettingsChanged() const;
    bool storageSettingsChanged() const;
    bool rebootSettingsChanged() const;

    // TCP change detection
    String _tcpSnapHost;
    uint16_t _tcpSnapPort = 0;
    bool _tcpSnapAuto = false;
    void snapshotTCPSettings();
    bool tcpSettingsChanged() const;

    // Keyboard backlight change detection
    uint8_t _kbBrightness = 0;
    uint32_t _lastIdentityRevision = 0;
    std::vector<std::string> _identityHashes;
    uint32_t _fwViewGeneration = 0, _fwRequestGeneration = 0;
    uint32_t _fwResultGeneration = 0;

    // LVGL widgets
    lv_obj_t* _scrollContainer = nullptr;
    std::vector<lv_obj_t*> _rowObjs;
    lv_obj_t* _editValueLbl = nullptr;  // Cached for in-place updates during text/freq editing
};
